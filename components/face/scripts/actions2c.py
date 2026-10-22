#!/usr/bin/env python3
"""Convert the face action library (actions.json) into C tables.

Usage:
    actions2c.py <actions.json> <avatar.json> <out_dir>

Writes face_action_data.h and face_action_data.c into <out_dir>. The avatar
file is only read to resolve expression names, so an action that borrows an
expression the avatar does not have stops the build here. The face
component's CMakeLists.txt runs this automatically at build time.

Errors stop the build. Warnings (motion faster than the servos, offsets past
their limits) do not: the screen draws whatever the servos cannot.
"""
import json
import os
import re
import sys

SCHEMA = "tong/face-actions"
EASE = {"smooth": "FACE_TRANSITION_SMOOTH", "snappy": "FACE_TRANSITION_SNAPPY", "spring": "FACE_TRANSITION_SPRING"}
# peak speed of each ease relative to a linear move over the same time
EASE_PEAK = {"smooth": 1.5, "snappy": 3.0, "spring": 6.0}
AXES = ("yaw", "pitch", "roll")
MAX_KEYS = 32

SERVO_DEG_PER_S = 300.0                 # the servo component's speed limit
YAW_LIMIT = 35.0                        # servo limits, REP-103 degrees
PITCH_DOWN, PITCH_UP = 18.0, -90.0
ROLL_WARN = 25.0                        # screen only; past this the face looks odd

warnings = []


def fail(msg):
    sys.exit("actions2c: error: " + msg)


def warn(msg):
    warnings.append(msg)


def num(value, where):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        fail(f"{where}: expected a number, got {value!r}")
    return float(value)


def ms(value, where):
    v = num(value, where)
    if v < 0 or v > 65535 or v != int(v):
        fail(f"{where}: expected whole ms in 0..65535, got {value!r}")
    return int(v)


def cf(x):
    s = f"{x:.9g}"
    if s == "-0":
        s = "0"
    if "e" not in s and "." not in s:
        s += ".0"
    return s + "f"


def parse(actions_path, avatar_path):
    with open(avatar_path, encoding="utf-8") as fp:
        avatar = json.load(fp)
    expr_index = {k: i for i, k in enumerate(avatar["expressionOrder"])}

    with open(actions_path, encoding="utf-8") as fp:
        doc = json.load(fp)
    if doc.get("schema") != SCHEMA or doc.get("version") != 1:
        fail(f"{actions_path}: not a {SCHEMA} v1 file")

    out, seen = [], set()
    for ai, a in enumerate(doc["actions"]):
        name = a.get("name")
        w = f"actions[{ai}]"
        if not isinstance(name, str) or not re.fullmatch(r"[a-z][a-z0-9_]{0,31}", name):
            fail(f"{w}: name must be lowercase letters, digits and _, got {name!r}")
        w = f"action '{name}'"
        if name in seen:
            fail(f"{w}: defined twice")
        seen.add(name)
        desc = a.get("description")
        if not isinstance(desc, str) or not desc.strip() or len(desc) > 200 or '"' in desc or "\\" in desc:
            fail(f"{w}: needs a one-line description (no quotes or backslashes, at most 200 characters)")

        keys = a.get("keys")
        if not isinstance(keys, list) or not 2 <= len(keys) <= MAX_KEYS:
            fail(f"{w}: needs 2..{MAX_KEYS} keys")
        parsed = []
        for ki, k in enumerate(keys):
            wk = f"{w} key {ki}"
            extra = set(k) - {"t", "ease", *AXES}
            if extra:
                fail(f"{wk}: unknown field(s) {sorted(extra)}")
            ease = k.get("ease", "smooth")
            if ease not in EASE:
                fail(f"{wk}: ease must be one of {', '.join(EASE)}, got {ease!r}")
            parsed.append({"t": ms(k.get("t"), f"{wk}.t"), "ease": ease,
                           "off": tuple(num(k.get(ax, 0), f"{wk}.{ax}") for ax in AXES)})
        if parsed[0]["t"] != 0 or any(parsed[0]["off"]):
            fail(f"{w}: the first key must be t=0 with no offset")
        if any(parsed[-1]["off"]):
            fail(f"{w}: the last key must bring every axis back to 0")
        for k0, k1 in zip(parsed, parsed[1:]):
            if k1["t"] <= k0["t"]:
                fail(f"{w}: key times must increase (t={k0['t']} then t={k1['t']})")

        # what the servos can do; the screen covers the rest
        for k0, k1 in zip(parsed, parsed[1:]):
            dt = (k1["t"] - k0["t"]) / 1000.0
            for ax in (0, 1):
                speed = abs(k1["off"][ax] - k0["off"][ax]) / dt * EASE_PEAK[k1["ease"]]
                if speed > SERVO_DEG_PER_S:
                    warn(f"{w}: {AXES[ax]} peaks at ~{speed:.0f} deg/s between t={k0['t']} and t={k1['t']} "
                         f"(servos: {SERVO_DEG_PER_S:.0f}); the screen will make up the lag")
        for k in parsed:
            yaw, pitch, roll = k["off"]
            if abs(yaw) > YAW_LIMIT:
                warn(f"{w}: yaw {yaw:g} at t={k['t']} is past the servo limit ({YAW_LIMIT:g})")
            if pitch > PITCH_DOWN or pitch < PITCH_UP:
                warn(f"{w}: pitch {pitch:g} at t={k['t']} is past the servo range ({PITCH_UP:g}..{PITCH_DOWN:g})")
            if abs(roll) > ROLL_WARN:
                warn(f"{w}: roll {roll:g} at t={k['t']} is a lot for the screen")

        expr = {"index": -1, "name": None, "in": 0, "until": 0, "out": 0}
        e = a.get("expression")
        if e is not None:
            we = f"{w} expression"
            if e.get("name") not in expr_index:
                fail(f"{we}: '{e.get('name')}' is not an expression of {os.path.basename(avatar_path)}")
            expr = {"index": expr_index[e["name"]], "name": e["name"],
                    "in": ms(e.get("in_ms", 300), f"{we}.in_ms"),
                    "until": ms(e.get("until_ms", parsed[-1]["t"]), f"{we}.until_ms"),
                    "out": ms(e.get("out_ms", 300), f"{we}.out_ms")}
            if expr["until"] > parsed[-1]["t"]:
                fail(f"{we}: until_ms {expr['until']} is after the action ends ({parsed[-1]['t']})")
            if expr["until"] < expr["in"]:
                warn(f"{we}: given back (until_ms {expr['until']}) before it is fully in (in_ms {expr['in']})")
        out.append({"name": name, "description": desc, "keys": parsed, "expr": expr})
    if not out:
        fail("no actions")
    if len(out) > 255:
        fail("more than 255 actions")
    return out


def emit_header(actions, src):
    out = [f"/* Generated by scripts/actions2c.py from {src}. Do not edit. */",
           "#pragma once", "", '#include "face_priv.h"', "",
           f"#define FACE_ACTION_COUNT {len(actions)}", "", "enum {"]
    out += [f"    FACE_ACTION_{a['name'].upper()} = {i}," for i, a in enumerate(actions)]
    out += ["};", "", "extern const face_action_def_t FACE_ACTIONS[FACE_ACTION_COUNT];", ""]
    return "\n".join(out)


def emit_source(actions, src):
    out = [f"/* Generated by scripts/actions2c.py from {src}. Do not edit. */",
           '#include "face_action_data.h"', ""]
    for a in actions:
        out.append(f"static const face_key_def_t s_keys_{a['name']}[] = {{")
        for k in a["keys"]:
            out.append(f"    {{ {k['t']}, {EASE[k['ease']]}, {{ {', '.join(cf(v) for v in k['off'])} }} }},")
        out += ["};", ""]
    out.append("const face_action_def_t FACE_ACTIONS[FACE_ACTION_COUNT] = {")
    for a in actions:
        e = a["expr"]
        out += [f"    [FACE_ACTION_{a['name'].upper()}] = {{",
                f"        .name = {json.dumps(a['name'])},",
                f"        .description = {json.dumps(a['description'])},",
                f"        .expr = {e['index']},{'  /* ' + e['name'] + ' */' if e['name'] else ''}",
                f"        .expr_in_ms = {e['in']}, .expr_until_ms = {e['until']}, .expr_out_ms = {e['out']},",
                f"        .key_count = {len(a['keys'])},",
                f"        .keys = s_keys_{a['name']},",
                "    },"]
    out += ["};", ""]
    return "\n".join(out)


def write(path, text):
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(text)


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    actions_path, avatar_path, out_dir = sys.argv[1:]
    actions = parse(actions_path, avatar_path)
    for w in warnings:
        print("actions2c: warning: " + w, file=sys.stderr)
    os.makedirs(out_dir, exist_ok=True)
    src = os.path.basename(actions_path)
    write(os.path.join(out_dir, "face_action_data.h"), emit_header(actions, src))
    write(os.path.join(out_dir, "face_action_data.c"), emit_source(actions, src))


if __name__ == "__main__":
    main()
