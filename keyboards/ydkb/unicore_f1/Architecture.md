# Dusk67 / UniCore-F1 — working architecture & debugging notes

Written 2026-10-03. This is the **operational** document: what the current
firmware is, how it is built and verified, and which past conclusions were
wrong. It is meant to be picked up cold, without the session that produced it.

Read `USB_SOF_TRIM_PLAN.md` for the USB tear-off fix (separate concern, older).
`readme.md` is the user-facing user manual. This file is the maintainer log.

---

## 1. Where everything lives

Two checkouts exist; this repo is the source of truth for the keyboard files.

| What | Path |
|---|---|
| **This repo (published)** | `~/work/dusk67-uf2/` — keyboard + docs + QMK submodule |
| Keyboard inside QMK | `qmk_firmware/keyboards/ydkb/unicore_f1/` (a copy; see below) |
| My primary dev tree | `~/work/qmk_firmware/keyboards/ydkb/unicore_f1/` (untracked) |
| Build artifacts / UF2 | `~/work/dusk67-uf2/*.uf2` |
| VIA definition | `docs/dusk67_via.json` here; side-loaded into the VIA app |
| Vial app layout export | `docs/dusk67.layout.json` |
| ARM toolchain | `~/work/toolchain-arm-gnu-13.2/bin` (**must be on PATH**) |

The keyboard must sit *inside* the QMK tree for `make` to find it. Here that is
the submodule at `qmk_firmware/`, so `keyboards/ydkb/unicore_f1/` is copied into
it after a fresh submodule checkout:

```bash
cp -r keyboards/ydkb/unicore_f1 qmk_firmware/keyboards/ydkb/
```

Keep `keyboards/ydkb/unicore_f1/` and the copy in `qmk_firmware/` in sync — the
submodule is pinned upstream and will not carry our files.

QMK is pinned to **`7a1bbf37c5`** ("Add support for mini_mighty/kbd", tag
`0.34.6-1`), the commit this port was last built and verified against.

## 2. What the firmware is

- Board: KBDFans **Dusk67**, UniCore-F1 controller, STM32F103CBT6, wired USB.
- Upstream-QMK-native. **VIA protocol 13, zero Vial dependency** (this is the
  whole point of the port — the VIA Configurator app can drive it).
- Matrix: 14x8, rows 0–9 populated, 10–13 padding the apps expect to see.
- 6 dynamic keymap layers, `VIA_EEPROM_LAYOUT_OPTIONS_SIZE 2` (9 bits used).
- No HSE crystal; 48 MHz USB clock derives from HSI (see the tear-off note).

## 3. Build & verify (the commands that actually matter)

```bash
export PATH=~/work/toolchain-arm-gnu-13.2/bin:$PATH
cd qmk_firmware

make ydkb/unicore_f1:dusk67_via     # -> .build/ydkb_unicore_f1_dusk67_via.bin
qmk lint -kb ydkb/unicore_f1        # -> "Lint check passed!"
```

The two ad-hoc checks are also wired to make targets, so they take a reference
file the same way `check_*.py` do when called directly:

```bash
make ydkb/unicore_f1:dusk67_via via_json     VIA_JSON=../docs/dusk67_via.json
make ydkb/unicore_f1:dusk67_via keymap_check KEYMAP_EXPORT=../docs/dusk67.layout.json
```

Both `check_*` scripts are **ad-hoc**, not upstream CI. `check_keymap.py` guards
the hand transcription from `dusk67.layout.json`; it is what caught a missing
`KC_APPLICATION` alias and a dropped matrix row during development.

Flash artifact (regenerate after every change):
```bash
python3 util/uf2conv.py .build/ydkb_unicore_f1_dusk67_via.bin \
    -b 0x8004000 -c -f 0x9d5bcf10 -o ~/work/dusk67-uf2/dusk67_via_prod.uf2
# hold Esc while plugging in, then drop the .uf2 on the mounted drive
```

### Ad-hoc vs canonical — do not conflate these

There is **no canonical test suite** for this keyboard. What exists:

- **Canonical**: `make` (build) and `qmk lint` — both must be green.
- **Ad-hoc**: `check_via_json.py` — a purpose-written static cross-check of the
  VIA definition against the firmware sources. It is *not* upstream QMK CI.
- **Ad-hoc, hardware**: the raw-HID protocol probe in §7.

A green ad-hoc script next to a red canonical test must be reported as
exactly that, never as "done".

## 4. Live VIA protocol (this was the hard-won unlock)

With `/dev/hidraw2` readable (see §5), the keyboard speaks VIA directly. The
report is 34 bytes; **byte 0 of the payload is the command**, and responses
echo it. Measured working commands:

| cmd | meaning | measured reply |
|---|---|---|
| `0x01` | get protocol version | `0x0D 0x00` = **13** |
| `0x02 0x01` | uptime | non-zero once running |
| `0x02 0x03` | layout options (value id) | layout-option bits |
| `0x04 <layer> <offset>` | dynamic keymap get | keycode at bytes 4–5, big-endian |

Worked example:
```python
b = bytearray(34); b[0] = 0x00; b[1] = 0x04; b[2] = 0; b[3] = 0
os.write(fd, bytes(b))                       # then read 34 bytes back
# reply[0]=0x04 cmd echo, reply[4:6] = keycode, big-endian
```

**Framing trap:** the first attempt at this read garbage (`0x00 0x02 0x04 …`
patterns) because I sent the command in the wrong byte offset and mis-sized
the keycode field. The response *always* echoes `reply[0] == command`. If that
does not hold, the framing is wrong — do not trust any payload you decoded.

## 5. Environment facts (needed to reproduce any of this)

- **udev rule is required** or the board is visible but unusable:
  `lsusb` lists it, `lsusb -v` prints "Couldn't open device". The node is
  `0664 root:root` and an unprivileged process has `CapEff=0`.
  Fix (needs interactive `sudo` — I cannot apply it from the agent shell):
  ```bash
  echo 'KERNEL=="hidraw*", ATTRS{idVendor}=="9d5b", ATTRS{idProduct}=="2406", MODE="0660", GROUP="plugdev", TAG+="uaccess"' \
    | sudo tee /etc/udev/rules.d/99-dusk67.rules
  sudo udevadm control --reload-rules && sudo udevadm trigger
  ```
  Result: `crw-rw---- root plugdev /dev/hidraw2`, `lsusb -v` opens cleanly.
- `sudo` on this host needs **interactive auth** (`sudo -n` fails). Agent shell
  cannot escalate. Hand the user exact commands.
- Real IME is **kime**, not fcitx5 — do not touch fcitx5 packages.
- `lib/chibios` is a **git submodule**; the `MAPLEMINI_STM32_F103` board lives
  inside it, not in `platforms/`. A plain `git ls-tree` on the superproject
  will not show it — that is expected, not a missing-board bug.

## 6. Corrections — past findings that were WRONG (do not repeat)

These were asserted earlier and then disproved by measurement. They cost real
time; the doc exists so they are not re-derived.

1. **"VIA/Vial does not communicate with the keyboard" — FALSE.**
   The firmware was always fine. The host could not open the device (§5). Once
   udev was fixed, `get_protocol_version` returned `0x0D` immediately. Every
   past "rawhid transport fault" symptom was this permission problem.

2. **"4 of 5 layout options are inert / a firmware gap" — FALSE.**
   A VIA layout option does **not** require a firmware-side decoder. Keys carry
   a `group,option` legend; the configurator app draws a different key per
   option value. Verified against `@the-via/reader`'s `kle-parser.js`. So
   Split Backspace / ISO Enter / Split LShift / Space Row are all *app-side* and
   need no firmware bit; only CapsLock Color is decoded in firmware (`led.c`,
   3 bits x 3 indicators). **Do not go implement firmware decoders for 0–3.**

3. **KLE decals are NOT sticky.** A `{"d": true}` decoration object applies to
   **the next key only**. Treating `d` as sticky reclassifies every
   group/alias key as art and produces phantom "missing key" failures.

4. **"Every via_get_layout_options consumer is a missing decoder" — FALSE.**
   Grep the whole tree before concluding: e.g. other ydkb boards legitimately
   consume option bits. Scope the claim to *this* keyboard.

5. A `SOF_TRIM_DEBUG` log in an earlier tree referenced `g_rawhid_*` counters
   that only exist in the hand-patched Vial fork, so it never linked here. The
   current `usb_sof_trim.c` was fixed to read USB hardware registers directly
   and links on any QMK base. (Recorded in `USB_SOF_TRIM_PLAN.md` §.)

## 7. Hardware validation (2026-10-03, after flashing)

The keyboard was reflashed with `dusk67_via_prod.uf2` and re-enumerated as
`Bus 001 Device 014: ID 9d5b:2406`. Measured on the live device:

- `get_protocol_version` -> **0x0D (13)** on `/dev/hidraw2`
- `get_keyboard_value uptime` -> **152415 ms** (a fresh boot, not a stale one)
- `layout_options` -> **0x03000000** (non-zero; the 9 decoded bits are set)

So: **flash OK, VIA OK, protocol 13 OK.** That much is measured, not assumed.

**EEPROM readback is NOT yet trustworthy.** Individually-read cells returned the
right keycodes (`0x2c`=SPC, `0x11`=N, `0x10`=M, `0x36`=COMM, `0x90`=LNG1,
`0x65`=APP, `0x46`=PSCR, `0x1e`=KC_1, `0x20`=KC_3), but a full 18-row sweep in
one pass produced a scrambled result. The cause is on the **host** side: the
device returns stale queued frames, and a reply is identified by
`reply[0] == command` **and** `reply[1..3]` echoing the row/col you asked for.
Frames with `reply[0] == 0xFF` are junk. Validate both fields before trusting a
payload — a 10x read of one cell is stable, a fast bulk sweep is not.

Wire format, measured and confirmed against `quantum/via.c`:

```
out : [0]=0x00  [1]=cmd  [2..4]=args
in  : [0]=cmd   [1..3]=args echoed   [4..5]=keycode (big-endian)
```
The keycode args are **(layer, row, column)** — *not* a flat offset. An early
probe passed a flat offset and decoded bytes [4:6] and produced confident
garbage; treat any readback script that does not echo-check as unverified.

Because of this, "the EEPROM holds the new keymap" is still **unconfirmed**.
Confirm by one-off reads (or the VIA Configurator's own keymap editor) rather
than trusting a scripted bulk sweep. See §8 step 1.

## 7b. Current state (as of 2026-10-03)

- Keymap `dusk67_via` **mirrors the user's Vial export** `dusk67.layout.json`
  exactly on layers 0 and 1 (112 tokens each, 0 diffs). Layers 2–5 are empty in
  the export and are intentionally left unimplemented in firmware.
- The export's Vial-only short keycodes were mapped to QMK names:
  `KC_HAEN→KC_LANGUAGE_1`, `KC_HANJ→KC_LANGUAGE_2`, `KC_SLCK→KC_SCROLL_LOCK`,
  `KC_NLCK→KC_NUM_LOCK`, `KC_GESC→QK_GESC`, `KC_VOL_UP/DOWN→KC_VOLU/KC_VOLD`.
- `KC_PRINT_SCREEN` was added at `(8,1)` — a position `keyboard.json` already
  labelled `BL_TOGG` but the firmware left `KC_NO`.
- Tree is rebased to **`origin/master` tip** (`7a1bbf37c5`), 0 ahead / 0 behind.
  Build + lint + checker all green after the rebase.
- Flashed and VIA-live (§7). EEPROM keymap readback still unconfirmed (§7).

## 8. If you are handed a new bug while using the keyboard

Fast triage, in order:

1. Is it *reproducible with the board unplugged and replugged*? If yes, likely
   EEPROM/keymap, not runtime. Reflash (§3) and read back the keymap (§4).
2. Is USB dropping mid-typing? That is the **HSI tear-off** issue — go to
   `USB_SOF_TRIM_PLAN.md`, not here. It is a clock problem, and no VIA log will
   show it because a dead transport silences the log exactly when you need it.
3. Is VIA/Vial failing to connect *now that udev is fixed*? It genuinely was
   working (protocol 13) — re-run the §4 probe before believing a report.
4. Only after 1–3: suspect the matrix (`matrix.c`) or `keymap.c`. Use the
   `check_via_json.py` geometry checks — a key that "does nothing" is usually a
   key whose `(row,col)` is wrong or `KC_NO`.

## 9. Sharing this port (analysis, not yet done)

**Measured: the port modifies nothing tracked by upstream QMK.**
`git diff --stat origin/master` is **empty**. The whole thing is 23 files,
188 KB, confined to the untracked `keyboards/ydkb/unicore_f1/`. `lib/chibios`
is untouched (clean submodule), and `MAPLEMINI_STM32_F103` is an unmodified
upstream ChibiOS board.

That single fact decides the options:

**A separate repo, QMK as a submodule or tracked dependency — best fit.**
Clone `qmk/qmk_firmware` as a submodule (or pin a tag), drop this keyboard dir
beside it, and users get a self-contained checkout. This is the standard way to
share a keyboard port without forking. Nothing here needs a QMK core change, so
there is nothing to keep in sync with upstream — which is exactly why a fork
would buy nothing.

**A QMK user-space (`users/`) entry — does not fit.** QMK's `users/` space is
for *shared keymap code* across keyboards you already have in the tree. This is
a whole new keyboard with its own matrix, mcuconf and linker script; it belongs
in `keyboards/`.

**A full QMK fork — not worth it.** 0 tracked files differ. A fork would carry
102+ commits of unrelated upstream churn and a permanent rebase burden for
nothing.

**Upstreaming to qmk/qmk_firmware proper** is the other real option: `docs/
porting_your_keyboard_to_qmk.md` exists, the port is VIA-native and lint-clean,
so it would plausibly be accepted. Vendor/maintainer metadata in `keyboard.json`
would need to point at whoever is upstreaming it.

Practical note for any of these: the build needs
`~/work/toolchain-arm-gnu-13.2/bin` on PATH. The `check_*.py` scripts default to
reference files under `~/work/dusk67-uf2/` that a fresh clone will not have, so
both now **print `SKIP` and exit 0** when their reference file is absent — a
missing reference is not a firmware defect and must not look like a red build.
Both accept an explicit path argument, and the two cases are deliberately
different: an absent **default** prints `SKIP` and exits 0 (a fresh clone has
nothing to compare against, which is not a firmware defect), while an absent
**explicit argument** exits non-zero with `error: no such file:` — otherwise a
typo'd path would read as a silently green check.
