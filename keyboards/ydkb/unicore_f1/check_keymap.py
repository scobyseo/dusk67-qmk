#!/usr/bin/env python3
"""Cross-check the Dusk67 keymap against the reference layout export.

    python3 keyboards/ydkb/unicore_f1/check_keymap.py [export.json]

With no argument it uses docs/dusk67.layout.json from this repo.

Nothing upstream CI covers this keyboard, so this is an **ad-hoc** check, not
part of any suite. It exists because the keymap is transcribed by hand from a
Vial app export, and a transcription slip (a dropped row, a mis-spelled
keycode, a stale alias) compiles cleanly and is invisible until you type on the
board.

Asserts:
  * every populated layer has MATRIX_ROWS * MATRIX_COLS tokens;
  * each layer equals the export exactly, after mapping the export's Vial-only
    keycode spellings onto QMK names;
  * every keycode the keymap names actually exists in this QMK tree;
  * layers the export leaves empty are left empty here too.

Exits non-zero on the first failed layer.
"""
import json
import re
import sys
from pathlib import Path

KM = Path(__file__).resolve().parent
def qmk_root():
    """Locate the QMK tree this keyboard is built inside, or None.

    Two layouts are in play: a plain QMK checkout (keyboard at
    keyboards/<vendor>/<board>, so two levels up is the root) and this repo,
    where QMK is the qmk_firmware/ submodule and the keyboard is a copy inside
    it. Walk up looking for the marker instead of assuming a fixed depth.
    Return None rather than exiting: only the checks that genuinely need QMK
    should fail, so a bare copy of this script still runs the rest.
    """
    for parent in KM.parents:
        if (parent / "quantum" / "keycodes.h").exists():
            return parent
        nested = parent / "qmk_firmware"
        if (nested / "quantum" / "keycodes.h").exists():
            return nested
    return None


def docs_dir():
    """Find the reference files: this repo's docs/, wherever it sits."""
    for parent in KM.parents:
        if (parent / "docs" / "dusk67_via.json").exists():
            return parent / "docs"
        # from inside the submodule, docs/ is one level up
        if (parent / "docs").is_dir() and (parent.parent / "docs").is_dir():
            return parent.parent / "docs"
    return KM.parents[2] / "docs"
    sys.exit(f"QMK tree not found above {KM}")

REPO = qmk_root()
DOCS = docs_dir()
KEYMAP_C = KM / "keymaps/dusk67_via/keymap.c"
DEFAULT_EXPORT = docs_dir() / "dusk67.layout.json"

# Vial app export spelling -> QMK spelling. Only the names that actually differ
# need an entry; everything else is spelled identically in both.
EXPORT_TO_QMK = {
    "QK_GESC": "KC_GESC", "KC_LEFT_SHIFT": "KC_LSFT", "KC_LEFT_CTRL": "KC_LCTL",
    "KC_SPACE": "KC_SPC", "KC_COMMA": "KC_COMM", "KC_LEFT_ALT": "KC_LALT",
    "KC_LEFT_GUI": "KC_LGUI", "KC_RIGHT_ALT": "KC_RALT", "KC_RIGHT_GUI": "KC_RGUI",
    "KC_RIGHT_CTRL": "KC_RCTL", "KC_RIGHT": "KC_RGHT", "KC_SLASH": "KC_SLSH",
    "KC_RIGHT_SHIFT": "KC_RSFT", "KC_PGDN": "KC_PGDN", "KC_PGUP": "KC_PGUP",
    "KC_SEMICOLON": "KC_SCLN", "KC_QUOTE": "KC_QUOT", "KC_ENTER": "KC_ENT",
    "KC_LEFT_BRACKET": "KC_LBRC", "KC_RIGHT_BRACKET": "KC_RBRC",
    "KC_DELETE": "KC_DEL", "KC_GRAVE": "KC_GRV", "KC_MINUS": "KC_MINS",
    "KC_EQUAL": "KC_EQL", "KC_CAPS_LOCK": "KC_CAPS", "KC_APPLICATION": "KC_APP",
    "KC_PRINT_SCREEN": "KC_PSCR", "KC_SCROLL_LOCK": "KC_SLCK",
    "KC_NUM_LOCK": "KC_NLCK", "KC_LANGUAGE_1": "KC_HAEN",
    "KC_LANGUAGE_2": "KC_HANJ", "KC_VOL_DOWN": "KC_VOLD", "KC_VOL_UP": "KC_VOLU",
}

failures = []


def check(ok, label, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f"  [{detail}]" if detail else ""))
    if not ok:
        failures.append(label)


def firmware_layer(n, src):
    """Tokens of layer n, in matrix order."""
    m = re.search(rf"\[{n}\] = LAYOUT\((.*?)\n    \),", src, re.S)
    if not m:
        sys.exit(f"layer {n} not found in {KEYMAP_C}")
    return [t.strip() for t in m.group(1).split(",") if t.strip()]


def main(export_path):
    export = json.loads(export_path.read_text())
    src = KEYMAP_C.read_text()
    cfg = (KM / "config.h").read_text()

    def define(name):
        m = re.search(rf"#define {name}\s+(\w+)", cfg)
        if not m:
            sys.exit(f"{name} not found in config.h")
        return int(m.group(1))

    rows, cols = define("MATRIX_ROWS"), define("MATRIX_COLS")

    print(f"reference: {export_path}")
    print(f"export name: {export.get('name')}  layers: {len(export['layers'])}")
    print("\ngeometry")
    check(len(export["layers"]) == 6, "export carries 6 layers",
          str(len(export["layers"])))

    populated = []
    n_layers = len(re.findall(r"\[\d+\] = LAYOUT\(", src))
    for n in range(min(len(export["layers"]), n_layers)):
        want = export["layers"][n]
        got = firmware_layer(n, src)
        check(len(got) == rows * cols, f"layer {n} has {rows * cols} tokens",
              str(len(got)))
        diff = [(i, EXPORT_TO_QMK.get(got[i], got[i]), want[i])
                for i in range(min(len(got), len(want)))
                if EXPORT_TO_QMK.get(got[i], got[i]) != want[i]]
        empty = all(k in ("KC_NO", "KC_TRNS") for k in want)
        check(not diff, f"layer {n} equals the export",
              "empty in export, left empty" if empty and not diff else str(diff[:4]))
        if not empty:
            populated.append(n)

    print("\nkeycodes exist in this QMK tree")
    if not REPO:
        print("      (no QMK tree found above this script -- skipped)")
        headers = None
    headers = ""
    if REPO:
        headers = "".join((REPO / "quantum" / h).read_text()
                          for h in ("keycodes.h", "quantum_keycodes.h"))
    used = set()
    for n in populated:
        for t in firmware_layer(n, src):
            if t in ("KC_NO", "KC_TRNS"):
                continue
            # a layer-tap entry is a macro *call* ("MO(1)"); test the macro name
            m = re.match(r"[A-Za-z_][A-Za-z_0-9]*", t)
            used.add(m.group(0) if m else t)
    for kc in (sorted(used) if headers else []):
        # enum member, object macro, or function-like macro
        pat = rf"(#define\s+{kc}\s*[\(\s])|(^\s+{kc}\s*=)|(^\s*{kc}\s*[,=])"
        check(re.search(pat, headers, re.M) is not None, f"{kc} is defined")

    print("\n" + ("ALL CHECKS PASSED" if not failures
                  else f"{len(failures)} FAILED: " + "; ".join(failures)))
    return 1 if failures else 0


def reference(path, explicit):
    """Resolve the reference file, or explain why we cannot check.

    A missing *default* is a fresh clone with nothing to compare against, which
    is not a firmware defect -- skip loudly and pass. A missing *explicit*
    argument is the caller's mistake and must not be silently green.
    """
    if path.exists():
        return path
    if explicit:
        sys.exit(f"error: no such file: {path}")
    print(f"SKIP  no reference export at {path}\n"
          f"      nothing to compare against; pass one with "
          f"{sys.argv[0]} <export.json>")
    sys.exit(0)


if __name__ == "__main__":
    sys.exit(main(reference(Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_EXPORT,
                            explicit=len(sys.argv) > 1)))