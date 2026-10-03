# Dusk67 USB tear-off fix — implementation plan & log

Status: **implemented, awaiting on-hardware validation** (branch `dusk67-fix`, base: `ava` @ `a1e3865b6f`, 2026-09-27)

## Scope warning — read before using this for debugging

This module exists to fix **one** symptom: the OS randomly dropping the device
because the HSI-derived USB clock drifts out of the ±0.25 % full-speed
tolerance ("tear-off"). It is **not** a general USB diagnostic.

The separate, still-unresolved problem in the Dusk67 work is **"Vial/VIA does
not communicate with the keyboard"** (`~/work/vial-qmk-tip`). That is a raw-HID
transport fault, not a clock fault, and no evidence has ever linked it to HSI
drift. Do not read SOF-trim console output while chasing it: if the transport is
dead the log is silent exactly when you need it, and the trim loop converges
happily on a perfectly healthy clock that the app still cannot talk to.

### Self-correction, 2026-10-03 (measured, not assumed)

The `SOF_TRIM_DEBUG` line added on 2026-09-28 to chase the Vial problem was
wrong in two ways and never worked:

1. **It could not link.** It `extern`-declared six counters
   (`g_rawhid_rx_count`, `g_rawhid_tx_count`, `g_out_arm_count`,
   `g_out_isr_count`, `g_ctrl_req_count`, `g_ctrl_req`) that exist only in the
   hand-patched `usb_main.c`/`usb_driver.c` of the `vial-qmk-tip` fork. This
   upstream tree has no such symbols, so building with `SOF_TRIM_DEBUG` failed:
   `undefined reference to g_rawhid_rx_count` (and 5 more). The production build
   only passed because **nothing in the tree ever defined `SOF_TRIM_DEBUG`** —
   the log was dead code, never compiled, so nobody noticed it was broken.
2. **It was reading the wrong register — twice over.**
   It dumped `STM32_USB->EPR[3]` and interpreted bits 13:12. Two independent
   errors lived in that one line:

   a. **The shift was off by one.** `EPR_STAT_RX_MASK` is `0x3000`, i.e. the
      field occupies bits **13:12**, so it must be shifted `>> 12`. The code
      used `>> 13`, which discards bit 12 and therefore collapses all four
      values: `00`→0, `01`→0, `10`→1, `11`→1. The logged "state" could only
      ever read 0 or 1, and both DISABLED and NAK were indistinguishable.
   b. **The endpoint identity was luck, not knowledge.** `EPR[3]` happens to be
      the raw-HID **OUT** endpoint on this build, but nothing in the code
      established that. Reading the *compiled* descriptor
      (`.build/obj_ydkb_unicore_f1_dusk67_via/usb_descriptor.o`) and matching each
      interface by its HID
      report length (68 = keyboard, 34 = raw-HID, 182 = shared) gives the real
      assignment:

      | iface | report | endpoints |
      |-------|--------|-----------|
      | 0 | 68  | `0x81` IN ep1 — KEYBOARD |
      | 1 | 34  | `0x82` IN ep2 — RAW IN, `0x03` OUT ep3 — RAW OUT |
      | 2 | 182 | `0x84` IN ep4 — SHARED / CONSOLE |

      **`usb_descriptor.h`'s `RAW_IN_EPNUM` / `RAW_OUT_EPNUM` enum disagrees with
      the descriptor it generates** — it yields IN=3 / OUT=4, because it counts
      the shared IN endpoint in its `__COUNTER__` sequence while the raw
      interface is emitted before the shared one. Confirmed by compiling a
      probe with the build's own cflags. Using the enum here reads the wrong
      EPR; the literals are deliberate and commented as such.

Both are now fixed: the log shifts `>> 12`, and reads `EPR[3]`/`EPR[2]` with a
comment recording why the enum is not used. It takes every value from USB
hardware registers, so `usb_sof_trim.c` borrows no symbols from the USB driver
and links on any QMK base. Verified:
`make ydkb/unicore_f1:dusk67_via:CONSOLE_ENABLE=yes EXTRAFLAGS=-DSOF_TRIM_DEBUG`
links `[OK]`.

Note the surviving comment on `EPR[3]` in the original source ("RAW OUT
endpoint, STAT_RX bits 13:12") was right about the endpoint and wrong about
the shift. Treat the current comment block as the record.

The same broken log still exists in `~/work/vial-qmk-tip`
(`keyboards/ydkb/unicore_f1/usb_sof_trim.c`); it is *not* fixed there.


## Implementation record (2026-09-27)

- New files: `usb_sof_trim.c` / `usb_sof_trim.h` (compiled clean, no warnings),
  bench keymap `keymaps/dusk67_vial_dbg/` (console + `SOF_TRIM_DEBUG`).
- Integration: `usb_sof_trim_init()` at the end of `matrix_init()` (runs after
  `WAIT_FOR_USB` completes — verified via `quantum/main.c` call order);
  `usb_sof_trim_task()` called from `matrix_scan()`; `SRC += usb_sof_trim.c` in
  `rules.mk`; knob documentation in the keymap `config.h`.
- **As-built deviation from the design above:** no vector-table hijacking was
  needed. ChibiOS's F1 USB LLD already invokes `USBConfig.sof_cb` on every SOF
  (`ISTR_SOF` = 0x0200, `CNTR_SOFM` = 0x0200 — verified against
  `lib/chibios/os/hal/ports/STM32/LLD/USBv1/`), and QMK's own `usbcfg` registers
  one (which also keeps `CNTR_SOFM` enabled across bus resets via
  `usb_lld_reset`). The module therefore swaps `USBD1.config` to a RAM copy with
  its own `sof_cb` that timestamps TIM4 and chains QMK's original callback
  (`kbd_sof_cb` + `qmkusbSOFHookI`) — race-free, no core or submodule edits.
- Register facts verified in-tree: `RCC_CR_HSITRIM` bits [7:3] (default 16),
  TIM4 free (TIM2 = ChibiOS tick; ws2812 on this build is a busy-wait bit-bang,
  no timer), timer clock 48 MHz (APB1 ÷2 → ×2), PA12 board default = floating
  input (`MAPLEMINI_STM32_F103/board.h` `VAL_GPIOACRH`).
- Build artifacts (toolchain: arm-gnu 13.2.Rel1, qmk CLI 1.2.0 on python3.11):
  - `.build/dusk67_vial_softrim.uf2` — production (47 KB bin)
  - `.build/dusk67_vial_softrim_dbg.uf2` — bench/console logging (52 KB bin)


## Hardware / symptom / root cause

- Keyboard: Dusk67 — keymap `keyboards/ydkb/unicore_f1/keymaps/dusk67_vial/` on the
  UniCore-F1 controller (STM32F103CBT6, vendor UF2 bootloader app offset `0x8004000`,
  `BOOTLOADER=custom`, ld `STM32F103CBT6_uf2_bootloader`, `CORTEX_VTOR_INIT=0x4000`).
- **No HSE crystal on the board** (confirmed by visual inspection, 2026-09-27).
- Symptom: random USB disconnects ("tears off from the OS") while typing, on Windows.
- Root cause: the 48 MHz USB clock is derived from the internal HSI RC oscillator
  (`mcuconf.h`: `HSI/2 × 12 = 48 MHz`, `USBPRE DIV1`). HSI tolerance is ±1–2 % over
  temperature/voltage vs the ±0.25 % the full-speed USB spec requires. When drift
  crosses the edge the host drops the device. Windows is the strictest host.

## Decision log

1. **Stay on the vial-qmk-v5 vendor fork** (`yangdigi/vial-qmk-v5`, branch `ava`) for
   this fix. Porting to vial-kb tip / upstream QMK does not fix the clock problem
   (silicon, not firmware stack) — that is a separate, deferred project (below).
2. Fix = **software USB clock recovery**: trim the F103's HSI (`RCC_CR.HSITRIM`)
   against the host's 1 kHz USB SOF frames, plus a disconnect watchdog for fast
   self-recovery.
3. No SWD/JTAG debugger — flashing/recovery stays on the UF2 + Esc-bootmagic path
   (Esc held on plug-in → `bootmagic_lite()` → `enter_bootloader()`).

## Project 1 — SOF-based HSI trim (this branch)

### Design (all changes inside `keyboards/ydkb/unicore_f1/`)

- **New file `usb_sof_trim.c`** (+ `SRC` entry in `rules.mk`):
  - **TIM4** free-running @ 48 MHz, no prescaler: 1 SOF frame (1 ms) ≈ 48000 ticks,
    fits the 16-bit counter; per-frame deltas handle wrap via 16-bit subtraction.
    (TIM2 = ChibiOS systick per `mcuconf.h`; all GPT/PWM/serial are disabled there.)
  - **SOF capture:** enable `CNTR.SOFM` and take over the `USB_LP_CAN1_RX0` (IRQ 20)
    vector by copying the vector table to RAM, pointing `SCB->VTOR` at the copy, and
    installing a chained handler: on `ISTR.SOF` → latch `TIM4->CNT` → clear SOF →
    call the original ChibiOS handler. SOF is cleared *before* chaining so ChibiOS's
    F1 ISR (which does not expect SOF) never sees it.
  - **Control loop** (`usb_sof_trim_task()`, called from `matrix_scan()`):
    accumulate 32 per-frame tick deltas; error = `sum − 32×48000`;
    ppm = error/1536; nudge `RCC_CR.HSITRIM` ±1 when |error| exceeds half a trim
    step; clamp; freeze when SOF stops (suspend) or USB is not active; self-measures
    the real trim-step size in ppm and logs it.
  - **Self-healing:** task re-arms `CNTR.SOFM` periodically in case ChibiOS rewrites
    CNTR on suspend/resume events.
- **Disconnect watchdog** (same file, same task):
  - Signal: SOF stream silent > ~250 ms while USB driver is not in the suspended
    state and we had a live SOF stream recently (i.e. bus died under us, not a
    legitimate host suspend — suspend stops SOF too but sets `USB_SUSPENDED`).
  - Tier 1: pulse PA12 (USB D+) output-low ~20 ms — the hard-disconnect trick the
    firmware already uses at boot in `early_hardware_init_pre()` (matrix.c) — then
    release the pin back to USB and give the host 500 ms to re-detect.
  - Tier 2: still dead → system reset → clean re-enumeration (~1 s).
    `WAIT_FOR_USB` guarantees the firmware blocks (no reset loops) if the cable is
    actually unplugged.
- **Integration:** `usb_sof_trim_task()` called from `matrix_scan()` in `matrix.c`;
  `usb_sof_trim_init()` from `matrix_init()` (runs after `WAIT_FOR_USB`, so USB is
  active and SOF is flowing). Config knobs in the keymap `config.h`.
- No changes to `mcuconf.h` clock init (HSI/48 MHz stays) → no "hangs before USB"
  brick class; Esc-on-plug always recovers.

### Expected outcome & limits

- HSI trim granularity (~0.4 %/LSB per RM0008; real value self-measured) means the
  clock can be parked within ~±0.2 % of nominal — right at USB's ±0.25 % edge.
  Expect tear-offs to become **rare**, not impossible; the watchdog converts any
  survivor into a sub-second auto-reconnect instead of an OS-level hang.

### Validation protocol

1. Bench build with console logging (`hid_listen`/QMK Toolbox): watch `ppm/trim/step`
   converge within ~2 s of plug-in; record measured trim step and steady-state ppm.
2. Windows soak test 24–48 h of normal typing; count disconnect events vs baseline.
3. Warm-board test (HSI worst case).
4. Suspend/resume cycles: watchdog must stay quiet, trim must freeze/resume cleanly.
5. Ship build: console off. **Rollback:** keep the currently-flashed vendor UF2;
   hold Esc while plugging in to re-enter the bootloader and reflash it.

## Project 2 — upgrade to vial-kb tip (deferred; feasibility verified)

- Fresh clone of `vial-kb/vial-qmk` (branch `vial`), **overlay, not rebase**
  (vendor delta = 95 commits / 550 files / +32k lines; 42 core files, of which
  dusk67 needs ~3; a literal rebase would replay days of conflicts and still force
  a QMK-2026 format conversion).
- Carry: `keyboards/ydkb/unicore_f1/` + dusk67 keymap + `debounce_pk.h` +
  `platforms/chibios/boards/MAPLEMINI_STM32_F103/` (**removed upstream** — copy the
  3 board files) + custom UF2 ld (`BOOTLOADER=custom` still exists at tip).
- Convert: `keyboard.json`, keycode-v5 aliases, `ws2812_write_leds`, drop
  `UNICODE_ENABLE`, move user-config to the upstream VIA custom-config API;
  `via_set_layout_options_after` → `via_set_layout_options_kb`; drop
  `FLASH_KEYMAP_COUNT` (upstream seeds all layers).
- Re-apply the 2B `usb_sof_trim.c` patch (F1 USB LLD is stable across bases).
- Still verified true: F103 toolchain support at tip (`STM32_F103_STM32DUINO`
  board intact), `custom.c` bootloader stub present.
- Dropped by design: SOCD (`action.c`), `tmk_core/protocol/ble51/` BLE stack,
  ps2/AVR-suspend patches — irrelevant to a wired Dusk67.

## Flashing (both projects)

```
make ydkb/unicore_f1:dusk67_vial
util/uf2conv.py ydkb_unicore_f1_dusk67_vial.bin -b 0x8004000 -c -f 0x9d5bcf10 -o dusk67.uf2
# hold Esc while plugging in → drag dusk67.uf2 onto the UF2 drive
```
