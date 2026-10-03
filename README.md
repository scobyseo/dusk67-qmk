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

The keyboard must live inside the submodule's working tree for QMK to find it,
so after cloning:

```bash
git clone --recurse-submodules <this-repo>
# or, in an existing clone:
git submodule update --init --recursive
```

If you add the keyboard by hand instead, put it at
`qmk_firmware/keyboards/ydkb/unicore_f1/`.

## Build

Requires an ARM toolchain (tested with `arm-none-eabi-gcc` 13.2.Rel1):

```bash
export PATH=~/work/toolchain-arm-gnu-13.2/bin:$PATH
cd qmk_firmware
make ydkb/unicore_f1:dusk67_via
```

The binary lands at `.build/ydkb_unicore_f1_dusk67_via.bin`. Convert and flash:

```bash
python3 util/uf2conv.py .build/ydkb_unicore_f1_dusk67_via.bin \
    -b 0x8004000 -c -f 0x9d5bcf10 -o dusk67_via.uf2
```

Hold **Esc** while plugging the keyboard in (bootmagic -> bootloader), then
drag the `.uf2` onto the mounted drive.

The board has **no HSE crystal** — the 48 MHz USB clock derives from the
internal HSI oscillator, which drifts outside the USB full-speed tolerance and
makes the keyboard randomly "tear off" from the OS. `usb_sof_trim.c` fixes this
in software by trimming HSI against the host's 1 kHz SOF frames. See
`keyboards/ydkb/unicore_f1/USB_SOF_TRIM_PLAN.md`.

## Checks

`qmk lint` is the canonical gate. The two scripts in the keyboard directory are
**ad-hoc** cross-checks written for this port; they are not upstream CI and they
are not a test suite:

```bash
qmk lint -kb ydkb/unicore_f1
python3 keyboards/ydkb/unicore_f1/check_via_json.py [definition.json]
python3 keyboards/ydkb/unicore_f1/check_keymap.py    [export.json]
```

Both scripts need a reference file that is not in this repo. Given an explicit
path they check it; with no path they print `SKIP` and exit 0. An explicit path
that does not exist is an error, not a skip.

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