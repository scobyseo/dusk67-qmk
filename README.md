# dusk67-qmk

QMK firmware for the **KBDFans Dusk67** (UniCore-F1 / STM32F103CBT6), packaged
as a thin wrapper around upstream QMK rather than a fork.

This is a plain **VIA protocol 13** keyboard — no Vial dependency — so it works
with the VIA Configurator app.

## Why a wrapper, not a fork

This port changes **nothing** that upstream QMK tracks. The entire keyboard
lives in `keyboards/ydkb/unicore_f1/`, and no core file needs patching. A fork
would carry hundreds of commits of unrelated upstream churn and a permanent
rebase burden for no benefit.

So upstream QMK is a **git submodule**, pinned, and this repo holds only the
keyboard.

## Layout

```
qmk_firmware/                      submodule -> github.com/qmk/qmk_firmware
keyboards/ydkb/unicore_f1/          the keyboard (this repo's actual content)
docs/                               VIA definition + layout notes
```

## Getting started

```bash
git clone --recurse-submodules https://github.com/scobyseo/dusk67-qmk.git
cd dusk67-qmk
./sync-qmk.sh
```

`sync-qmk.sh` copies the keyboard into the submodule, because QMK only finds
keyboards inside its own tree and a submodule checkout wipes anything placed
there. Run it after any `git submodule update`; it verifies the copy rather
than trusting `cp`.

## Build

Requires an ARM toolchain (`arm-none-eabi-gcc`, tested with 13.2.Rel1) on your
`PATH`. Then:

```bash
cd qmk_firmware
make ydkb/unicore_f1:dusk67_via
```

The binary lands at `.build/ydkb_unicore_f1_dusk67_via.bin`. Convert and flash:

```bash
python3 util/uf2conv.py .build/ydkb_unicore_f1_dusk67_via.bin \
    -b 0x8004000 -c -f 0x9d5bcf10 -o dusk67_via.uf2
```

Hold **Esc** while plugging the keyboard in (bootmagic -> bootloader), then
drag the `.uf2` onto the mounted drive. That is the manual path.

### Scripted flashing (firmware v0.2+)

After v0.2, Esc is no longer needed — the repo can drive the whole update:

```bash
./flash-dusk67.sh              # build + flash
./flash-dusk67.sh --uf2 FILE   # flash a prebuilt .uf2
```

The firmware implements VIA `id_bootloader_jump` (**0x0B**) in `led.c`, which
upstream `quantum/via.c` defines in its enum but never dispatches. `0x0B` puts
the board into its UF2 bootloader; the script then copies the artifact onto the
bootloader's MSC volume.

**One manual step is required first.** `0x0B` does not exist in v0.1, so on v0.1
the command is ignored and the script cannot work. Flash v0.2 by hand once
(hold Esc); every update after that is fully scripted.

Verified on hardware: the script was run unattended six times, each time
rebuilding, jumping to the bootloader over raw-HID, copying the UF2 onto the
bootloader's MSC volume, and leaving the board back at `9d5b:2406` with all 224
EEPROM keymap cells matching the compiled keymap. See Architecture.md §10 for
the measured bootloader identity and the three things measurement corrected.

The board has **no HSE crystal** — the 48 MHz USB clock derives from the
internal HSI oscillator, which drifts outside the USB full-speed tolerance and
makes the keyboard randomly "tear off" from the OS. `usb_sof_trim.c` fixes this
in software by trimming HSI against the host's 1 kHz SOF frames. See
`keyboards/ydkb/unicore_f1/USB_SOF_TRIM_PLAN.md`.

### Verified

This was built and verified from a fresh `git clone --recurse-submodules` of
the published repository: the resulting binary is byte-identical to the local
one (`30564` bytes, md5 `93dfabcb4d5e012f9f41cbaba2540a3b`), and so is the
UF2 (`719bd42bcb074dcce855703d85523506`).

## Checks

`qmk lint` is the canonical gate. The two scripts in the keyboard directory are
**ad-hoc** cross-checks written for this port; they are not upstream CI and they
are not a test suite:

```bash
cd qmk_firmware
qmk lint -kb ydkb/unicore_f1
make ydkb/unicore_f1:dusk67_via:via_json      # VIA definition vs this firmware
make ydkb/unicore_f1:dusk67_via:keymap_check  # keymap vs the layout export
```

Both checks default to the reference files in `docs/`, so they work with no
arguments. Pass a path to check a different file. If no reference file can be
found at all they print `SKIP` and exit 0, since a missing reference is not a
firmware defect; a path you passed explicitly that does not exist is an error,
so a typo cannot read as silently green.

### Reading the keymap back off the board

`via_readback.py` is a third, **hardware-attached** check: it reads every
`(layer, row, col)` keymap cell over the VIA raw-HID interface and diffs it
against `keymaps/dusk67_via/keymap.c`. It needs the udev rule from
`keyboards/ydkb/unicore_f1/Architecture.md` §5 and the keyboard plugged in.

```bash
python3 keyboards/ydkb/unicore_f1/via_readback.py
```

It only sends read commands, and refuses by name any command that would write
EEPROM — in particular `id_dynamic_keymap_reset` (0x06), which corrupted 8 cells
under v0.1. It does not reproduce under v0.2, and a driver trace shows why: the
reset performs **zero writes**, because every byte already matches flash, so
there is no write log and no compaction. What made those bytes differ under
v0.1 is still unknown — unexplained, not fixed (Architecture.md §7.2).

## Layout options

| Option | Values |
|---|---|
| Split Backspace | toggle |
| ISO Enter | toggle |
| Split LShift | toggle |
| Space Row | 6.25u / 7u |
| CapsLock Color | Red / Orange / Yellow / Green / Cyan / Blue / Violet / White |

The first four are resolved by the **configurator app** (keys carry a
`group,option` legend and the app draws a different key per value) and need no
firmware support. Only CapsLock Color is decoded in firmware, in `led.c`.

## Maintainer notes

`keyboards/ydkb/unicore_f1/Architecture.md` is the working log: build and verify
commands, the live VIA protocol wire format, environment requirements, and a
list of **conclusions that were wrong** and must not be re-derived.