#!/usr/bin/env python3
"""Convert a bible-strong avatar definition (.avatar.json) into C tables.

Usage:
    avatar2c.py <avatar.json> <out_dir>

Writes face_data.h and face_data.c into <out_dir>. The face component's
CMakeLists.txt runs this automatically at build time, so editing the JSON
and running `idf.py build` is enough.

Only what the face engine supports is accepted; anything else stops the
build with a message instead of silently drawing something wrong.
"""
import json
import os
import re
import sys

SCHEMA = "bible-strong/avatar-definition"

MOTION = {
    "none": "FACE_MOTION_NONE",
    "microSaccades": "FACE_MOTION_MICRO_SACCADES",
    "shake": "FACE_MOTION_SHAKE",
    "slowDrift": "FACE_MOTION_SLOW_DRIFT",
}
TRANSITION = {
    "smooth": "FACE_TRANSITION_SMOOTH",
    "snappy": "FACE_TRANSITION_SNAPPY",
    "spring": "FACE_TRANSITION_SPRING",
}
PLAYBACK = {
    "loop": "FACE_PLAYBACK_LOOP",
    "once": "FACE_PLAYBACK_ONCE",
    "pingPong": "FACE_PLAYBACK_PING_PONG",
}


def fail(msg):
    sys.exit("avatar2c: error: " + msg)


def ident(key):
    s = re.sub(r"[^A-Za-z0-9]+", "_", key).strip("_").upper()
    if not s or s[0].isdigit():
        fail(f"cannot make a C identifier from '{key}'")
    return s


def color(value, where):
    if not isinstance(value, str) or not re.fullmatch(r"#[0-9a-fA-F]{6}", value):
        fail(f"{where}: expected a color like '#d97757', got {value!r}")
    return int(value[1:], 16)


def num(value, where):
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        fail(f"{where}: expected a number, got {value!r}")
    return float(value)


def ms(value, where):
    v = num(value, where)
    if v < 0 or v > 65535:
        fail(f"{where}: {v} ms is outside 0..65535")
    return int(round(v))


def cf(x):
    s = f"{x:.4f}".rstrip("0")
    if s.endswith("."):
        s += "0"
    if s in ("-0.0",):
        s = "0.0"
    return s + "f"


def lookup(table, value, where):
    if value not in table:
        fail(f"{where}: unsupported value {value!r} (supported: {', '.join(table)})")
    return table[value]


def parse(path):
    with open(path, encoding="utf-8") as fp:
        doc = json.load(fp)

    if doc.get("schema") != SCHEMA or doc.get("schemaVersion") != 1:
        fail(f"{path}: not a {SCHEMA} v1 file")

    prim = doc["body"]["primary"]
    if prim.get("type") != "sphere":
        fail(f"body type '{prim.get('type')}' is not supported yet (only 'sphere')")
    if doc["body"].get("nodes"):
        fail("body nodes (extra parts) are not supported yet")
    body = tuple(num(prim[k], f"body.{k}") for k in ("width", "height", "depth"))

    default_body = color(doc["colors"]["body"], "colors.body")
    default_eyes = color(doc["colors"]["eyes"], "colors.eyes")

    order = doc["expressionOrder"]
    if "neutral" not in order:
        fail("expressionOrder has no 'neutral'")
    if len(order) > 255:
        fail("more than 255 expressions")

    exprs = []
    for key in order:
        e = doc["expressions"].get(key)
        if e is None:
            fail(f"expressionOrder lists '{key}' but it is not defined")
        w = f"expressions.{key}"
        eyes = []
        for side in ("left", "right"):
            s = e["eyes"][side]
            eyes.append(tuple(num(s[k], f"{w}.eyes.{side}.{k}")
                              for k in ("width", "height", "x", "y", "angle")))
        colors = e.get("colors", {})
        exprs.append({
            "key": key,
            "head": tuple(num(e["head"][k], f"{w}.head.{k}") for k in ("x", "y", "z")),
            "eyes": eyes,
            "spacing": num(e["eyes"]["spacing"], f"{w}.eyes.spacing"),
            "perspective": num(e.get("perspective", 1), f"{w}.perspective"),
            "eye_motion": lookup(MOTION, e["motion"]["eyes"], f"{w}.motion.eyes"),
            "body_motion": lookup(MOTION, e["motion"]["body"], f"{w}.motion.body"),
            "body_rgb": color(colors["body"], f"{w}.colors.body") if "body" in colors else default_body,
            "eyes_rgb": color(colors["eyes"], f"{w}.colors.eyes") if "eyes" in colors else default_eyes,
        })

    index = {e["key"]: i for i, e in enumerate(exprs)}

    anims = []
    for key in doc.get("animationOrder", []):
        a = doc["animations"].get(key)
        if a is None:
            fail(f"animationOrder lists '{key}' but it is not defined")
        w = f"animations.{key}"
        steps = []
        for i, st in enumerate(a["steps"]):
            ws = f"{w}.steps[{i}]"
            if st["expression"] not in index:
                fail(f"{ws}: unknown expression '{st['expression']}'")
            steps.append({
                "expr": index[st["expression"]],
                "transition": lookup(TRANSITION, st.get("transition", "smooth"), f"{ws}.transition"),
                "transition_ms": ms(st["transitionMs"], f"{ws}.transitionMs"),
                "hold_ms": ms(st["holdMs"], f"{ws}.holdMs"),
            })
        if not steps or len(steps) > 255:
            fail(f"{w}: needs 1..255 steps")
        b = a.get("blink", {"enabled": False})
        anims.append({
            "key": key,
            "playback": lookup(PLAYBACK, a.get("playbackMode", "loop"), f"{w}.playbackMode"),
            "steps": steps,
            "blink": (
                bool(b.get("enabled", False)),
                ms(b.get("initialDelayMs", 0), f"{w}.blink.initialDelayMs"),
                ms(b.get("minIntervalMs", 0), f"{w}.blink.minIntervalMs"),
                ms(b.get("maxIntervalMs", 0), f"{w}.blink.maxIntervalMs"),
                ms(b.get("durationMs", 0), f"{w}.blink.durationMs"),
            ),
        })
    if len(anims) > 255:
        fail("more than 255 animations")

    return {"name": doc.get("name", "avatar"), "body": body,
            "default_body": default_body, "default_eyes": default_eyes,
            "exprs": exprs, "anims": anims}


def emit_header(d, src_name):
    out = [f"/* Generated by scripts/avatar2c.py from {src_name}. Do not edit. */",
           "#pragma once", "", '#include "face_priv.h"', "",
           f"#define FACE_EXPR_COUNT {len(d['exprs'])}",
           f"#define FACE_ANIM_COUNT {len(d['anims'])}", "",
           "enum {"]
    out += [f"    FACE_EXPR_{ident(e['key'])} = {i}," for i, e in enumerate(d["exprs"])]
    out += ["};", "", "enum {"]
    out += [f"    FACE_ANIM_{ident(a['key'])} = {i}," for i, a in enumerate(d["anims"])]
    out += ["};", "",
            "extern const char *const FACE_AVATAR_NAME;",
            "extern const face_body_t FACE_BODY;",
            "extern const uint32_t FACE_DEFAULT_BODY_RGB;",
            "extern const uint32_t FACE_DEFAULT_EYES_RGB;",
            "extern const char *const FACE_EXPR_NAMES[FACE_EXPR_COUNT];",
            "extern const face_expr_def_t FACE_EXPRS[FACE_EXPR_COUNT];",
            "extern const face_anim_def_t FACE_ANIMS[FACE_ANIM_COUNT];", ""]
    return "\n".join(out)


def emit_source(d, src_name):
    q = lambda s: json.dumps(s)
    bw, bh, bd = d["body"]
    out = [f"/* Generated by scripts/avatar2c.py from {src_name}. Do not edit. */",
           '#include "face_data.h"', "",
           f"const char *const FACE_AVATAR_NAME = {q(d['name'])};",
           f"const face_body_t FACE_BODY = {{ {cf(bw)}, {cf(bh)}, {cf(bd)} }};",
           f"const uint32_t FACE_DEFAULT_BODY_RGB = 0x{d['default_body']:06x};",
           f"const uint32_t FACE_DEFAULT_EYES_RGB = 0x{d['default_eyes']:06x};", "",
           "const char *const FACE_EXPR_NAMES[FACE_EXPR_COUNT] = {"]
    out += [f"    {q(e['key'])}," for e in d["exprs"]]
    out += ["};", "", "const face_expr_def_t FACE_EXPRS[FACE_EXPR_COUNT] = {"]
    for e in d["exprs"]:
        hx, hy, hz = e["head"]
        eyes = ", ".join("{ " + ", ".join(cf(v) for v in eye) + " }" for eye in e["eyes"])
        out += [f"    [FACE_EXPR_{ident(e['key'])}] = {{",
                f"        .head = {{ {cf(hx)}, {cf(hy)}, {cf(hz)} }},",
                f"        .eye = {{ {eyes} }},",
                f"        .spacing = {cf(e['spacing'])},",
                f"        .perspective = {cf(e['perspective'])},",
                f"        .eye_motion = {e['eye_motion']},",
                f"        .body_motion = {e['body_motion']},",
                f"        .body_rgb = 0x{e['body_rgb']:06x},",
                f"        .eyes_rgb = 0x{e['eyes_rgb']:06x},",
                "    },"]
    out += ["};", ""]
    for a in d["anims"]:
        out.append(f"static const face_step_def_t s_steps_{ident(a['key']).lower()}[] = {{")
        for st in a["steps"]:
            out.append(f"    {{ {st['expr']}, {st['transition']}, {st['transition_ms']}, {st['hold_ms']} }},"
                       f"  /* {d['exprs'][st['expr']]['key']} */")
        out += ["};", ""]
    out.append("const face_anim_def_t FACE_ANIMS[FACE_ANIM_COUNT] = {")
    for a in d["anims"]:
        en, init, mn, mx, dur = a["blink"]
        out += [f"    [FACE_ANIM_{ident(a['key'])}] = {{",
                f"        .name = {q(a['key'])},",
                f"        .playback = {a['playback']},",
                f"        .step_count = {len(a['steps'])},",
                f"        .steps = s_steps_{ident(a['key']).lower()},",
                f"        .blink = {{ {'true' if en else 'false'}, {init}, {mn}, {mx}, {dur} }},",
                "    },"]
    out += ["};", ""]
    return "\n".join(out)


def write(path, text):
    # always rewrite: the build tool compares timestamps against the JSON
    with open(path, "w", encoding="utf-8") as fp:
        fp.write(text)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    src, out_dir = sys.argv[1], sys.argv[2]
    d = parse(src)
    os.makedirs(out_dir, exist_ok=True)
    name = os.path.basename(src)
    write(os.path.join(out_dir, "face_data.h"), emit_header(d, name))
    write(os.path.join(out_dir, "face_data.c"), emit_source(d, name))


if __name__ == "__main__":
    main()
