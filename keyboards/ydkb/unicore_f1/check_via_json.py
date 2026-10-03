#!/usr/bin/env python3
"""Check that a VIA definition actually describes this firmware.

A VIA JSON is only loadable-and-correct if it matches the firmware it will be
used against. Everything asserted here is read out of the keyboard sources or
the build output -- never out of the vendor document, which is only used as a
provenance reference.

    python3 keyboards/ydkb/unicore_f1/check_via_json.py [definition.json]

Exits non-zero on any failure.
"""
import json
import re
import shutil
import sys
import tempfile
from pathlib import Path

KM = Path(__file__).resolve().parent
def qmk_root():
    """Locate the QMK tree this keyboard is built inside.

    Two layouts are in play: a plain QMK checkout (keyboard at
    keyboards/<vendor>/<board>, so two levels up is the root) and this repo,
    where QMK is the qmk_firmware/ submodule and the keyboard is a copy inside
    it. Walk up looking for the marker instead of assuming a fixed depth.
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
DEFAULT = docs_dir() / "dusk67_via.json"
# Provenance: the definition this one was derived from.
VENDOR = Path.home() / "work/vial-qmk-tip/keyboards/ydkb/unicore_f1/ydkb_dusk67_via_v3.json"

# VIA layout-option fields the firmware actually decodes (see led.c).
IMPLEMENTED_OPTIONS = {"CapsLock Color"}

failures = []


def check(ok, label, detail=""):
    print(f"  {'PASS' if ok else 'FAIL'}  {label}" + (f"  [{detail}]" if detail else ""))
    if not ok:
        failures.append(label)


def main(def_path):
    d = json.loads(def_path.read_text())
    kb = json.loads((KM / "keyboard.json").read_text())
    cfg = (KM / "config.h").read_text()
    led = (KM / "led.c").read_text()
    keymap_c = (KM / "keymaps/dusk67_via/keymap.c").read_text()

    print("identity")
    check(d["name"] == "Dusk67", "name", d["name"])
    check(d["vendorId"].lower() == kb["usb"]["vid"].lower(), "vendorId == usb.vid",
          f'{d["vendorId"]} vs {kb["usb"]["vid"]}')
    check(d["productId"].lower() == kb["usb"]["pid"].lower(), "productId == usb.pid",
          f'{d["productId"]} vs {kb["usb"]["pid"]}')

    def define(src, name, cast=int):
        m = re.search(rf"#define {name}\s+(\w+)", src)
        if not m:
            sys.exit(f"{name} not found in config.h -- keyboard cannot match a definition")
        return cast(m.group(1))

    rows = define(cfg, "MATRIX_ROWS")
    cols = define(cfg, "MATRIX_COLS")
    print("\ngeometry")
    check((d["matrix"]["rows"], d["matrix"]["cols"]) == (rows, cols),
          "matrix == MATRIX_ROWS x MATRIX_COLS", f"firmware {rows}x{cols}")

    layers = re.findall(r"\[(\d)\] = LAYOUT\((.*?)\n    \),", keymap_c, re.S)
    check(len(layers) >= 1, "keymap has layers", str(len(layers)))
    for n, body in layers:
        ntok = len([t for t in body.split(",") if t.strip()])
        check(ntok == rows * cols, f"layer {n} has {rows*cols} tokens", str(ntok))
    populated = {(i // cols, i % cols)
                 for i, t in enumerate(t.strip() for t in layers[0][1].split(",") if t.strip())
                 if t != "KC_NO"}

    # A VIA/KLE key string's label lines are assigned to slots by the reader's
    # fixed alignment table (the-via/reader kle-parser.js `alignmentArr[0]`):
    # slot 0 = "row,col", slot 1 = LED, slot 3 = "group,option" (the layout
    # option this key belongs to). We replicate that table so the checks below
    # read the same fields the configurator app will.
    ALIGN = [0, 6, 2, 8, 9, 11, 3, 5, 1, 4, 7, 10]
    SLOT_ROWCOL, SLOT_GROUP = 0, 8

    def slots(key_string):
        """Split a KLE key label into the reader's slot layout."""
        out = {}
        for idx, label in enumerate(key_string.split("\n")):
            if idx < len(ALIGN):
                out[ALIGN[idx]] = label.strip()
        return out

    def walk():
        """Yield (slots, is_decal) for every key in the layout.

        In KLE a decoration object (`d: true`, plus its `w`/`h`/`x`/`y`) is a
        standalone entry that styles **the next key only** — it is not sticky.
        Getting this wrong silently reclassifies every alias/group key as art.

        A decal is pure decoration: the app must not offer it for assignment,
        so it is not required to resolve to a matrix position.
        """
        for row in d["layouts"]["keymap"]:
            pending_decal = False
            for entry in row:
                if isinstance(entry, dict):
                    if "d" in entry:
                        pending_decal = bool(entry["d"])
                    continue
                yield slots(entry), pending_decal
                pending_decal = False

    print("\nlayout coverage")
    keys = [(s, dec) for s, dec in walk()]
    decals = [s for s, dec in keys if dec]
    assignable = [s for s, dec in keys if not dec]
    check(bool(assignable), "layout has assignable keys", str(len(assignable)))
    check(len(assignable) + len(decals) == len(keys),
          "every key is either assignable or a decal",
          f"{len(assignable)} assignable + {len(decals)} decal")

    def rowcol(s):
        rc = s.get(SLOT_ROWCOL, "")
        m = re.fullmatch(r"(\d+),(\d+)", rc)
        return (int(m.group(1)), int(m.group(2))) if m else None

    unparsed = [s.get(SLOT_ROWCOL) for s in assignable if rowcol(s) is None]
    check(not unparsed, "every assignable key carries a 'row,col' legend",
          str(sorted(set(map(str, unparsed)))))

    outside = sorted({p for p in (rowcol(s) for s in assignable)
                      if p and not (0 <= p[0] < rows and p[1] < cols)})
    check(not outside, "every reference is inside the matrix", str(outside))
    missing = sorted({p for p in (rowcol(s) for s in assignable)
                      if p and p not in populated})
    check(not missing, "every assignable key resolves to a populated position",
          str(missing))
    orphan = sorted(populated - {p for s in assignable
                                 if (p := rowcol(s))})
    check(not orphan, "no populated key is missing from the layout", str(orphan))

    # Decals are allowed to name a position the firmware does not populate:
    # they are legend art for an absent switch. Report it, do not fail on it.
    phantom = sorted({p for s in decals if (p := rowcol(s)) and p not in populated})
    print(f"      {len(assignable)} assignable keys, {len(decals)} decals")
    if phantom:
        print(f"      decals naming unpopulated positions (expected, art only): {phantom}")

    print("\nlayout options")
    labels = d["layouts"]["labels"]
    for i, kind in ((0, "toggle"), (1, "toggle"), (2, "toggle"),
                    (3, "enum"), (4, "enum")):
        ok = isinstance(labels[i], str) if kind == "toggle" \
            else isinstance(labels[i], list) and len(labels[i]) > 1
        check(ok, f"option {i} is a {kind}", str(labels[i])[:44])
    size = define(cfg, "VIA_EEPROM_LAYOUT_OPTIONS_SIZE")
    check(size == 2, "VIA_EEPROM_LAYOUT_OPTIONS_SIZE == 2", str(size))

    m = re.search(r"void via_set_layout_options_kb\(uint32_t value\) \{(.*?)\n\}", led, re.S)
    if not m:
        sys.exit("via_set_layout_options_kb not found in led.c")
    body = m.group(1)
    nfield = int(re.search(r"for \(uint8_t i = 0; i < (\d+); i\+\+\)", body).group(1))
    width = len(re.search(r"layout_value & (0b[01]+)", body).group(1)) - 2
    print("\nfirmware coverage of the options")
    check(nfield * width <= size * 8, "decoded fields fit in the option storage",
          f"{nfield}x{width} = {nfield*width} bits of {size*8}")
    # Scan only the trees that can define a keyboard-level decoder. A
    # repo-wide rglob("*.c") reads ~35k files and dominates the runtime; the
    # weak default lives in quantum/, real ones in keyboards/.
    SRC_DIRS = [REPO / "keyboards", REPO / "quantum", REPO / "users"] if REPO else []
    decoders = []
    for root in SRC_DIRS:
        if not root.is_dir():
            continue
        for f in root.rglob("*.c"):
            if ".build" in f.parts:
                continue
            text = f.read_text(errors="replace")
            if ("via_set_layout_options_kb" in text
                    and "__attribute__((weak))" not in text):
                decoders.append(f)
    def where(f):
        # The decoder may live in the QMK tree while this script is invoked from
        # a copy elsewhere; relative_to(KM) would then raise. Show the path
        # relative to whichever tree it is actually in.
        for root in (KM, REPO):
            try:
                return str(f.relative_to(root))
            except ValueError:
                continue
        return str(f)

    if not REPO:
        print("      (no QMK tree found above this script -- decoder scan skipped)")
    check(len(decoders) == 1, "exactly one real option decoder",
          ", ".join(where(f) for f in decoders))

    # Who implements each option? A layout option is a *firmware* switch only
    # if the firmware reads its bit. A VIA option can equally be resolved by
    # the configurator: keys carry a "group,option" legend, and the app draws
    # a different key for each option value. Those need no firmware decoder and
    # are NOT a firmware gap.
    group_owners = {}
    for s, _dec in keys:
        g = s.get(SLOT_GROUP)
        if not g:
            continue
        m = re.fullmatch(r"(\d+),(\d+)", g)
        if not m:
            check(False, "group legend is 'group,option'", repr(g))
            continue
        gi = int(m.group(1))
        group_owners.setdefault(gi, set()).add(int(m.group(2)))
    names = [x[0] if isinstance(x, list) else x for x in labels]
    n_options = len(labels)
    n_app_side = 0
    for i, name in enumerate(names):
        if name in IMPLEMENTED_OPTIONS:
            how = "firmware"
        elif len(group_owners.get(i, ())) > 1:
            # The app draws a distinct key per value, so the option is real
            # without any firmware-side bit being read.
            how = "app"
            n_app_side += 1
        else:
            how = "UNIMPLEMENTED"
        print(f"      {how:<14} option {i}: {name}"
              + (f"   [group {i} values {sorted(group_owners[i])}]" if i in group_owners else ""))
    unowned = [i for i in range(n_options)
               if names[i] not in IMPLEMENTED_OPTIONS and i not in group_owners]
    check(not unowned, "every option is resolved by firmware or by the app",
          str(unowned))
    print(f"      -> {n_app_side} app-side (group/legend), "
          f"{len(IMPLEMENTED_OPTIONS)} firmware-side; "
          f"firmware decodes {nfield * width} of {size * 8} stored bits")

    print("\nwell-formedness")
    scratch = Path(tempfile.mkdtemp(prefix="hermes-verify-viajson-"))
    try:
        rt = scratch / "rt.json"
        rt.write_text(json.dumps(d))
        check(json.loads(rt.read_text()) == d, "round-trips through a temp file")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    allowed = {"x", "y", "w", "h", "x2", "y2", "w2", "h2", "c", "d"}
    used = {k for row in d["layouts"]["keymap"] for k in row
            if isinstance(k, dict) for k in k}
    check(used <= allowed, "decoration keys are standard VIA keys", str(sorted(used - allowed)))
    if VENDOR.exists():
        v = json.loads(VENDOR.read_text())
        check(d["layouts"] == v["layouts"], "layout block identical to the vendor definition")
        check({k: x for k, x in d.items() if k != "layouts"}
              == {k: x for k, x in v.items() if k != "layouts"},
              "identity block matches the vendor definition")
    else:
        print(f"      (vendor reference absent, skipped: {VENDOR})")

    print("\n" + ("ALL CHECKS PASSED" if not failures
                  else f"{len(failures)} FAILED: " + "; ".join(failures)))
    return 1 if failures else 0


def reference(path, explicit):
    """Resolve the reference file, or explain why we cannot check.

    A missing *default* is a fresh clone with nothing to compare against, which
    is not a firmware defect -- skip loudly and pass. A missing *explicit*
    argument is the caller's mistake and must not read as silently green.
    """
    if path.exists():
        return path
    if explicit:
        sys.exit(f"error: no such file: {path}")
    print(f"SKIP  no VIA definition at {path}\n"
          f"      nothing to compare against; pass one with "
          f"{sys.argv[0]} <definition.json>")
    sys.exit(0)


if __name__ == "__main__":
    sys.exit(main(reference(Path(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT,
                            explicit=len(sys.argv) > 1)))