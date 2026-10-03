/* Copyright 2022 YANG <drk@live.com>
 * Copyright 2026
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

/* Serial shift-register matrix size (custom matrix has no pins to declare). */
#define MATRIX_ROWS 14
#define MATRIX_COLS 8

/* VIA layout options carry 9 bits (split backspace / ISO enter / split LShift
 * / space row variant / 3-bit indicator colour) -> needs 2 bytes instead of
 * the default 1. */
#define VIA_EEPROM_LAYOUT_OPTIONS_SIZE 2

/* Bump when the meaning of the VIA layout options changes, so a VIA
 * definition and this firmware can be matched up. */
#define VIA_FIRMWARE_VERSION 0x00000001

/* Debounce is QMK's default 5ms (the vendor firmware used 3ms down / 5ms up);
 * set DEBOUNCE in config.h to override. */

/* --- EEPROM (legacy STM32 flash emulation) ---
 * The driver defaults to FEE_PAGE_SIZE 0x400 x FEE_PAGE_COUNT 2 (2 KB total,
 * 1 KB emulated density) on STM32F103xB, which is far too small for a
 * 14x8 matrix: 6 layers x 112 keys x 2 B = 1344 B of keymap alone, plus the
 * VIA config block and the 2-byte layout options, and QMK additionally
 * requires >= 100 B left over for macros (nvm_dynamic_keymap.c STATIC_ASSERT).
 *
 * The linker script already reserves the top 8 KB of flash for this
 * (flash0 len = 128k - 0x4000 - 8k, i.e. 0x0801E000..0x08020000) and the
 * emulation lives at the end of flash, so grow the page count to match the
 * reservation: 8 pages x 1 KB = 8 KB region.
 *
 * FEE_DENSITY_BYTES is the emulated EEPROM size and is cached in RAM. 2 KB
 * covers the 1344 B keymap + VIA config with headroom while keeping the RAM
 * cost modest on this 20 KB part; the remaining 6 KB becomes write log, so
 * the compacted area rarely needs rewriting. Max allowed is 16384.
 */
#define FEE_PAGE_COUNT   8
#define FEE_DENSITY_BYTES 2048

/* Block startup until USB is enumerated (vendor firmware behaviour). Without
 * it the SOF trim module initialises before the bus is up. */
#define WAIT_FOR_USB

/* Magic key combination for the debug command feature:
 * LShift+RShift or LShift+LCtrl+RShift. */
#define IS_COMMAND() ( \
    (get_mods() == (MOD_BIT(KC_LEFT_SHIFT) | MOD_BIT(KC_RIGHT_SHIFT))) || \
    (get_mods() == (MOD_BIT(KC_LEFT_SHIFT) | MOD_BIT(KC_LEFT_CTRL) | MOD_BIT(KC_RIGHT_SHIFT))) \
)

/* --- USB tear-off fix (see USB_SOF_TRIM_PLAN.md) ---
 * Enabled by default on this HSI-only board:
 *  - SOF-based HSI clock trim (locks the 48MHz USB clock to the host's frames)
 *  - USB watchdog (auto-reconnect if the host silently drops the device)
 * Optional knobs:
 * #define USB_SOF_TRIM_DISABLE      // turn the trim loop off
 * #define USB_WATCHDOG_DISABLE      // turn the watchdog off
 * #define SOF_TRIM_DEBUG            // console logging (build w/ CONSOLE_ENABLE)
 * #define SOF_TRIM_WINDOW_FRAMES 128 // measurement window in frames (ms)
 */
