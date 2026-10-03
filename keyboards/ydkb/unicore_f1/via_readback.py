#!/usr/bin/env python3
"""Read the Dusk67's VIA dynamic keymap off the hardware and diff it against
the keymap compiled into the firmware.

    python3 keyboards/ydkb/unicore_f1/via_readback.py [hidraw-node]

Reads every (layer, row, col) cell through the VIA protocol 13 raw-HID
interface and compares it with `keymaps/dusk67_via/keymap.c`. Exit code 0 means
the EEPROM on the board holds the keymap the firmware was built with.

This is an **ad-hoc**, hardware-attached check, not part of any suite (see
Architecture.md §3). It exists because "the firmware builds" and "the EEPROM on
the board holds the keymap" are different claims, and only the second one
matters to the person typing on the keyboard.

## Why it is slow, and why it checks the echo

A raw-HID write returns *queued* frames, not one answer to one question: the
device can hand back a frame from an earlier request. So every reply is
accepted only when it identifies itself, on two fields at once:

    reply[0]      == 0x04             (command echo)
    reply[1..3]   == layer, row, col  (the args you asked for)

A read that checks neither is reading noise and reports confident garbage --
that is the failure this script is written against (Architecture.md §7: an
18-row bulk sweep in one pass came back scrambled, and an earlier probe passed a
flat offset instead of (layer, row, col) and decoded it anyway).

## Why the expected keycodes are parsed, not tabulated

The expected value for each token is resolved by parsing this QMK tree's own
`quantum/keycodes.h` and `quantum/quantum_keycodes.h`. A hand-written table
would be a second source of truth that silently drifts from the tree -- and
would put back exactly the class of bug this port keeps re-deriving
(Architecture.md §6).

Requires the udev rule from Architecture.md §5. Only the raw-HID node answers
these commands; the other hidraw nodes are the keyboard / shared / console
interfaces.

**This script only reads.** It sends no command that writes EEPROM -- in
particular never `id_dynamic_keymap_reset` (0x06), which on this keyboard leaves
8 cells disagreeing with flash (Architecture.md §7.2). Repair a cell, if you
must, with a single targeted `id_dynamic_keymap_set_keycode` (0x05).
"""
import os
import re
import sys
import time
from pathlib import Path

KM = Path(__file__).resolve().parent
KEYMAP_C = KM / "keymaps/dusk67_via/keymap.c"
CONFIG_H = KM / "config.h"

REPORT_LEN = 34        # VIA report size on the wire
# What actually comes back from /dev/hidraw*. hidapi strips the report ID, so a
# 34-byte report reads as 32 bytes -- and a 34-byte read() would block or
# short-read. Measured 2026-10-03: a get_protocol_version reply arrives as 32
# bytes, `01 00 0d ...`. Reading 34 is not "more complete", it is wrong.
WIRE_READ_LEN = REPORT_LEN - 2
ID_GET_PROTOCOL_VERSION = 0x01
ID_GET_KEYBOARD_VALUE = 0x02
ID_LAYOUT_OPTIONS = 0x02   # NOT 0x03 -- see via.h: 0x03 is id_switch_matrix_state
ID_DYNAMIC_KEYMAP_GET_KEYCODE = 0x04
ID_UNHANDLED = 0xFF

# Commands this tool refuses to send. 0x06 reseeds EEPROM from flash and on this
# board corrupts 8 cells (Architecture.md §7.2); 0x05/0x09/0x13 write EEPROM.
# Only the read commands below are ever used.
FORBIDDEN = {
    0x05: "id_dynamic_keymap_set_keycode writes EEPROM",
    0x06: "id_dynamic_keymap_reset corrupts 8 cells on this board (Arch.md 7.2)",
    0x09: "id_custom_save writes EEPROM",
    0x13: "id_dynamic_keymap_set_buffer writes EEPROM",
}

SETTLE_S = 0.004       # slow enough that the device is not still busy
READ_RETRIES = 40


def qmk_root():
    """The QMK tree this keyboard is built inside, or None.

    Two layouts: a plain QMK checkout (two levels up is the root) and this repo,
    where QMK is the qmk_firmware/ submodule and the keyboard is a copy inside
    it. Walk up for the marker rather than assuming a fixed depth.
    """
    for parent in KM.parents:
        if (parent / "quantum" / "keycodes.h").exists():
            return parent
        nested = parent / "qmk_firmware"
        if (nested / "quantum" / "keycodes.h").exists():
            return nested
    return None


def parse_keycodes(root):
    """name -> int for every keycode this QMK tree defines, aliases resolved."""
    if root is None:
        return None
    enum_m = re.compile(r"^\s*(KC_[A-Z0-9_]+|QK_[A-Z0-9_]+)\s*=\s*(.+?)\s*,\s*$", re.M)
    defn_m = re.compile(r"^\s*#define\s+(KC_[A-Z0-9_]+|QK_[A-Z0-9_]+)\s+(\S.*?)\s*$", re.M)

    raw = {}
    for name in ("keycodes.h", "quantum_keycodes.h"):
        text = (root / "quantum" / name).read_text()
        for m in enum_m.finditer(text):
            raw.setdefault(m.group(1), m.group(2))
        for m in defn_m.finditer(text):
            raw.setdefault(m.group(1), m.group(2))

    resolved = {}

    def value(expr, depth=0):
        if depth > 8:
            raise ValueError("alias loop")
        e = expr.strip()
        if e.startswith("0x"):
            return int(e, 16)
        if re.fullmatch(r"\d+", e):
            return int(e)
        if e in raw:
            return value(raw[e], depth + 1)
        raise ValueError(e)

    for name, expr in raw.items():
        try:
            resolved[name] = value(expr)
        except (ValueError, RecursionError):
            pass
    return resolved


def token_value(tok, keycodes):
    """QMK value of one keymap.c token, or raise."""
    t = tok.strip()
    if t in ("KC_NO", "KC_TRNS"):
        return 0x0000 if t == "KC_NO" else 0x0001
    m = re.fullmatch(r"MO\((\d+)\)", t)          # momentary layer tap
    if m and keycodes is not None:
        return keycodes["QK_MOMENTARY"] | (int(m.group(1)) & 0x3F)
    m = re.fullmatch(r"(LT\((\d+),\s*)?([A-Z][A-Z0-9_]+)\)?", t)
    if m and m.group(1) and keycodes is not None:
        return (keycodes["QK_LAYER_TAP"] | (int(m.group(2)) & 0xFF) << 8
                | keycodes[m.group(3)])
    if keycodes is None:
        raise ValueError("no QMK tree to resolve against")
    if t not in keycodes:
        raise ValueError(f"unknown token {t}")
    return keycodes[t]


def firmware_layers(rows, cols):
    src = KEYMAP_C.read_text()
    layers = []
    for n in range(len(re.findall(r"\[\d+\] = LAYOUT\(", src))):
        m = re.search(rf"\[{n}\] = LAYOUT\((.*?)\n    \),", src, re.S)
        if not m:
            sys.exit(f"layer {n} declared but not parsable in {KEYMAP_C}")
        toks = [t.strip() for t in m.group(1).split(",") if t.strip()]
        if len(toks) != rows * cols:
            sys.exit(f"layer {n}: {len(toks)} tokens, expected {rows * cols}")
        layers.append(toks)
    return layers


def defines(path, name):
    m = re.search(rf"#define {name}\s+(\w+)", path.read_text())
    if not m:
        sys.exit(f"{name} not found in {path}")
    return int(m.group(1))


class Via:
    def __init__(self, node):
        self.node = node
        # non-blocking: the device only answers when it feels like it, and a
        # blocking read would hang instead of letting us retry for a
        # self-identifying frame
        self.fd = os.open(node, os.O_RDWR | os.O_NONBLOCK)
        self.stale = 0
        self.junk = 0
        self.reads = 0

    def close(self):
        os.close(self.fd)

    def get(self, cmd, *args):
        """Write one command; return the first reply that identifies itself."""
        if cmd in FORBIDDEN:
            raise RuntimeError(f"refusing to send cmd {cmd:#04x}: {FORBIDDEN[cmd]}")
        out = bytearray(REPORT_LEN)
        out[1] = cmd
        out[2:2 + len(args)] = args
        os.write(self.fd, bytes(out))
        for _ in range(READ_RETRIES):
            time.sleep(SETTLE_S)
            # drain everything queued, then keep it -- the self-identifying
            # frame may not be the first one (see module docstring)
            frames = bytearray()
            for _ in range(8):
                try:
                    frames += os.read(self.fd, WIRE_READ_LEN)
                except BlockingIOError:
                    break
            self.reads += 1
            if len(frames) < WIRE_READ_LEN:
                continue
            r = bytearray(frames[:WIRE_READ_LEN])
            if r[0] == ID_UNHANDLED:
                self.junk += 1
                continue
            if r[0] != cmd:                      # another command's reply
                self.stale += 1
                continue
            if bytes(r[1:1 + len(args)]) != bytes(args):   # wrong cell
                self.stale += 1
                continue
            return r
        raise TimeoutError(f"no self-identifying reply to cmd {cmd:#04x} "
                           f"args {list(args)}")

    def protocol_version(self):
        r = self.get(ID_GET_PROTOCOL_VERSION)
        return (r[1] << 8) | r[2]

    def value(self, vid):
        return int.from_bytes(bytes(self.get(ID_GET_KEYBOARD_VALUE, vid)[2:6]), "big")

    def keycode(self, layer, row, col):
        r = self.get(ID_DYNAMIC_KEYMAP_GET_KEYCODE, layer, row, col)
        return (r[4] << 8) | r[5]


def main(argv):
    node = argv[1] if len(argv) > 1 else "/dev/hidraw2"
    root = qmk_root()
    keycodes = parse_keycodes(root)
    if keycodes is None:
        print("warning: no QMK tree found above this script; keycode values "
              "cannot be resolved, only raw wire values will be reported",
              file=sys.stderr)

    rows, cols = defines(CONFIG_H, "MATRIX_ROWS"), defines(CONFIG_H, "MATRIX_COLS")
    layers = firmware_layers(rows, cols)

    print(f"node: {node}")
    if keycodes:
        print(f"keycode values from: {root / 'quantum/keycodes.h'}")
    print()
    try:
        via = Via(node)
    except PermissionError:
        print(f"error: cannot open {node} for read/write.\n"
              "  This is the udev rule from Architecture.md §5, not a firmware fault:\n"
              "  echo 'KERNEL==\"hidraw*\", ATTRS{idVendor}==\"9d5b\", "
              "ATTRS{idProduct}==\"2406\", MODE=\"0660\", GROUP=\"plugdev\", "
              "TAG+=\"uaccess\"' | sudo tee /etc/udev/rules.d/99-dusk67.rules\n"
              "  sudo udevadm control --reload-rules && sudo udevadm trigger")
        return 2

    try:
        proto = via.protocol_version()
        print(f"protocol version : {proto}  ({'ok' if proto == 13 else 'EXPECTED 13'})")
        print(f"uptime           : {via.value(0x01)} ms")
        print(f"layout options   : {via.value(ID_LAYOUT_OPTIONS):#010x}"
              f"  (firmware {via.value(0x04):#010x})")
        print(f"matrix           : {rows} rows x {cols} cols, "
              f"{len(layers)} layer(s) in keymap.c")
        print()

        checked = bad = 0
        for l, toks in enumerate(layers):
            print(f"layer {l}")
            for r in range(rows):
                for c in range(cols):
                    want_tok = toks[r * cols + c]
                    try:
                        got = via.keycode(l, r, c)
                    except TimeoutError as e:
                        print(f"  ({r},{c}) ERROR   {e}")
                        bad += 1
                        continue
                    checked += 1
                    try:
                        want = token_value(want_tok, keycodes)
                    except (ValueError, KeyError) as e:
                        print(f"  ({r},{c}) SKIP    {want_tok}: {e}")
                        continue
                    if got != want:
                        print(f"  ({r},{c}) MISMATCH  want {want_tok} "
                              f"(0x{want:04x})  got 0x{got:04x}")
                        bad += 1
            print(f"  {rows * cols} cells read")
            print()

        print(f"frames read: {via.reads}  "
              f"stale (other command/cell): {via.stale}  "
              f"unhandled (0xFF): {via.junk}")
        if bad:
            print(f"FAILED: {bad} problem(s) over {checked} cells read")
            return 1
        print(f"OK: all {checked} cells match keymap.c")
        return 0
    finally:
        via.close()


if __name__ == "__main__":
    sys.exit(main(sys.argv))