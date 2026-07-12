#!/usr/bin/env python3
"""Build gate: cross-file contracts that used to live only in docs.

Each section mechanizes a rule that was previously prose (CLAUDE.md /
TUNING.md / code comments) and therefore rotted silently:

  graph    story successor indices in range, every node reachable from 0,
           battle/brew nodes carry a def, brew combos are 1..7
  battle   enemy HP > 2 x max attack damage (14) so the scripted crit
           lands before the enemy can die (docs' "battle rules")
  stir     kStirCap > kStirDecay per level ("or it's unfillable"),
           align fill/drain/tolerance all positive
  pins     src/pins.h vs the GPIO numbers quoted in CLAUDE.md,
           docs/CIRCUIT.md, docs/HARDWARE_TEST.md and docs/ASSEMBLY.md
           (copies that were updated by hand this week — never again)
  twins    the d20 face-numbering and icosahedron face tables duplicated
           in tools/screens_preview.py match main.cpp exactly
  strings  every multi-word display string the sim twin draws still
           exists in main.cpp — stale transliterations get flagged the
           moment the firmware copy changes
  tuning   every `CONSTANT` | `value` table row in docs/TUNING.md matches
           the value in main.cpp (quoted numbers rot on every retune)
  docs     menu labels, potion and ingredient names all appear in
           CLAUDE.md (the reference tables readers actually consult)

Runs standalone or as a PlatformIO extra_scripts pre-action; any failure
aborts the build with the section and offending item named.

Usage: python3 tools/check_invariants.py [project-root]
"""
import ast
import os
import re
import sys


def tools_dir(root):
    return os.path.join(root, "tools")


# ---- story graph -----------------------------------------------------------

STORY_KINDS_BOTH_NEXT = {"N_CHOICE", "N_BATTLE"}


def parse_story_nodes(cst, src):
    m = re.search(r"kStorySkyrim\[\]\s*=\s*", src)
    if not m:
        return None
    body = cst.extract_braced(src, src.index("{", m.end()))
    nodes, i = [], 0
    while i < len(body):
        if body[i] == "{":
            entry = cst.extract_braced(body, i)
            i += len(entry) + 2
            nodes.append(cst.split_fields(entry))
        else:
            i += 1
    return nodes


def check_graph(errors, cst, src):
    nodes = parse_story_nodes(cst, src)
    if nodes is None:
        errors.append("graph: kStorySkyrim[] not found — update check_invariants.py")
        return
    n = len(nodes)

    def as_int(f):
        try:
            return int(f.strip())
        except ValueError:
            return None

    edges = {}
    for idx, f in enumerate(nodes):
        if len(f) < 14:
            errors.append(f"graph: node {idx} parsed {len(f)} fields — table shape "
                          f"changed? update check_invariants.py")
            continue
        kind = f[0].strip()
        nextA, nextB = as_int(f[5]), as_int(f[6])
        succ = []
        if kind != "N_END":
            if nextA is None or not (0 <= nextA < n):
                errors.append(f"graph: node {idx} ({kind}) nextA={f[5]} out of range 0..{n-1}")
            else:
                succ.append(nextA)
            takes_b = kind in STORY_KINDS_BOTH_NEXT or (kind == "N_BREW" and nextB != 0)
            if takes_b:
                if nextB is None or not (0 <= nextB < n):
                    errors.append(f"graph: node {idx} ({kind}) nextB={f[6]} out of range")
                else:
                    succ.append(nextB)
        if kind in ("N_BATTLE", "N_BREW") and f[13].strip() == "nullptr":
            errors.append(f"graph: node {idx} ({kind}) has no BattleDef")
        edges[idx] = succ

    seen, stack = {0}, [0]
    while stack:
        for s in edges.get(stack.pop(), []):
            if s not in seen:
                seen.add(s)
                stack.append(s)
    for idx in range(n):
        if idx not in seen:
            errors.append(f"graph: node {idx} ({nodes[idx][0].strip()}) unreachable from node 0")


def check_battle(errors, cst, src):
    for m in re.finditer(r"BattleDef\s+(\w+)\s*=\s*", src):
        name = m.group(1)
        f = cst.split_fields(cst.extract_braced(src, src.index("{", m.end())))
        if len(f) < 8:
            continue  # shape error already reported by check_story_text
        is_battle = cst.field_str(f[0]) is not None   # brew-only defs have no name
        try:
            hp, combo = int(f[3]), int(f[5])
        except ValueError:
            errors.append(f"battle: {name}: non-literal enemyHP/brewCombo — "
                          f"update check_invariants.py")
            continue
        if is_battle and hp <= 14:
            errors.append(f"battle: {name}: enemyHP {hp} <= 14 — two max rolls kill it "
                          f"before the scripted crit can land (see CLAUDE.md battle rules)")
        if not 1 <= combo <= 7:
            errors.append(f"battle: {name}: brewCombo {combo} not a valid combo 1..7")


# ---- tuning tables ---------------------------------------------------------

def float_array(src, name):
    m = re.search(re.escape(name) + r"\[\]\s*=\s*\{([^}]*)\}", src)
    if not m:
        return None
    return [float(x) for x in re.findall(r"-?\d+\.?\d*", m.group(1))]


def check_stir(errors, src):
    cap, decay = float_array(src, "kStirCap"), float_array(src, "kStirDecay")
    if not cap or not decay:
        errors.append("stir: kStirCap/kStirDecay not found — update check_invariants.py")
        return
    for i, (c, d) in enumerate(zip(cap, decay)):
        if c <= d:
            errors.append(f"stir: level {i}: kStirCap {c} <= kStirDecay {d} — bar unfillable")
    for name in ("kAlignTol", "kAlignDrift", "kAlignFill", "kAlignDrain"):
        vals = float_array(src, name)
        if not vals:
            errors.append(f"stir: {name} not found — update check_invariants.py")
        elif any(v <= 0 for v in vals):
            errors.append(f"stir: {name} has a non-positive entry: {vals}")


# ---- pin map vs docs -------------------------------------------------------

def check_pins(errors, root):
    pins_src = open(os.path.join(root, "src", "pins.h")).read()
    pins = {k: int(v) for k, v in re.findall(r"(PIN_\w+)\s*=\s*(\d+)", pins_src)}
    if len(pins) < 9:
        errors.append(f"pins: only {len(pins)} PIN_* found in pins.h")
        return

    def expect(doc, text, pattern, pin_name, what):
        m = re.search(pattern, text)
        if not m:
            errors.append(f"pins: {doc}: no row matching {what} — table reworded? "
                          f"update check_invariants.py")
        elif int(m.group(1)) != pins[pin_name]:
            errors.append(f"pins: {doc}: {what} says GPIO {m.group(1)}, "
                          f"pins.h says {pins[pin_name]}")

    claude = open(os.path.join(root, "CLAUDE.md")).read()
    for pat, pin, what in [
        (r"Reed slot 1[^|]*\|\s*(\d+)", "PIN_REED_SLOT1", "Reed slot 1"),
        (r"Reed slot 2[^|]*\|\s*(\d+)", "PIN_REED_SLOT2", "Reed slot 2"),
        (r"Reed slot 3[^|]*\|\s*(\d+)", "PIN_REED_SLOT3", "Reed slot 3"),
        (r"OLED SDA[^|]*\|\s*(\d+)", "PIN_OLED_SDA", "OLED SDA"),
        (r"OLED SCL[^|]*\|\s*(\d+)", "PIN_OLED_SCL", "OLED SCL"),
        (r"Buzzer[^|]*\|\s*(\d+)", "PIN_BUZZER", "Buzzer"),
        (r"Encoder A[^|]*\|\s*(\d+)", "PIN_ENC_A", "Encoder A"),
        (r"Encoder B[^|]*\|\s*(\d+)", "PIN_ENC_B", "Encoder B"),
        (r"Encoder SW[^|]*\|\s*(\d+)", "PIN_ENC_SW", "Encoder SW"),
    ]:
        expect("CLAUDE.md", claude, pat, pin, what)

    circuit = open(os.path.join(root, "docs", "CIRCUIT.md")).read()
    for pat, pin, what in [
        (r"GPIO(\d+) SDA", "PIN_OLED_SDA", "SDA"),
        (r"GPIO(\d+) SCL", "PIN_OLED_SCL", "SCL"),
        (r"GPIO(\d+) tone", "PIN_BUZZER", "buzzer"),
        (r"GPIO(\d+) CLK", "PIN_ENC_A", "encoder CLK"),
        (r"GPIO(\d+) DT", "PIN_ENC_B", "encoder DT"),
        (r"GPIO(\d+) SW", "PIN_ENC_SW", "encoder SW"),
        (r'GPIO(\d+)"\|\s*RD1', "PIN_REED_SLOT1", "reed 1"),
        (r'GPIO(\d+)"\|\s*RD2', "PIN_REED_SLOT2", "reed 2"),
        (r'GPIO(\d+)"\|\s*RD3', "PIN_REED_SLOT3", "reed 3"),
    ]:
        expect("docs/CIRCUIT.md", circuit, pat, pin, what)

    asm = open(os.path.join(root, "docs", "ASSEMBLY.md")).read()
    for pat, pin, what in [
        (r"Reed 1 / slot 1[^|]*\|\s*(\d+)", "PIN_REED_SLOT1", "reed 1"),
        (r"Reed 2 / slot 2[^|]*\|\s*(\d+)", "PIN_REED_SLOT2", "reed 2"),
        (r"Reed 3 / slot 3[^|]*\|\s*(\d+)", "PIN_REED_SLOT3", "reed 3"),
        (r"OLED SDA \|\s*(\d+)", "PIN_OLED_SDA", "OLED SDA"),
        (r"OLED SCL \|\s*(\d+)", "PIN_OLED_SCL", "OLED SCL"),
        (r"Buzzer[^|]*\|\s*(\d+)", "PIN_BUZZER", "buzzer"),
        (r"Encoder CLK \|\s*(\d+)", "PIN_ENC_A", "encoder CLK"),
        (r"Encoder DT \|\s*(\d+)", "PIN_ENC_B", "encoder DT"),
        (r"Encoder SW \|\s*(\d+)", "PIN_ENC_SW", "encoder SW"),
    ]:
        expect("docs/ASSEMBLY.md", asm, pat, pin, what)

    hwt = open(os.path.join(root, "docs", "HARDWARE_TEST.md")).read()
    for pat, pin, what in [
        (r"Reed 1 / slot 1 \|\s*(\d+)", "PIN_REED_SLOT1", "reed 1"),
        (r"Reed 2 / slot 2 \|\s*(\d+)", "PIN_REED_SLOT2", "reed 2"),
        (r"Reed 3 / slot 3 \|\s*(\d+)", "PIN_REED_SLOT3", "reed 3"),
        (r"OLED SDA / SCL \|\s*(\d+)", "PIN_OLED_SDA", "OLED SDA"),
        (r"Buzzer \|\s*(\d+)", "PIN_BUZZER", "buzzer"),
        (r"Encoder A / B / SW \|\s*(\d+)", "PIN_ENC_A", "encoder A"),
    ]:
        expect("docs/HARDWARE_TEST.md", hwt, pat, pin, what)


# ---- twin data tables ------------------------------------------------------

def int_table(text, pattern):
    m = re.search(pattern, text, flags=re.S)
    return [int(x) for x in re.findall(r"\d+", m.group(1))] if m else None


def check_twins(errors, root, main_src):
    twin = open(os.path.join(tools_dir(root), "screens_preview.py")).read()
    pairs = [
        ("kD20FaceNum", r"kD20FaceNum\[20\]\s*=\s*\{([^}]*)\}",
         "D20_NUM", r"D20_NUM\s*=\s*\[([^\]]*)\]"),
        ("kIcoF", r"kIcoF\[20\]\[3\]\s*=\s*\{(.*?)\};",
         "ICO_F", r"ICO_F\s*=\s*\[(.*?)\]\n"),
    ]
    for cname, cpat, pname, ppat in pairs:
        cvals, pvals = int_table(main_src, cpat), int_table(twin, ppat)
        if cvals is None or pvals is None:
            errors.append(f"twins: {cname}/{pname} not found — update check_invariants.py")
        elif cvals != pvals:
            errors.append(f"twins: {cname} (main.cpp) != {pname} (screens_preview.py) — "
                          f"the d20 twins have drifted")


def twin_display_strings(path):
    """Multi-word display literals the sim draws (plain strings only —
    docstrings, f-string fragments and single tokens are skipped)."""
    tree = ast.parse(open(path).read())
    fstring_parts = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.JoinedStr):
            for v in node.values:
                if isinstance(v, ast.Constant):
                    fstring_parts.add(id(v))
    out = []
    for node in ast.walk(tree):
        if (isinstance(node, ast.Constant) and isinstance(node.value, str)
                and id(node) not in fstring_parts):
            s = node.value
            if len(s) >= 6 and " " in s and "\n" not in s:
                out.append(s)
    return out


def check_twin_strings(errors, root, main_src):
    path = os.path.join(tools_dir(root), "screens_preview.py")
    for s in sorted(set(twin_display_strings(path))):
        if s not in main_src:
            errors.append(f'strings: twin draws "{s}" but main.cpp no longer contains it '
                          f"— screens_preview.py has drifted from the firmware")


# ---- TUNING.md quoted values vs code ----------------------------------------

def scalar_const(src, name):
    m = re.search(r"\b" + re.escape(name) + r"\s*=\s*(-?[0-9.]+)", src)
    return float(m.group(1)) if m else None


def check_tuning(errors, root, main_src):
    """Every `CONSTANT` | `value` table row in TUNING.md must match main.cpp —
    the doc quotes exact numbers, and quoted numbers rot (RIT_GLYPH_MS did)."""
    doc = open(os.path.join(root, "docs", "TUNING.md")).read()
    for line in doc.splitlines():
        if not line.startswith("| `"):
            continue
        cells = line.split("|")
        if len(cells) < 4:
            continue
        names = re.findall(r"`(\w+)(\[\])?`", cells[1])
        doc_vals = [float(x) for x in re.findall(r"-?\d+\.?\d*", cells[2])]
        if not names or not doc_vals:
            continue
        code_vals = []
        for name, brackets in names:
            if brackets:
                vals = float_array(main_src, name)
                if vals is None:
                    errors.append(f"tuning: TUNING.md documents `{name}[]` but it isn't "
                                  f"in main.cpp — renamed?")
                    code_vals = None
                    break
                code_vals.extend(vals)
            else:
                v = scalar_const(main_src, name)
                if v is None:
                    errors.append(f"tuning: TUNING.md documents `{name}` but it isn't "
                                  f"in main.cpp — renamed?")
                    code_vals = None
                    break
                code_vals.append(v)
        if code_vals is None:
            continue
        if len(code_vals) != len(doc_vals) or any(
                abs(a - b) > 1e-6 * max(1.0, abs(b)) for a, b in zip(doc_vals, code_vals)):
            errors.append(f"tuning: TUNING.md row {'/'.join(n for n, _ in names)} says "
                          f"{doc_vals}, main.cpp says {code_vals}")


# ---- docs reference tables -------------------------------------------------

def check_docs(errors, root, main_src):
    claude = open(os.path.join(root, "CLAUDE.md")).read()
    m = re.search(r"kMenu\[\]\s*=\s*\{(.*?)\n\};", main_src, flags=re.S)
    if m:
        for label in re.findall(r'\{\s*"([^"]+)"', m.group(1)):
            if label not in claude:
                errors.append(f'docs: menu item "{label}" not mentioned in CLAUDE.md')
    else:
        errors.append("docs: kMenu[] not found — update check_invariants.py")
    for arr in ("kPotions", "kIngredients"):
        m = re.search(re.escape(arr) + r"\[.*?=\s*\{(.*?)\n\};", main_src, flags=re.S)
        if not m:
            errors.append(f"docs: {arr} not found — update check_invariants.py")
            continue
        for name in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
            if len(name) >= 2 and name not in claude:
                errors.append(f'docs: {arr} entry "{name}" missing from CLAUDE.md tables')


# ---- driver ----------------------------------------------------------------

def run(root):
    sys.path.insert(0, tools_dir(root))
    import check_story_text as cst
    main_src = cst.strip_comments(open(os.path.join(root, "src", "main.cpp")).read())
    errors = []
    check_graph(errors, cst, main_src)
    check_battle(errors, cst, main_src)
    check_stir(errors, main_src)
    check_pins(errors, root)
    check_twins(errors, root, main_src)
    check_twin_strings(errors, root, main_src)
    check_tuning(errors, root, main_src)
    check_docs(errors, root, main_src)
    return errors


def main(root=None):
    if root is None:  # __file__ is undefined under SCons; only touch it here
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    errors = run(root)
    if errors:
        print("\n[check_invariants] CONTRACT VIOLATIONS:", file=sys.stderr)
        for e in errors:
            print("  " + e, file=sys.stderr)
        print(f"  ({len(errors)} problem(s) — each section is documented at the top of "
              f"tools/check_invariants.py)", file=sys.stderr)
        raise SystemExit(1)
    print("[check_invariants] OK — graph, battle maths, stir tables, pin docs, "
          "d20 twins, twin strings, TUNING values, doc tables all consistent")


# PlatformIO extra_scripts pre-action. The try guards ONLY the Import probe —
# main() must never sit inside it, or real errors get silently swallowed.
try:
    Import("env")  # noqa: F821  (defined only under SCons)
    _scons_root = env["PROJECT_DIR"]  # noqa: F821
except NameError:
    _scons_root = None

if _scons_root is not None:
    main(_scons_root)
elif __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else None)
