# Dusk67

UniCore-F1 (STM32F103CBT6) 67-key keyboard, wired USB, **VIA protocol 13**.

![Dusk67](https://raw.githubusercontent.com/ydkb/dusk67/master/dusk67.jpg)

This is an upstream-QMK-native port. It talks plain VIA — there is no Vial
dependency — so it works with the VIA Configurator app.

## Layout

14x8 serial shift-register matrix (74HC595 + 5020 on PB12/PB13); matrix rows
0–9 are populated, rows 10–13 are unused padding that the configurator apps
still expect to see. `LAYOUT` in `keyboard.json` has 112 entries.

VIA layout options (2 bytes, 9 bits used):

| Option | Values |
|---|---|
| Split Backspace | toggle |
| ISO Enter | toggle |
| Split LShift | toggle |
| Space Row | 6.25u / 7u |
| CapsLock Color | Red / Orange / Yellow / Green / Cyan / Blue / Violet / White |

## Flashing

The board has a vendor UF2 bootloader occupying the first 16 KB of flash, so
the application is linked at `0x08004000`.

1. Hold **Esc** while plugging in the keyboard (bootmagic → bootloader).
2. Drag the `.uf2` onto the mounted drive.

```
make ydkb/unicore_f1:dusk67_via
python3 util/uf2conv.py .build/ydkb_unicore_f1_dusk67_via.bin \
    -b 0x8004000 -c -f 0x9d5bcf10 -o dusk67_via.uf2
```

## Hardware notes

- **No HSE crystal.** The 48 MHz USB clock comes from the internal HSI
  oscillator, whose ±1–2 % tolerance is far outside the USB full-speed budget
  of ±0.25 %. That is what makes the keyboard randomly "tear off" from the OS.
- `usb_sof_trim.c` fixes this in software: it timestamps every 1 kHz USB Start
  Of Frame with TIM4 and nudges `RCC_CR.HSITRIM` to lock the clock to the
  host's frames, plus a watchdog that force-reconnects (PA12 pulse, then MCU
  reset) if the host drops the bus silently. See `USB_SOF_TRIM_PLAN.md`.
- **EEPROM** is flash emulation at the top of flash (`0x0801E000`, 8 KB
  reserved by `ld/STM32F103CBT6_uf2_bootloader.ld`). `FEE_PAGE_COUNT` /
  `FEE_DENSITY_BYTES` in `config.h` are raised from the F103 defaults because
  6 layers x 112 keys does not fit the stock 1 KB emulated density.
- Bootloader entry is the vendor protocol: magic `0x9d5bfc2b` written to
  `0x20004000`, then a reset (`bootloader_jump()` in `led.c`). Esc-to-bootloader
  does **not** wipe the EEPROM; bind `EE_CLR` for a full wipe.

## Keymaps

| Keymap | Description |
|---|---|
| `default` | Same as `dusk67_via`. |
| `dusk67_via` | Base + Fn layer transcribed from the user's Vial layout export; `BL_TOGG` left unbound (this board has no backlight). |
