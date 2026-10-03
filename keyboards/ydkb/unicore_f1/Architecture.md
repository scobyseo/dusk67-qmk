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

The two ad-hoc static checks are also wired to make targets, so they take a
reference file the same way `check_*.py` do when called directly:

```bash
make ydkb/unicore_f1:dusk67_via via_json     VIA_JSON=../docs/dusk67_via.json
make ydkb/unicore_f1:dusk67_via keymap_check KEYMAP_EXPORT=../docs/dusk67.layout.json
```

There is a third ad-hoc check, and it is the only one that touches hardware —
`via_readback.py` reads the EEPROM keymap off the board and diffs it against
`keymap.c`:

```bash
python3 keyboards/ydkb/unicore_f1/via_readback.py [/dev/hidraw2]
```

It needs the udev rule (§5) and the keyboard plugged in. It resolves expected
keycode values by parsing this QMK tree's own `quantum/keycodes.h`, so it has
no hand-written keycode table to drift out of sync. See §7 for what it found
and what it cost.

Both `check_*` scripts are **ad-hoc**, not upstream CI. `check_keymap.py` guards
the hand transcription from `dusk67.layout.json`; it is what caught a missing
`KC_APPLICATION` alias and a dropped matrix row during development.
`via_readback.py` is the hardware-attached third check (§3, §7.1).

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
| `0x02 0x02` | layout options (value id) | layout-option bits |
| `0x02 0x04` | firmware version | `0x00 0x00 0x00 0x01` = `VIA_FIRMWARE_VERSION` |
| `0x04 <layer> <row> <col>` | dynamic keymap get | keycode at bytes 4–5, big-endian |

**`0x02 0x03` is `id_switch_matrix_state`, not layout options.** The layout
options value id is **0x02** (`quantum/via.h`, `enum via_keyboard_value_id`).
Reading `0x03` returns the matrix state word and looks like a plausible
all-zero answer, which is exactly how the wrong id survives review. Earlier
revisions of this section used `0x03`; the `0x03000000` figure recorded in §7
came from that mistake and should not be cited.

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
   a `group,option` legend; the configurator app draws a different key per option
   value. Verified against `@the-via/reader`'s `kle-parser.ts` (TypeScript in the
   current tree, not `.js`). So Split Backspace / ISO Enter / Split LShift /
   Space Row are all *app-side* and need no firmware bit; only CapsLock Color is
   decoded in firmware (`led.c`, 3 bits x 3 indicators). **Do not go implement
   firmware decoders for 0–3.**

   The transport framing was cross-checked against `the-via/app` itself
   (`src/shims/node-hid.ts`, `src/utils/keyboard-api.ts`) — see §7.2's framing
   table. The app and this document agree: 33 padded bytes plus a leading
   report-ID byte, and replies are matched on an echo of both the command and its
   arguments.

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

6. **"The bulk readback scramble was a device/EEPROM fault" — FALSE.** It was
   entirely the host probe. Two separate host-side bugs, both measured
   (2026-10-03):
   - **`/dev/hidraw*` returns 32 bytes, not 34.** hidapi strips the report ID,
     so a 34-byte VIA report reads as 32. A 34-byte `read()` therefore
     short-reads or blocks, and every reply gets rejected — which looks
     identical to "the board is not answering".
   - **The layout-options value id is 0x02, not 0x03** (see §4). `0x03` is
     `id_switch_matrix_state`.
   Once replies are matched on *both* the command echo and the echoed
   (layer,row,col), all 224 cells read cleanly, repeatably, with **zero** stale
   frames out of 228. The EEPROM was never scrambled.

7. **The Vial export is not a suspect for EEPROM drift.** Checked rather than
   assumed: `check_keymap.py` passes (every token in `keymap.c` matches
   `dusk67.layout.json`), the only `d:true` decal in `layouts.keymap[0]`
   applies to the single entry `'0,0\n\n\n4,0'`, and cell (0,0)'s token
   `KC_GESC` → `QK_GESC` = `0x7C16` is present **in flash** (verified by
   disassembling the `keymaps` symbol at `0x0800abb8`). A hand transcription
   cannot put a wrong value into EEPROM; it can only make the firmware disagree
   with the export.

## 7. Hardware validation (2026-10-03, after flashing)

The keyboard was reflashed with `dusk67_via_prod.uf2` and re-enumerated as
`Bus 001 Device 014: ID 9d5b:2406`. The flashed UF2 is **byte-identical** to a
fresh build of the committed tree (md5 `719bd42bcb074dcce855703d85523506`, bin
`93dfabcb4d5e012f9f41cbaba2540a3b`, 30564 B) — re-measured this session, so
"the board is running this source" is established, not assumed.

Measured on the live device:

- `get_protocol_version` -> **0x0D (13)** on `/dev/hidraw2`
- `get_keyboard_value firmware_version` -> **0x00000001** (matches
  `VIA_FIRMWARE_VERSION` in `config.h`)
- `layout_options` (value id **0x02**) -> **0x00000040** — bit 6 set, i.e. the
  3-bit CapsLock-colour field reads "Blue". (`0x03000000` recorded in earlier
  revisions was a mis-read of value id `0x03`; see §4 and §6.6.)
- **`id_eeprom_reset` (0x0A) is not compiled in** — the reply is absent because
  `VIA_EEPROM_ALLOW_RESET` is not defined. That is the correct posture: it means
  the host cannot casually wipe this EEPROM. Recovery is via Esc-bootmagic.

### 7.1 EEPROM keymap readback — now trustworthy, and verified

The earlier claim "EEPROM readback is NOT yet trustworthy" was **wrong about the
cause** and has been resolved. The scrambling was the host probe, not the board:
`/dev/hidraw*` hands back **32** bytes (hidapi strips the report ID), so a
34-byte read never matched. With the length fixed and every reply matched on
*both* the command echo and the echoed `(layer,row,col)`:

```
protocol version : 13  (ok)
frames read: 228  stale (other command/cell): 0  unhandled (0xFF): 0
OK: all 224 cells match keymap.c
```

**All 224 cells (layers 0-1 x 14 rows x 8 cols) match `keymap.c`, repeated
3x consecutively with zero stale frames.** So "the EEPROM holds the keymap"
is now **confirmed**, not assumed. Run it yourself:

```bash
python3 keyboards/ydkb/unicore_f1/via_readback.py
```

Ground truth for "what should be there" is the ELF, not the source: `keymaps`
lives at `0x0800abb8`, and the disassembly confirms every cell (e.g. (0,0) =
`16 7c` little-endian = `0x7C16` = `QK_GESC`).

Wire format, measured and confirmed against `quantum/via.c`:

```
out : [0]=0x00  [1]=cmd  [2..4]=args
in  : [0]=cmd   [1..3]=args echoed   [4..5]=keycode (big-endian)
```
The keycode args are **(layer, row, column)** — *not* a flat offset. An early
probe passed a flat offset and decoded bytes [4:6] and produced confident
garbage; treat any readback script that does not echo-check as unverified.

### The buffer commands are offset-relative — this caused a false alarm

`0x12`/`0x13` take a different layout: `offset` is **2-byte big-endian** at
`[2..3]`, `size` (max 28) at `[4]`, and the reply's payload starts at index 4
(`via.c` writes it at `&command_data[3]`, which is `data[4]`, and hidapi has
already stripped the report-ID byte at `data[0]`).

The decisive detail, measured: **the offset is relative to
`DYNAMIC_KEYMAP_EEPROM_START`, not to the start of EEPROM.** Reading a cell via
`0x12` needs `(layer*112 + row*8 + col)*2` — adding the base address instead
reads a *different cell's* bytes.

Getting this wrong made all 8 previously-broken cells look wrong through the
buffer while `0x04` reported them correct, which briefly looked like evidence of
a decoding artifact in the defect itself. It was an artifact of the probe. With
relative offsets, `0x12`, `0x04` and flash now agree on all 8 cells.

**The base is 42, and it was 42 all along.** An earlier revision of this file
called 264 "measured" — that was wrong, and the error is worth recording because
it is the same class of mistake twice: a sentinel scan was fed an *absolute*
offset to a command that takes a *relative* one, so the "measured" 264 was just
the cell index, not an address. The EEPROM driver's own trace settles it: a
`0x06` reset's very first keymap read is at EEPROM `0x2a` = 42 and returns
`0x7C16`, which is exactly `keymaps[0][0][0]` (`QK_GESC`). 42 is what
`nvm_eeprom_via_internal.h`'s macro chain predicts
(`EECONFIG_SIZE` 37 + 3 magic + 2 layout options), so the macros were right and
the measurement was wrong.

### 7.2 An EEPROM reset command damaged 8 cells — do not send 0x06 casually

While diagnosing, I sent `id_dynamic_keymap_reset` (**0x06**) to the live board.
It reseeds the keymap from flash, and it left **8 cells wrong**:

| cell | before/after reset | flash (correct) |
|---|---|---|
| L0 (8,1) | `0x0000` | `0x0046` KC_PRINT_SCREEN |
| L1 (5,0) | `0x00a9` | `0x004d` KC_END |
| L1 (5,1) | `0x00aa` | `0x004e` KC_PGDN |
| L1 (5,2) | `0x00a8` | `0x004a` KC_HOME |
| L1 (5,6) | `0x0001` | `0x004b` KC_PGUP |
| L1 (8,0) | `0x0001` | `0x0039` KC_CAPS_LOCK |
| L1 (8,1) | `0x0001` | `0x0047` KC_SCROLL_LOCK |
| L1 (8,2) | `0x0001` | `0x0053` KC_NUM_LOCK |

Properties measured about this, so it is not re-derived:

- The MCU does **not** reboot (uptime kept counting through the reset), so this
  is not a reset-interrupt artifact.
- Individual writes (`0x05`) work and do not alias: a `0xABCD` sentinel written
  to L1 (5,0) read back exactly and left L1 (6,0) and L0 (5,0) untouched.
- The pattern is **value-shaped, not position-shaped**: the wrong values are the
  *correct* keycodes for *neighbouring* keys (`0xa9`/`0xaa`/`0xa8` = VOLU/VOLD/
  MUTE are L1 row 6's values; `0x41`-`0x45` = F8-F12 also row 8). Some cells
  lost their value, others gained a row-6/row-8 value. Repeated resets
  reproduce the same 8 cells, so it is deterministic, not wear or noise.

**Root cause: still open.** What has been *ruled out* by measurement, so it is
not re-derived:

- **It does not reproduce on v0.2.** After flashing v0.2, command 0x06 was sent
  five times in a row from a verified-clean board; every time all 224 cells
  matched flash afterwards. The "8 cells always break" claim was measured under
  **v0.1** and does not carry over unchanged. Something differs between the two
  builds or between the EEPROM states they inherited, and that difference is
  still unidentified — so this is *unexplained*, not *fixed*.
- **A sentinel cell is written correctly.** Writing `0xBEEF` into one of the
  affected cells and then sending 0x06 restores the correct flash value, so the
  reset's own `dynamic_keymap_set_keycode()` writes do reach those cells. That
  rules out "the corruption happens outside the keymap write path" and localises
  the fault to what differs *before* the write — i.e. what the EEPROM held or
  which byte the write targeted.

- **Not an EEPROM capacity overflow.** QMK's own assert passes. Measured EEPROM
  geometry (STM32F103xB, `FEE_PAGE_SIZE` 0x400 x `FEE_PAGE_COUNT` 8, from
  `config.h`): `FEE_PAGE_BASE_ADDRESS` = `0x0801E000` (matches the linker's 8 KB
  reservation exactly), compacted area `0x0801E000..0x0801E800` (2048 B),
  write log `0x0801E800..0x08020000` (6144 B). `DYNAMIC_KEYMAP_EEPROM_START` is
  **42** (see the correction in §7.1), so the highest cell byte address is
  42 + 1343 = 1385, comfortably inside 2048.
- **Not the 13-bit write-log address field.** `eeprom_write_log_word_entry`
  masks `address &= 0x1FFF`, which looked like a limit at first. Max address
  here is 1385 (12 bits after the `<<1`), so nothing is truncated.
- **Not the `FEE_BYTE_RANGE` (0x80) byte-log/word-log split.** All keymap
  addresses are >= 42, and the word-log path is used from 0x80 up; only a handful
  of cells fall in the byte-log range, which cannot explain 8 specific cells.
- **Not byte-order or a word-swap inside the cell.** In all 8 failures the high
  byte is correct and only the **low** byte is wrong — so it is not an
  endianness bug in `nvm_dynamic_keymap`'s big-endian store.
- **Not a misaligned/offset read.** Sweeping +-3 cells around each failing
  index in flash does not reproduce the observed values.
- **Not a reboot or an interrupted write.** Uptime keeps counting across the
  reset; the MCU does not restart.

Two further facts worth keeping:

- **Only the low byte is ever wrong**, and the wrong values are *plausible
  keycodes from elsewhere in the same layer* (`L1(5,0)`=`0xa9`=VOLU which lives
  at `L1(6,5)`; `L1(5,1)`=`0xaa`=VOLD which lives at `L1(6,4)`; `L1(5,2)`=`0xa8`
  =MUTE which lives at `L1(5,4)`). Several others simply read `0x0001`. It looks
  like *stale or misattributed* data rather than erasure.
- **`id_eeprom_reset` (0x0A) is not compiled in** (`VIA_EEPROM_ALLOW_RESET`
  undefined), so the host cannot wipe EEPROM wholesale — only 0x06 is reachable.
  Confirmed on the live v0.2 board: 0x0A draws no self-identifying reply and
  uptime does not move; all 224 cells still matched afterwards.

**Who can actually reach the defect — checked against the app's own source,
not against my own tooling.** This matters: judging the protocol by a script I
wrote myself would be circular, so the reference here is `the-via/app`, the
Configurator itself (`src/utils/keyboard-api.ts`, read 2026-10-03):

```ts
enum APICommand {
  DYNAMIC_KEYMAP_SET_KEYCODE = 0x05,
  //  DYNAMIC_KEYMAP_CLEAR_ALL = 0x06,     <-- COMMENTED OUT
  EEPROM_RESET                = 0x0a,
  BOOTLOADER_JUMP             = 0x0b,
}
async resetEEPROM() { await this.hidCommand(APICommand.EEPROM_RESET); }
```

Two things follow, and the second one is stronger than I expected:

1. **The Configurator's "Reset EEPROM" sends 0x0A, and 0x0A is not compiled in
   here.** So an app user cannot trigger this defect at all.
2. **0x06 is not merely unused by the app — it is deliberately commented out of
   the app's command enum.** The project that owns this protocol has already
   marked "clear all dynamic keymap" as a command the configurator must not
   send. The exposure is therefore limited to hand-written raw-HID scripts.

That is the honest scope: a real hazard for anyone probing the board by hand,
not a user-facing data-loss path. It also means the fix belongs in the handler
and the tools, not in a user-facing warning.

### Cross-checked against the Configurator's own framing

My readback tool is not the yardstick; `the-via/app` is. The two agree:

| | Configurator (`node-hid.ts` / `keyboard-api.ts`) | `via_readback.py` |
|---|---|---|
| report size | 33 padded + 1 report-ID byte | 34 written, 32 read |
| write path | `sendReport(0, new Uint8Array(arr.slice(1)))` | writes byte 0, reads it back stripped |
| byte 0 | `COMMAND_START = 0x00`, "really a HID Report ID" | treated as the report id, not a field |
| reply match | `eqArr(commandBytes.slice(1), buffer.slice(0, len-1))` — compares **command and args** | `reply[0]==cmd && reply[1..3]==args` |
| stale frames | `fastForwardGlobalBuffer(lastWriteTimestamp)` drops pre-write timestamps | a reply that fails the echo match is retried |

So the Configurator **does** validate the echo on command *and* arguments, which
is what made the 224-cell readback trustworthy here. One difference worth
recording: it filters stale frames by **timestamp** while this tool filters by
**echo mismatch**; the echo check is strictly stronger, since a queued frame
from an earlier request can share a timestamp window.

### The compaction hypothesis is now DISPROVED (driver trace, 2026-10-03)

Built with `CONSOLE_ENABLE=yes EXTRAFLAGS="-DEEPROM_TRACE_PROBE -DSOF_TRIM_DEBUG"`
plus a one-line `#define DEBUG_EEPROM_OUTPUT` in the driver (both reverted
afterwards; the submodule is pristine and `led.c` carries no probe). Console
output arrives on the 4th HID interface, `/dev/hidraw4`.

Observed across one `0x06` on the v0.2 board:

| driver event | count |
|---|---|
| `EEPROM_ReadDataByte` | 58+ (contiguous, `0x2a`..`0x63`) |
| read bytes differing from flash | **0** |
| `FLASH_ProgramHalfWord` | **0** |
| write-log entries (byte or word) | **0** |
| `[DIRECT]` compacted-area writes | **0** |
| `eeprom_compact()` / `eeprom_clear` | **0** |
| `[SKIP SAME]` | 0 (suppression is silent in this driver) |

So `eeprom_update_byte()` found `orig == value` for every byte and **wrote
nothing at all**. With no writes there is no write log, no log pressure and no
compaction — which is precisely why 0x06 does not reproduce on v0.2. **The
compaction hypothesis is dead.**

This also means the v0.1 corruption, if it was ever real, came from a state in
which those bytes *did* differ — the reset then had work to do and took a path
this trace never exercised. The trigger for that difference is still unknown.

### What this trace did NOT establish

**The log is lossy.** The EEPROM stream ends in a torn fragment (`EEPROM 0x01`)
and the console pads each message into its own HID report (812 zero-pad runs in
61 KB), so report boundaries cut through lines and at least one report was never
delivered. **58 is a lower bound, not a total.** A full 6x14x8 reset would issue
1344 reads, so it is not established that the reset visited more than 29 cells
or where it stopped.

The obvious fix is to rebuild *without* `-DSOF_TRIM_DEBUG`: the SOF-trim log
produced 811 of the 872 captured lines and is pure noise here, which is what
pushed the EEPROM output out of reports. That rebuild has not been done.

Also note `via_eeprom_is_valid()` encodes `QMK_BUILDDATE` as BCD **YYMMDD**
(`quantum/via.c:71`, indices 2-3/5-6/8-9 = year, month, day) — so the magic
only changes when the **calendar date** changes, not per build. A same-day
reflash therefore does *not* auto-trigger `eeconfig_init_via()`. The magic bytes
cannot be read back through any command this firmware exposes, so this is
inferred from the source, not measured on the board.

**Repaired** by writing the 8 flash values back with `0x05`; all 224 cells
verify clean across repeated runs (`via_readback.py` green). **Treat `0x06` as
unsafe on this keyboard** until the cause is understood — it is not a recovery
tool here. If the keymap is ever genuinely lost, use Esc-bootmagic instead.

## 7b. Current state (as of 2026-10-03)

- Keymap `dusk67_via` **mirrors the user's Vial export** `dusk67.layout.json`
  exactly on layers 0 and 1 (112 tokens each, 0 diffs; `check_keymap.py` green).
  Layers 2-5 are empty in the export and intentionally unimplemented in
  firmware — though `DYNAMIC_KEYMAP_LAYER_COUNT` is 6, so the VIA app *will*
  offer 6 layers and layers 2-5 are simply blank.
- The export's Vial-only short keycodes were mapped to QMK names:
  `KC_HAEN->KC_LANGUAGE_1`, `KC_HANJ->KC_LANGUAGE_2`, `KC_SLCK->KC_SCROLL_LOCK`,
  `KC_NLCK->KC_NUM_LOCK`, `KC_GESC->QK_GESC`, `KC_VOL_UP/DOWN->KC_VOLU/KC_VOLD`.
- `KC_PRINT_SCREEN` was added at `(8,1)` — a position `keyboard.json` already
  labelled `BL_TOGG` but the firmware left `KC_NO`.
- Tree is rebased to **`origin/master` tip** (`7a1bbf37c5`), 0 ahead / 0 behind.
  Build + lint + both static checkers green after the rebase.
- Flashed (v0.2), VIA-live, **EEPROM keymap readback verified against the ELF**
  (§7.1), and firmware updates are now hands-free (§10).
- Outstanding: the `0x06` defect in §7.2. It **does not reproduce on v0.2**, and
  a driver trace explains why — the reset performs **zero writes** because every
  byte already matches flash, so there is no write log and no compaction. The
  compaction hypothesis is disproved. What made those bytes differ under v0.1
  is still unknown, so this is unexplained, not fixed. Treat 0x06 as unsafe
  until that is established.
- The EEPROM keymap base is **42** (`EECONFIG_SIZE` 37 + 3 magic + 2 layout
  options). An earlier claim of a "measured 264" was a units error — an absolute
  offset passed to the offset-relative `0x12` — and is corrected in §7.1.

## 8. If you are handed a new bug while using the keyboard

Fast triage, in order:

1. Is it *reproducible with the board unplugged and replugged*? If yes, likely
   EEPROM/keymap, not runtime. Reflash (§3) and run `via_readback.py` (§7.1) —
   it names the exact cells that disagree with flash. Do **not** try to fix it
   by sending VIA `0x06`; it corrupted cells under v0.1 and the cause is still
   unknown (§7.2), even though it does not currently reproduce under v0.2.
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

## 10. Scripted flashing (added 2026-10-03, firmware v0.2)

### Why Esc could not simply be automated

Esc works through `bootmagic`, which samples the matrix during boot. There is no
way to inject it from the host: interface 0 is a **boot-protocol keyboard**
(`bInterfaceSubClass=1`, `bInterfaceProtocol=1`), so the Linux HID driver claims
boot reports itself and consumes them as key events. Writing a boot report to
`/dev/hidraw*` produces an Esc *keystroke*, not a matrix state the firmware can
see. This is host policy, not something configurable away.

### The way out: `id_bootloader_jump` (0x0B)

`via.h` defines `id_bootloader_jump = 0x0B`, but **`quantum/via.c` has no case
for it** in `raw_hid_receive` — the command is never dispatched, so a host that
sends it gets `id_unhandled` (0xFF) and the board stays put. Meanwhile
`led.c` already implements `bootloader_jump()` (magic `0x9d5bfc2b` at
`0x20004000`, then `NVIC_SystemReset()`).

QMK provides a sanctioned join point for exactly this: `raw_hid_receive` calls
`via_command_kb(data, length)` *before* its own switch and returns early when it
returns true. So a keyboard-level override bridges the two **without touching
upstream QMK** — no submodule edit, no fork, consistent with §9:

```c
bool via_command_kb(uint8_t *data, uint8_t length) {
    if (data[0] != id_bootloader_jump) return false;
    raw_hid_send(data, length);
    wait_ms(20);
    bootloader_jump();
    return true;   /* not reached */
}
```

Verified in the linked image, not just the source: `via_command_kb` is a strong
`T` symbol (the weak default in `via.c` is overridden), and `raw_hid_receive`
disassembles to `bl via_command_kb` followed by `cmp r3, #11`.

### Do not depend on the reply

The firmware queues a reply and then resets within microseconds, while USB polls
the endpoint only every 1 ms — so a reply is **not guaranteed** to leave the
board.

**Measured 2026-10-03: it did arrive** (`reply[0] = 0x0b`) on this host. An
earlier revision of this section asserted it could not, which was an over-claim
from reasoning about poll timing rather than observation; the measurement
corrected it. The design does not change — `flash-dusk67.sh` still treats
"the device disappeared from the bus" as the success signal, because a reply
that depends on poll alignment is not something to build a flash on, and because
a *missing* reply would be indistinguishable from a reset that never happened.

### The one-time manual step

`0x0B` only exists in **v0.2 and later**. On v0.1 the command is ignored, so the
script cannot work. Therefore:

1. hold **Esc** while plugging in, and flash the v0.2 `.uf2` by hand (once);
2. from then on `./flash-dusk67.sh` needs no hands at all.

This looks circular but is not: the first manual flash installs the very handler
that automates every later flash.

### Measured on hardware (2026-10-03)

v0.2 was flashed once by hand, then the script ran **unattended six times**, each
time ending with the board back at `9d5b:2406` and all 224 EEPROM cells
matching `keymap.c`.

Three things were guessed wrong and corrected by measurement:

1. **The bootloader's USB id is not a `9d5b` id.** It enumerates as
   **`1209:db42`** — "Generic Devan Lai dapboot DFU bootloader" (picode's
   generic VID/PID), exposing a 3.9 MB vfat volume labelled **`UniCore-F1`**.
   An earlier draft carried a guessed `9d5b:0000`; it was wrong.
2. **The USB device appears before its block node.** Waiting for the USB id is
   not sufficient — the first run failed because `bootloader_device()` was
   called once, immediately, before `/dev/sdX` existed. The script now polls
   for the volume itself.
3. **`--uf2` deleted the caller's own file.** The EXIT cleanup referenced a
   `main()` local after `main()` had returned (an unbound-variable error under
   `set -u`), and then the same cleanup was pointed at the user-supplied path.
   The artifact path is now file-scoped and is only recorded for deletion when
   *we* made the file.

Mounting uses `udisksctl`, so no root is needed. Verified properties worth
keeping: a caller-supplied `--uf2` is never deleted; the build path's own temp
file always is; both paths exit 0.
