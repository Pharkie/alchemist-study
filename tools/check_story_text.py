#!/usr/bin/env python3
"""Build gate: authored story text must FIT its screen.

drawWrapped() silently drops words past its line budget, so overflowing
copy ships as truncated cards (found the hard way: "Realm Saved" lost
"...Alchemist of Whiterun." on-device). This script parses the story
tables straight out of src/main.cpp — every new node is covered with no
extra bookkeeping — replicates drawWrapped's greedy word wrap against
each renderer's real budget, and FAILS THE BUILD on overflow.

Budgets mirror the render call sites in main.cpp (grep drawWrapped):
  card body        4 lines x 22 chars   (drawWrapped ... 4, 114; 5x8 = 5 px/char)
  card body w/ HP  2 lines x 22         (heal != 0 shows the bar; ... 2, 114)
  speak bubble     4 lines x 15         (drawWrapped ... 4, 76)
  brew hint        2 lines x 24         (drawWrapped ... 2, 120)
  battle intro     2 lines x 24         (s_bmsg seed; drawWrapped ... 2, 124)
  card/speak title 21 chars             (helvB08; longest bench-proven title)
  brew title       24 chars             (drawTitleBar, left-aligned helvB08)

Dynamic strings (snprintf'd battle messages) are not checked — keep
their format strings short. Runs standalone or as a PlatformIO
extra_scripts pre-action (any error aborts the build).

Usage: python3 tools/check_story_text.py [path/to/main.cpp]
"""
import os
import re
import sys

CARD_LINES, CARD_HP_LINES, CARD_COLS = 4, 2, 22
SPEAK_LINES, SPEAK_COLS = 4, 15
HINT_LINES, HINT_COLS = 2, 24
INTRO_LINES, INTRO_COLS = 2, 24
TITLE_COLS, BREW_TITLE_COLS = 21, 24


def wrap_lines(text, cols):
    """drawWrapped's greedy wrap: first word always starts a line."""
    lines, line = [], ""
    for tok in text.split():
        trial = (line + " " + tok) if line else tok
        if not line or len(trial) <= cols:
            line = trial
        else:
            lines.append(line)
            line = tok
    if line:
        lines.append(line)
    return lines


def check_wrap(errors, where, text, max_lines, cols):
    lines = wrap_lines(text, cols)
    if len(lines) > max_lines:
        errors.append(f"{where}: needs {len(lines)} lines, budget {max_lines}x{cols}\n"
                      f"    text: \"{text}\"\n"
                      f"    overflow: \"{' '.join(lines[max_lines:])}\"")
    for ln in lines:
        if len(ln) > cols:
            errors.append(f"{where}: word wider than a line ({cols} chars): \"{ln}\"")


def check_len(errors, where, text, cols):
    if len(text) > cols:
        errors.append(f"{where}: {len(text)} chars, budget {cols}: \"{text}\"")


def split_fields(entry):
    """Split one brace-entry on top-level commas, respecting string literals."""
    fields, depth, in_str, esc, cur = [], 0, False, False, ""
    for ch in entry:
        if in_str:
            cur += ch
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            cur += ch
        elif ch in "({[":
            depth += 1
            cur += ch
        elif ch in ")}]":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            fields.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        fields.append(cur.strip())
    return fields


def field_str(f):
    """A field's string literal (C adjacent literals concatenated), or None."""
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', f)
    return "".join(parts) if parts else None


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def extract_braced(src, start):
    """Return the text inside the brace block opening at src[start] == '{'."""
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start + 1:i]
    raise ValueError("unbalanced braces")


def check_story_table(errors, src):
    m = re.search(r"kStorySkyrim\[\]\s*=\s*", src)
    if not m:
        errors.append("parser: kStorySkyrim[] not found — update check_story_text.py")
        return
    body = extract_braced(src, src.index("{", m.end()))
    # Entries are themselves brace blocks at top level.
    i, n = 0, 0
    while i < len(body):
        if body[i] == "{":
            entry = extract_braced(body, i)
            i += len(entry) + 2
            fields = split_fields(entry)
            if len(fields) < 9:
                errors.append(f"node {n}: parsed {len(fields)} fields — table shape "
                              f"changed? update check_story_text.py")
                n += 1
                continue
            kind = fields[0]
            title, text = field_str(fields[1]), field_str(fields[2])
            heal_zero = fields[8].strip() in ("0", "")
            where = f"kStorySkyrim[{n}] ({kind}"
            where += f' "{title}")' if title else ")"
            if title:
                check_len(errors, where + " title", title, TITLE_COLS)
            if text:
                if kind == "N_CARD":
                    check_wrap(errors, where, text,
                               CARD_LINES if heal_zero else CARD_HP_LINES, CARD_COLS)
                elif kind == "N_SPEAK":
                    check_wrap(errors, where, text, SPEAK_LINES, SPEAK_COLS)
            n += 1
        else:
            i += 1


def check_battle_defs(errors, src):
    for m in re.finditer(r"BattleDef\s+(\w+)\s*=\s*", src):
        name = m.group(1)
        fields = split_fields(extract_braced(src, src.index("{", m.end())))
        if len(fields) < 8:
            errors.append(f"{name}: parsed {len(fields)} fields — BattleDef shape "
                          f"changed? update check_story_text.py")
            continue
        intro, brew_title, hint = (field_str(fields[1]), field_str(fields[6]),
                                   field_str(fields[7]))
        if intro:
            check_wrap(errors, f"{name} intro", intro, INTRO_LINES, INTRO_COLS)
        if brew_title:
            check_len(errors, f"{name} brewTitle", brew_title, BREW_TITLE_COLS)
        if hint:
            check_wrap(errors, f"{name} hint", hint, HINT_LINES, HINT_COLS)


def run(path):
    src = strip_comments(open(path).read())
    errors = []
    check_story_table(errors, src)
    check_battle_defs(errors, src)
    return errors


def main(path=None):
    if path is None:  # __file__ is undefined under SCons; only touch it here
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        path = os.path.join(root, "src", "main.cpp")
    errors = run(path)
    if errors:
        print("\n[check_story_text] STORY TEXT OVERFLOWS ITS SCREEN:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        print(f"  ({len(errors)} problem(s) — budgets documented at the top of "
              f"tools/check_story_text.py)", file=sys.stderr)
        raise SystemExit(1)
    print(f"[check_story_text] OK — all authored story text fits ({path})")


# PlatformIO extra_scripts pre-action: same check, abort the build on failure.
# The try guards ONLY the Import probe — main() must never be inside it, or a
# genuine NameError from the checker would be silently swallowed.
try:
    Import("env")  # noqa: F821  (defined only under SCons)
    _scons_root = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    _scons_root = None

if _scons_root is not None:
    main(os.path.join(_scons_root, "src", "main.cpp"))
elif __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else None)
