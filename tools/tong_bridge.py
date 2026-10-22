#!/usr/bin/env python3
"""Tong MCP bridge: connects MCP clients (Claude Desktop, Claude Code, ...)
to the Tong robot over its USB serial port.

The robot itself is the MCP server: it defines the tools and answers every
request. This script only carries messages between a client (on this
script's stdin/stdout) and the robot's serial port, which it shares with the
robot's log output.

Several clients at once: Claude Desktop starts one copy of this script for
chats and more for Cowork and Code sessions, and a serial port can only have
one reader. So the first copy becomes the hub: it opens the port and listens
on 127.0.0.1 (port 47333). Later copies connect to the hub instead of the
port. The hub gives every request a unique id on the way to the robot and
restores the client's own id on the way back, so replies never cross. If the
hub's client goes away, another copy takes over within a second or so.

The hub also:
  - finds the robot (USB-serial chips it knows, confirmed with a ping), or
    uses --port; opens the port without resetting the chip
  - pings the robot every 10 s, which tells the robot a host is attached
    (its buttons switch to their online functions)
  - writes everything the robot prints (its logs) and a one-line summary of
    every request and reply to a log file you can watch live
  - keeps running when the robot is unplugged or restarts, reconnects on its
    own, and answers requests it cannot deliver with a clear error

Usage:
    python3 tong_bridge.py [--port PORT] [--baud 115200] [--log FILE] [--hub-port 47333]
    python3 tong_bridge.py --check      # find the robot, list its tools, exit
    python3 tong_bridge.py --list       # show candidate serial ports, exit

PORT is a serial port (/dev/ttyACM0, COM3) or a pyserial URL such as
socket://192.168.4.1:3333. --hub-port 0 turns sharing off. Needs pyserial
(Ubuntu: sudo apt install python3-serial; elsewhere: pip install pyserial).
Python 3.8 or newer.

Watch the log live:  tail -f <log file>   (Windows: Get-Content -Wait <log file>)
The log file path is printed on stderr at start-up.
"""
import argparse
import itertools
import json
import os
import random
import socket
import sys
import tempfile
import threading
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.stderr.write("tong_bridge: pyserial is missing (Ubuntu: sudo apt install python3-serial; "
                     "otherwise: pip install pyserial)\n")
    sys.exit(2)

VERSION = "0.2.0"

# USB-serial chips the robot may sit behind, tried in this order
KNOWN_CHIPS = {
    (0x1A86, 0x55D3): "CH343",
    (0x1A86, 0x7523): "CH340",
    (0x10C4, 0xEA60): "CP210x",
    (0x303A, 0x1001): "ESP32 USB",
}

PROTOCOL_PREFIX = b'{"jsonrpc":"2.0"'
INTERNAL_ID = "tong-bridge-"        # ids of the bridge's own requests (probe, heartbeat)
LINE_MAX = 8192                     # the robot drops longer messages
REQUEST_TIMEOUT = 5.0               # seconds to wait for a reply
STARTUP_WAIT = 8.0                  # how long early requests wait for the first connection
PROBE_TIMEOUT = 1.5
RETRY_EVERY = 1.0
HEARTBEAT_EVERY = float(os.environ.get("TONG_BRIDGE_HEARTBEAT_S", "10"))   # override only for tests
HUB_PORT = 47333
HELLO = {"tong-bridge-hello": VERSION}
LOG_ROTATE_BYTES = 5 * 1024 * 1024

ERR_INTERNAL = -32603


# ---------------------------------------------------------------- log file

class Log:
    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        try:
            if os.path.getsize(path) > LOG_ROTATE_BYTES:
                os.replace(path, path + ".1")
        except OSError:
            pass
        self.f = open(path, "a", encoding="utf-8", buffering=1)

    def write(self, tag, text):
        t = time.time()
        stamp = time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % int((t % 1) * 1000)
        with self.lock:
            self.f.write("%s  %-6s %s\n" % (stamp, tag, text))


def default_log_path():
    return os.path.join(tempfile.gettempdir(), "tong-bridge.log")


def short(obj, limit=160):
    s = json.dumps(obj, ensure_ascii=False, separators=(",", ":"))
    return s if len(s) <= limit else s[:limit - 3] + "..."


def dumps(obj):
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")


def error_reply(rid, message, method):
    """A request we cannot deliver: a tool call gets a readable tool error the
    model can act on, anything else a JSON-RPC error."""
    if method == "tools/call":
        return {"jsonrpc": "2.0", "id": rid, "result": {
            "content": [{"type": "text", "text": message}], "isError": True}}
    return {"jsonrpc": "2.0", "id": rid, "error": {"code": ERR_INTERNAL, "message": message}}


def describe(msg):
    method = msg.get("method", "?")
    params = msg.get("params") or {}
    if method == "tools/call":
        return "tools/call %s %s" % (params.get("name"), short(params.get("arguments", {}), 200))
    if method == "initialize":
        ci = params.get("clientInfo") or {}
        return "initialize (%s) from %s %s" % (params.get("protocolVersion"), ci.get("name"), ci.get("version"))
    meta = params.get("_meta") or {}
    ver = meta.get("io.modelcontextprotocol/protocolVersion")
    return method + (" (%s)" % ver if ver else "")


def read_line(sock, buf, limit=4 * LINE_MAX):
    """(line, rest) from a socket, or (None, rest) on EOF. Raises socket.timeout."""
    while b"\n" not in buf:
        data = sock.recv(65536)
        if not data:
            return None, buf
        buf += data
        if len(buf) > limit and b"\n" not in buf:
            raise ValueError("line too long")
    line, buf = buf.split(b"\n", 1)
    return line.strip(b"\r"), buf


# ---------------------------------------------------------------- the robot side

class Robot:
    """Owns the serial port: finds and opens the robot, reads its lines,
    reconnects when it goes away, pings it now and then."""

    def __init__(self, port, baud, log, on_message, on_lost):
        self.port_arg = port
        self.baud = baud
        self.log = log
        self.on_message = on_message      # protocol messages for the clients
        self.on_lost = on_lost            # called when the link drops or the robot restarts
        self.ser = None
        self.name = None
        self.wlock = threading.Lock()
        self.connected = threading.Event()
        self.stopping = False
        self.ids = itertools.count(1)
        self.last_problem = None

    # ---- finding and opening

    def candidates(self):
        if self.port_arg:
            return [(self.port_arg, self.port_arg)]
        found = []
        for p in list_ports.comports():
            chip = KNOWN_CHIPS.get((p.vid, p.pid))
            if chip:
                found.append((list(KNOWN_CHIPS).index((p.vid, p.pid)), p.device, "%s (%s)" % (p.device, chip)))
        return [(dev, label) for _, dev, label in sorted(found)]

    def open(self, url):
        is_url = url.startswith(("socket://", "rfc2217://", "loop://"))
        ser = serial.serial_for_url(url, do_not_open=True, baudrate=self.baud, timeout=0.1, write_timeout=2)
        if not is_url:
            # Only one reader per port: lock it (flock on Linux; Windows is always exclusive).
            ser.exclusive = True
            # DTR/RTS drive the board's auto-reset circuit: EN (reset) goes low
            # only while RTS is asserted and DTR is not. The OS asserts both on
            # open; asking for both asserted means pyserial (which sets DTR,
            # then RTS) never passes through that state, so connecting does not
            # reset the chip. From both released, DTR first only touches IO0.
            ser.dtr = True
            ser.rts = True
        ser.open()
        return ser

    def problem(self, text):
        if text != self.last_problem:
            self.log.write("bridge", text)
            self.last_problem = text

    def probe(self, ser):
        """True if an MCP server answers a ping on `ser`. Log lines seen meanwhile are logged."""
        pid = "%sprobe-%d" % (INTERNAL_ID, next(self.ids))
        ser.write(b'{"jsonrpc":"2.0","id":"' + pid.encode() + b'","method":"ping"}\n')
        deadline = time.monotonic() + PROBE_TIMEOUT
        buf = b""
        while time.monotonic() < deadline:
            buf += ser.read(ser.in_waiting or 1)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip(b"\r")
                if line.startswith(PROTOCOL_PREFIX):
                    try:
                        if json.loads(line).get("id") == pid:
                            return True
                    except ValueError:
                        pass
                elif line:
                    self.log.write("dev", line.decode("utf-8", "replace"))
        return False

    def connect_once(self):
        cands = self.candidates()
        if not cands:
            self.problem("no robot found: no %s on any USB port (plugged in?)" % "/".join(sorted(set(KNOWN_CHIPS.values()))))
            return False
        for url, label in cands:
            try:
                ser = self.open(url)
            except (OSError, serial.SerialException, ValueError) as e:
                hint = ""
                msg = str(e)
                if any(w in msg.lower() for w in ("busy", "denied", "lock", "permission")):
                    hint = " (is idf.py monitor or another program using it? on Linux, is your user in the dialout group?)"
                self.problem("cannot open %s: %s%s" % (label, msg, hint))
                continue
            try:
                ok = self.probe(ser)
            except Exception as e:
                ok = False
                self.problem("lost %s while checking it: %s" % (label, e or type(e).__name__))
            if ok:
                with self.wlock:
                    self.ser, self.name = ser, label
                    self.connected.set()
                self.last_problem = None
                self.log.write("bridge", "connected to %s at %d baud" % (label, self.baud))
                return True
            ser.close()
            self.problem("%s does not answer MCP (Tong firmware with mcp not flashed? still booting?)" % label)
        return False

    # ---- running

    def run(self):
        while not self.stopping:
            if not self.connect_once():
                time.sleep(RETRY_EVERY)
                continue
            self.read_loop()
            if not self.stopping:
                time.sleep(0.2)

    def heartbeat(self):
        """Tells the robot a host is here (it counts as online for ~25 s after each)."""
        while not self.stopping:
            time.sleep(HEARTBEAT_EVERY)
            if self.connected.is_set():
                self.send(b'{"jsonrpc":"2.0","id":"' + ("%sheartbeat-%d" % (INTERNAL_ID, next(self.ids))).encode()
                          + b'","method":"ping"}')

    def read_loop(self):
        buf = b""
        while not self.stopping:
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception as e:      # backends differ: SerialException, OSError, even AttributeError once closed
                if not self.stopping:
                    self.drop("lost the robot: %s" % (e or type(e).__name__))
                return
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self.handle_line(line.strip(b"\r"))
            if len(buf) > 4 * LINE_MAX:       # no newline for a long time: flush it as log text
                self.log.write("dev", buf.decode("utf-8", "replace"))
                buf = b""

    def handle_line(self, line):
        if not line:
            return
        if not line.startswith(PROTOCOL_PREFIX):
            self.log.write("dev", line.decode("utf-8", "replace"))
            return
        try:
            msg = json.loads(line)
        except ValueError:
            self.log.write("dev", "(garbled protocol line) " + line.decode("utf-8", "replace"))
            return
        if msg.get("method") == "tong/ready":
            info = msg.get("params") or {}
            self.log.write("bridge", "robot (re)started: %s %s" % (info.get("name", "?"), info.get("version", "?")))
            self.on_lost("Tong restarted before answering")
            return
        mid = msg.get("id")
        if isinstance(mid, str) and mid.startswith(INTERNAL_ID):
            return                          # our own probe or heartbeat
        self.on_message(msg)

    def drop(self, why):
        """Called by the reader thread only."""
        self.log.write("bridge", why)
        with self.wlock:
            self.connected.clear()
            ser, self.ser = self.ser, None
        try:
            ser.close()
        except Exception:
            pass
        self.on_lost("Tong was disconnected")

    def send(self, line):
        """Send one message line; False if the robot is not connected."""
        with self.wlock:
            if not self.connected.is_set():
                return False
            try:
                self.ser.write(line + b"\n")
                return True
            except Exception as e:
                # Closing the port makes the reader thread notice and run drop().
                self.log.write("bridge", "write failed: %s" % e)
                self.connected.clear()
                try:
                    self.ser.close()
                except Exception:
                    pass
                return False

    def stop(self):
        self.stopping = True
        with self.wlock:
            self.connected.clear()
            ser = self.ser
        try:
            if ser:
                ser.close()
        except Exception:
            pass


# ---------------------------------------------------------------- clients of the hub

class Endpoint:
    """One MCP client of the hub: this process's own stdio, or another copy of the bridge."""

    def __init__(self, n, write, label):
        self.n = n
        self.label = label
        self._write = write
        self.lock = threading.Lock()

    def send(self, raw):
        try:
            with self.lock:
                self._write(raw)
        except OSError:
            pass


class Hub:
    """Owns the robot; serves this process's client and other bridge copies."""

    def __init__(self, args, log, local_write):
        self.log = log
        self.listener = None
        if args.hub_port:
            ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if os.name == "nt":
                ls.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            else:
                ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                ls.bind(("127.0.0.1", args.hub_port))
                ls.listen(8)
            except OSError:
                ls.close()
                raise
            self.listener = ls
        self.pending = {}                   # wire id -> (endpoint, client's id, method, sent at)
        self.plock = threading.Lock()
        self.wire_ids = itertools.count(1)
        self.client_numbers = itertools.count(1)
        self.endpoints = set()
        self.started = time.monotonic()
        self.closed = False
        self.local = Endpoint(0, local_write, "this client")
        self.robot = Robot(args.port, args.baud, log, self.from_robot, self.fail_all)
        for target in (self.robot.run, self.robot.heartbeat, self.expiry_loop):
            threading.Thread(target=target, daemon=True).start()
        if self.listener:
            threading.Thread(target=self.accept_loop, daemon=True).start()

    # ---- requests in

    def submit_local(self, raw):
        self.submit(self.local, raw)

    def submit(self, ep, raw):
        raw = raw.strip(b"\r\n")
        if not raw:
            return
        tag = "->" if ep.n == 0 else "->[%d]" % ep.n
        try:
            msg = json.loads(raw)
        except ValueError:
            self.log.write(tag, "(not JSON, ignored) " + raw[:200].decode("utf-8", "replace"))
            return
        if not isinstance(msg, dict) or "method" not in msg:
            self.log.write(tag, "(ignored) " + short(msg))
            return
        rid = msg.get("id")
        if rid is None:                     # a notification: pass it on, nothing comes back
            self.log.write(tag, describe(msg))
            self.robot.send(raw)
            return
        self.log.write(tag, "#%s %s" % (rid, describe(msg)))
        method = msg["method"]
        if len(raw) > LINE_MAX:
            self.reply_error(ep, rid, "Request too large for Tong (%d bytes, at most %d)." % (len(raw), LINE_MAX), method)
            return
        # early requests wait for the first connection instead of failing at once
        if not self.robot.connected.is_set():
            self.robot.connected.wait(max(0.0, STARTUP_WAIT - (time.monotonic() - self.started)))
        wire = "c%d.%d" % (ep.n, next(self.wire_ids))
        msg["id"] = wire
        with self.plock:
            self.pending[wire] = (ep, rid, method, time.monotonic())
        if not self.robot.send(dumps(msg)):
            with self.plock:
                self.pending.pop(wire, None)
            self.reply_error(ep, rid, "Tong is not connected (plugged in? see the bridge log: %s)." % self.log.path, method)

    # ---- replies out

    def reply_error(self, ep, rid, message, method):
        ep.send(dumps(error_reply(rid, message, method)))
        self.log.write("<-" if ep.n == 0 else "<-[%d]" % ep.n, "#%s error: %s" % (rid, message))

    def from_robot(self, msg):
        wire = msg.get("id")
        with self.plock:
            entry = self.pending.pop(wire, None) if isinstance(wire, str) else None
        if entry is None:
            self.log.write("<-", "(no request waiting for id %s, dropped) %s" % (json.dumps(wire), short(msg)))
            return
        ep, rid, method, sent = entry
        msg["id"] = rid
        ep.send(dumps(msg))
        tag = "<-" if ep.n == 0 else "<-[%d]" % ep.n
        ms = int((time.monotonic() - sent) * 1000)
        if "error" in msg:
            self.log.write(tag, "#%s %s error %s: %s" % (rid, method, msg["error"].get("code"), msg["error"].get("message")))
            return
        result = msg.get("result") or {}
        detail = ""
        if method == "tools/call":
            texts = [c.get("text", "") for c in result.get("content", []) if isinstance(c, dict)]
            detail = (" ERROR" if result.get("isError") else "") + ": " + " ".join(texts)[:200]
        elif method == "tools/list":
            detail = ": %d tools" % len(result.get("tools", []))
        self.log.write(tag, "#%s %s ok %d ms%s" % (rid, method, ms, detail))

    def fail_all(self, why, only=None):
        with self.plock:
            items = [(w, e) for w, e in self.pending.items() if only is None or e[0] is only]
            for w, _ in items:
                del self.pending[w]
        for _, (ep, rid, method, _) in items:
            self.reply_error(ep, rid, "%s; please try again." % why, method)

    def expiry_loop(self):
        while not self.closed:
            time.sleep(0.25)
            now = time.monotonic()
            with self.plock:
                late = [(w, e) for w, e in self.pending.items() if now - e[3] > REQUEST_TIMEOUT]
                for w, _ in late:
                    del self.pending[w]
            for _, (ep, rid, method, _) in late:
                self.reply_error(ep, rid, "Tong did not answer within %d s." % REQUEST_TIMEOUT, method)

    # ---- other bridge copies

    def accept_loop(self):
        while not self.closed:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(conn,), daemon=True).start()

    def serve(self, conn):
        buf = b""
        try:
            conn.settimeout(2.0)
            line, buf = read_line(conn, buf)
            if line is None or "tong-bridge-hello" not in json.loads(line):
                conn.close()
                return
            conn.sendall(dumps({"tong-bridge-hub": VERSION}) + b"\n")
            conn.settimeout(None)
        except (OSError, ValueError):
            conn.close()
            return
        n = next(self.client_numbers)
        ep = Endpoint(n, lambda raw: conn.sendall(raw + b"\n"), "bridge client %d" % n)
        self.endpoints.add(conn)
        self.log.write("bridge", "client [%d] joined (another MCP client sharing this robot)" % n)
        try:
            while True:
                line, buf = read_line(conn, buf)
                if line is None:
                    break
                self.submit(ep, line)
        except (OSError, ValueError):
            pass
        self.endpoints.discard(conn)
        self.fail_all("client left", only=ep)
        self.log.write("bridge", "client [%d] left" % n)
        try:
            conn.close()
        except OSError:
            pass

    def close(self):
        # Free the serial port first, so the copy that takes over can open it at once.
        self.closed = True
        self.robot.stop()
        if self.listener:
            try:
                self.listener.close()
            except OSError:
                pass
        for conn in list(self.endpoints):
            try:
                conn.shutdown(socket.SHUT_RDWR)
                conn.close()
            except OSError:
                pass


class Remote:
    """This process's client, served through the hub in another bridge copy."""

    def __init__(self, port, log, local_write, on_lost):
        self.log = log
        self.local_write = local_write
        self.on_lost = on_lost
        self.pending = {}                   # client's id (as JSON) -> method
        self.plock = threading.Lock()
        self.wlock = threading.Lock()
        self.lost = False
        sock = socket.create_connection(("127.0.0.1", port), timeout=1.0)
        try:
            sock.sendall(dumps(HELLO) + b"\n")
            line, self.buf = read_line(sock, b"")
            if line is None or "tong-bridge-hub" not in json.loads(line):
                raise ConnectionError("not a Tong bridge hub")
        except (OSError, ValueError):
            sock.close()
            raise ConnectionError("no Tong bridge hub on port %d" % port)
        sock.settimeout(None)
        self.sock = sock
        threading.Thread(target=self.read_loop, daemon=True).start()

    def submit_local(self, raw):
        raw = raw.strip(b"\r\n")
        if not raw:
            return
        rid = method = None
        try:
            msg = json.loads(raw)
            if isinstance(msg, dict):
                rid, method = msg.get("id"), msg.get("method")
        except ValueError:
            pass
        if rid is not None:
            with self.plock:
                self.pending[json.dumps(rid)] = method
        try:
            with self.wlock:
                self.sock.sendall(raw + b"\n")
        except OSError:
            self.lose()

    def read_loop(self):
        buf = self.buf
        try:
            while True:
                line, buf = read_line(self.sock, buf)
                if line is None:
                    break
                try:
                    rid = json.loads(line).get("id")
                    with self.plock:
                        self.pending.pop(json.dumps(rid), None)
                except (ValueError, AttributeError):
                    pass
                self.local_write(line)
        except (OSError, ValueError):
            pass
        self.lose()

    def lose(self):
        with self.plock:
            if self.lost:
                return
            self.lost = True
            items = list(self.pending.items())
            self.pending.clear()
        for key, method in items:
            self.local_write(dumps(error_reply(json.loads(key), "Tong's bridge was handing over; please try again.", method)))
        try:
            self.sock.close()
        except OSError:
            pass
        self.on_lost()

    def close(self):
        self.lost = True
        try:
            self.sock.close()
        except OSError:
            pass


class Switchboard:
    """Connects this process's client to the robot: through an existing hub,
    or by becoming the hub. Takes over when the hub goes away."""

    def __init__(self, args, log, local_write):
        self.args = args
        self.log = log
        self.local_write = local_write
        self.router = None
        self.lock = threading.Lock()
        self.closing = False

    def elect(self):
        for attempt in range(200):
            if self.closing:
                return
            if self.args.hub_port:
                try:
                    router = Remote(self.args.hub_port, self.log, self.local_write, self.on_remote_lost)
                    self.log.write("bridge", "sharing the robot through the bridge hub on port %d" % self.args.hub_port)
                    with self.lock:
                        self.router = router
                    return
                except (OSError, ConnectionError):
                    pass
            try:
                router = Hub(self.args, self.log, self.local_write)
                self.log.write("bridge", "serving as the hub (owns the serial port%s)" %
                               (", shares it on 127.0.0.1:%d" % self.args.hub_port if self.args.hub_port else ""))
                with self.lock:
                    self.router = router
                return
            except OSError:
                time.sleep(0.1 + random.random() * 0.3)     # someone else just became the hub
        self.log.write("bridge", "could not reach or become the hub on port %d" % self.args.hub_port)

    def on_remote_lost(self):
        with self.lock:
            self.router = None
        if not self.closing:
            self.log.write("bridge", "the hub went away; taking over")
            threading.Thread(target=self.elect, daemon=True).start()

    def submit(self, raw):
        router = None
        for _ in range(40):                 # a hand-over takes well under 2 s
            with self.lock:
                router = self.router
            if router is not None or self.closing:
                break
            time.sleep(0.05)
        if router is None:
            try:
                msg = json.loads(raw)
                if isinstance(msg, dict) and msg.get("id") is not None:
                    self.local_write(dumps(error_reply(msg["id"], "Tong's bridge is restarting; please try again.", msg.get("method"))))
            except ValueError:
                pass
            return
        router.submit_local(raw)

    def close(self):
        self.closing = True
        with self.lock:
            router = self.router
        if router:
            router.close()


# ---------------------------------------------------------------- command line

def stdout_writer():
    lock = threading.Lock()

    def write(raw):
        with lock:
            sys.stdout.buffer.write(raw + b"\n")
            sys.stdout.buffer.flush()
    return write


def run(args):
    log = Log(args.log)
    log.write("bridge", "tong_bridge %s started (pid %d), log %s" % (VERSION, os.getpid(), log.path))
    sys.stderr.write("tong_bridge: log file %s\n" % log.path)
    sys.stderr.flush()
    board = Switchboard(args, log, stdout_writer())
    board.elect()
    try:
        for raw in iter(sys.stdin.buffer.readline, b""):
            board.submit(raw)
    except KeyboardInterrupt:
        pass
    log.write("bridge", "client closed the connection; exiting (pid %d)" % os.getpid())
    board.close()
    return 0


def check(args):
    """Find the robot (through a running hub if there is one), list its tools, exit."""
    log = Log(args.log)
    replies = {}
    done = threading.Event()

    def capture(raw):
        try:
            msg = json.loads(raw)
            replies[msg.get("id")] = msg
            if "c1" in replies and "c2" in replies:
                done.set()
        except ValueError:
            pass

    print("looking for Tong ...")
    board = Switchboard(args, log, capture)
    board.elect()
    board.submit(b'{"jsonrpc":"2.0","id":"c1","method":"initialize","params":{"protocolVersion":"2025-11-25",'
                 b'"capabilities":{},"clientInfo":{"name":"tong_bridge --check","version":"' + VERSION.encode() + b'"}}}')
    board.submit(b'{"jsonrpc":"2.0","id":"c2","method":"tools/list"}')
    ok = done.wait(STARTUP_WAIT + REQUEST_TIMEOUT)
    board.close()
    if not ok or "result" not in replies.get("c1", {}) or "result" not in replies.get("c2", {}):
        why = (replies.get("c1") or replies.get("c2") or {}).get("error", {}).get("message", "no answer")
        print("Tong not reached:", why)
        print("details in", log.path)
        return 1
    info = replies["c1"]["result"]["serverInfo"]
    print("server: %s %s" % (info.get("name"), info.get("version")))
    for t in replies["c2"]["result"]["tools"]:
        print("  %-18s %s" % (t["name"], t.get("title", "")))
    return 0


def main():
    ap = argparse.ArgumentParser(description="MCP bridge for the Tong robot (stdio <-> serial).")
    ap.add_argument("--port", help="serial port or pyserial URL (default: find the robot)")
    ap.add_argument("--baud", type=int, default=115200, help="console baud rate (default 115200)")
    ap.add_argument("--log", default=default_log_path(), help="log file (default %(default)s)")
    ap.add_argument("--hub-port", type=int, default=HUB_PORT,
                    help="local port for sharing the robot between clients (default %(default)s, 0 = off)")
    ap.add_argument("--check", action="store_true", help="find the robot, list its tools, exit")
    ap.add_argument("--list", action="store_true", help="list candidate serial ports, exit")
    args = ap.parse_args()

    if args.list:
        for p in list_ports.comports():
            chip = KNOWN_CHIPS.get((p.vid, p.pid))
            vidpid = "%04x:%04x" % (p.vid, p.pid) if p.vid is not None else "-"
            print("%-16s %s  %s%s" % (p.device, vidpid, p.description, ("  <- " + chip) if chip else ""))
        return 0
    if args.check:
        return check(args)
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
