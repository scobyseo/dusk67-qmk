/*
Copyright 2023 YANG <drk@live.com>
Copyright 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* UniCore-F1 indicator LEDs.
 *
 * Depending on board revision (PB8 strap, detected in matrix.c), the
 * indicators are either plain GPIO LEDs (PB14 caps / PA8 scroll) or
 * WS2812-protocol LEDs driven through B15. The indicator colour is stored in
 * the VIA layout options (3 bits per indicator), so it can be changed from
 * a configurator app without reflashing.
 */

#include <ch.h>
#include <hal.h>
#include "quantum.h"
#include "via.h"
#include "raw_hid.h"
#include "wait.h"
#include "ws2812.h"
#include "switch_board.h"
#include "command.h"

extern bool is_sc_leds_mcu;

#ifndef PHY_INDICATOR_NUM
#    define PHY_INDICATOR_NUM 0
#endif
#ifndef LOGIC_INDICATOR_NUM
#    define LOGIC_INDICATOR_NUM PHY_INDICATOR_NUM
#endif
#ifndef INDICATOR_VAL
#    define INDICATOR_VAL 255
#endif

static uint8_t indicator_state        = 0;
static uint8_t indicator_color_config[3];
static uint8_t indicator_color[3][3];

/* Hue preset per layout-option colour index (matches the vendor firmware):
 * 254 -> white, 255 -> off, otherwise the HSV hue wheel. */
static const uint8_t indicator_hue_preset[8] = {254, 0, 42, 85, 127, 170, 212, 255};

static void hsv_s255_to_rgb(uint8_t hue, uint8_t val, uint8_t *rgb) {
    uint8_t region = hue / 43;
    uint8_t rest   = (hue - (region * 43)) * 6;
    uint8_t q      = (val * (255 - rest)) / 256;
    uint8_t t      = (val * rest) / 256;
    switch (region) {
        case 0: rgb[0] = val; rgb[1] = t;    rgb[2] = 0;    break;
        case 1: rgb[0] = q;    rgb[1] = val; rgb[2] = 0;    break;
        case 2: rgb[0] = 0;    rgb[1] = val; rgb[2] = t;    break;
        case 3: rgb[0] = 0;    rgb[1] = q;    rgb[2] = val; break;
        case 4: rgb[0] = t;    rgb[1] = 0;    rgb[2] = val; break;
        default: rgb[0] = val; rgb[1] = 0;   rgb[2] = q;    break;
    }
}

static void single_color_indicator_set(uint8_t index, bool on) {
    if (index == 0) {
        if (on) palSetPad(GPIOB, 14);
        else palClearPad(GPIOB, 14);
    } else if (index == 1) {
        if (on) palSetPad(GPIOA, 8);
        else palClearPad(GPIOA, 8);
    }
}

static void indicator_apply(void) {
    for (uint8_t i = 0; i < LOGIC_INDICATOR_NUM; i++) {
        bool on = (indicator_state & (1 << i)) && indicator_color_config[i] != 7;
        if (is_sc_leds_mcu) {
            single_color_indicator_set(i, on);
        }
    }
    if (!is_sc_leds_mcu && PHY_INDICATOR_NUM) {
        for (uint8_t i = 0; i < PHY_INDICATOR_NUM && i < 3; i++) {
            if ((indicator_state & (1 << i)) && indicator_color_config[i] != 7) {
                ws2812_set_color(i, indicator_color[i][0], indicator_color[i][1], indicator_color[i][2]);
            } else {
                ws2812_set_color(i, 0, 0, 0);
            }
        }
        ws2812_flush();
    }
}

/* (Re)load the indicator colours from the VIA layout options. Called at boot
 * (via_init -> via_set_layout_options_kb) and whenever a configurator app
 * changes the layout options. */
void via_set_layout_options_kb(uint32_t value) {
    uint16_t layout_value = (uint16_t)value;
    for (uint8_t i = 0; i < 3; i++) {
        indicator_color_config[i] = layout_value & 0b111;
        uint8_t hue               = indicator_hue_preset[indicator_color_config[i]];
        if (hue == 254) {
            indicator_color[i][0] = indicator_color[i][1] = indicator_color[i][2] = INDICATOR_VAL / 2; /* white */
        } else if (hue == 255) {
            indicator_color[i][0] = indicator_color[i][1] = indicator_color[i][2] = 0; /* off */
        } else {
            hsv_s255_to_rgb(hue, INDICATOR_VAL, indicator_color[i]);
        }
        layout_value >>= 3;
    }
    indicator_apply();
}

bool led_update_user(led_t led_state) {
    indicator_state = 0;
    if (led_state.caps_lock) indicator_state |= (1 << 0);
    if (led_state.num_lock) indicator_state |= (1 << 1);
    if (led_state.scroll_lock) indicator_state |= (1 << 2);
    indicator_apply();
    return true;
}

/* Vendor UF2 bootloader entry: magic at 0x20004000, then reset. Used by the
 * QK_BOOT keycode, bootmagic (Esc on plug-in) and the command below. */
void bootloader_jump(void) {
    clear_keyboard();
    volatile uint32_t *uf2bl_backup_reg = (uint32_t *)0x20004000;
    *uf2bl_backup_reg                   = 0x9d5bfc2bUL;
    NVIC_SystemReset();
}

/* Entering the bootloader via Esc/bootmagic must NOT wipe the EEPROM
 * (preserves the user's saved keymap); eeprom reset stays explicit. */
void bootmagic_reset_eeprom(void) {}

/* VIA has no case for id_bootloader_jump (0x0B) in quantum/via.c's
 * raw_hid_receive switch -- the command exists in the enum (via.h) but is
 * never dispatched, so a host that sends it gets id_unhandled (0xFF) back and
 * the board stays in the application. This is the sanctioned keyboard-level
 * hook (via.c calls via_command_kb *before* its own switch and honours a true
 * return), so implementing it here needs no change to upstream QMK.
 *
 * Purpose: let a host script put the board into its UF2 bootloader over
 * raw-HID, so a firmware update does not need Esc held by hand at plug-in.
 *
 * On the reply: the reset below happens within microseconds while a USB poll
 * is only every 1 ms, so a reply queued by raw_hid_send() is NOT reliably
 * delivered. It is sent anyway (harmless, and it lands on hosts that happen to
 * poll first) but callers must treat "the device vanished from the bus" as the
 * success signal, never the reply. flash_dusk67.sh does exactly that.
 *
 * The delay before resetting exists only to give the queued report a chance to
 * drain; it is not load-bearing for correctness.
 */
bool via_command_kb(uint8_t *data, uint8_t length) {
    if (data[0] != id_bootloader_jump) {
        return false;
    }
    raw_hid_send(data, length);
    wait_ms(20);
    bootloader_jump();
    return true; /* not reached: bootloader_jump() resets the MCU */
}

/* LShift+RShift+LCtrl+B -> bootloader; without Ctrl -> soft reset. */
bool command_extra(uint8_t code) {
    uint8_t pressed_mods = get_mods();
    clear_keyboard();
    switch (code) {
        case KC_B:
            wait_ms(500);
            if (pressed_mods & MOD_BIT(KC_LEFT_CTRL)) {
                bootloader_jump();
            }
            NVIC_SystemReset();
            break;
        default:
            return false; /* yield to default command */
    }
    return true;
}
