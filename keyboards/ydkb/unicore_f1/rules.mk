# Custom matrix: 74HC595 / 5020 shift-register scan (see matrix.c)
CUSTOM_MATRIX = lite

# Single WS2812-protocol indicator LED on B15 (no underglow array on Dusk67).
# Driver is bitbang (declared in keyboard.json); it is bit-banged because this
# HSI-only build reserves TIM4 for the SOF trim, leaving no timer for PWM.
WS2812_DRIVER_REQUIRED = yes

# Vendor UF2 bootloader occupies 0x08000000..0x08004000; the app links at
# 0x08004000 with the top 8k of flash reserved for the legacy emulated-flash
# EEPROM (see ld/STM32F103CBT6_uf2_bootloader.ld). Bootloader entry is the
# vendor protocol: magic 0x9d5bfc2b at 0x20004000 then MCU reset (see led.c).
MCU_LDSCRIPT = STM32F103CBT6_uf2_bootloader

# project specific files
SRC += matrix.c led.c usb_sof_trim.c

# Ad-hoc cross-checks, wired to `make` targets so they are discoverable:
#
#   make ydkb/unicore_f1:dusk67_via via_json     VIA definition vs this firmware
#   make ydkb/unicore_f1:dusk67_via keymap_check keymap vs a layout export
#
# Neither is part of upstream CI. Each needs a reference file that does not ship
# with the repo; with no variable set both print SKIP and succeed, so a fresh
# clone gets a usable `make` rather than a spurious failure. Point VIA_JSON /
# KEYMAP_EXPORT at a file to actually run the check.
VIA_JSON      ?=
KEYMAP_EXPORT ?=

# QMK already knows the keyboard's path relative to its root; use that instead
# of deriving one from MAKEFILE_LIST, which resolves to the wrong file by the
# time a keyboard rules.mk is read. Both recipes run from the QMK root.
.PHONY: via_json keymap_check
via_json:
	@python3 $(KEYBOARD_PATH_1)/check_via_json.py $(VIA_JSON)

keymap_check:
	@python3 $(KEYBOARD_PATH_1)/check_keymap.py $(KEYMAP_EXPORT)