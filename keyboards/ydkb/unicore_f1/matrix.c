/*
Copyright 2022 YANG <drk@live.com>
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

/* UniCore-F1 "lite" custom matrix: the switches are read serially through a
 * 74HC595 (output) / 5020 (input) shift-register pair on PB12/PB13, one cell
 * at a time, 14 rows x 8 cols. Debouncing is handled by QMK core. */

#include <ch.h>
#include <hal.h>
#include "quantum.h"
#include "timer.h"
#include "wait.h"
#include "matrix.h"
#include "switch_board.h"
#include "usb_sof_trim.h"

bool is_ver5020       = 0;
bool is_sc_leds_mcu   = 0;
bool has_extra_pull_up = 0;

static bool matrix_custom_ready = false;

static void select_key(uint8_t mode);
static uint8_t get_key(void);

/* One-time pin/board setup. Called lazily from matrix_scan_custom() as well,
 * because QMK's bootmagic scans the matrix before matrix_init() runs. */
static void matrix_custom_setup(void) {
    if (matrix_custom_ready) {
        return;
    }
    matrix_custom_ready = true;

    /* check ver595 or ver5020. PB9 */
    palSetPadMode(GPIOB, 9, PAL_MODE_INPUT_PULLUP);
    palSetPad(GPIOB, 9);
    /* check if single color led indicators. PB8 */
    palSetPadMode(GPIOB, 8, PAL_MODE_INPUT_PULLUP);
    palSetPad(GPIOB, 8);
    wait_ms(10);
    if (palReadPad(GPIOB, 9) == 0) is_ver5020 = 1;
    if (palReadPad(GPIOB, 8) == 0) is_sc_leds_mcu = 1;

    /* caps_led, PB14 */
    palSetPadMode(GPIOB, 14, PAL_MODE_OUTPUT_PUSHPULL);
    palClearPad(GPIOB, 14);
    /* scroll_led, PA8 */
    palSetPadMode(GPIOA, 8, PAL_MODE_OUTPUT_PUSHPULL);
    palClearPad(GPIOA, 8);

    /* 595 | 5020 pins */
    palSetGroupMode(GPIOB, (1 << 13 | 1 << 12), 0, PAL_MODE_OUTPUT_PUSHPULL);
    /* disable all keys */
    select_key_ready();
    KEY_SDI_OFF();
    for (uint8_t i = 0; i < MATRIX_ROWS * MATRIX_COLS; i++) {
        CLOCK_PULSE();
    }

    /* check extra pull up */
    palSetPad(GPIOB, 13);
    palSetPadMode(GPIOB, 13, PAL_MODE_INPUT_PULLDOWN);
    wait_ms(5);
    if (palReadPad(GPIOB, 13)) {
        has_extra_pull_up = 1;
    }
}

void matrix_init_custom(void) {
    matrix_custom_setup();

    /* SOF trim + USB watchdog (runs after WAIT_FOR_USB, so USB is active). */
    usb_sof_trim_init();
}

bool matrix_scan_custom(matrix_row_t current_matrix[]) {
    matrix_custom_setup();

    /* SOF trim loop + disconnect watchdog (no-op until initialized). */
    usb_sof_trim_task();

    matrix_row_t temp[MATRIX_ROWS] = {0};
    uint8_t       raw_down         = 0;

    select_key(0);
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        for (uint8_t col = 0; col < MATRIX_COLS; col++) {
            uint8_t key = get_key();
            select_key(1);
            if (key) {
                temp[row] |= (matrix_row_t)1 << col;
                raw_down++;
            }
        }
    }

    /* Guard against a KEY line shorted to GND: every cell reads down, which
     * is impossible with a real hand. Ignore the whole scan instead of
     * reporting every key held (vendor firmware "PREVENT_KEYIO_GND"). */
    if (raw_down >= MATRIX_ROWS * MATRIX_COLS) {
        return false;
    }

    bool changed = false;
    for (uint8_t row = 0; row < MATRIX_ROWS; row++) {
        if (current_matrix[row] != temp[row]) {
            current_matrix[row] = temp[row];
            changed             = true;
        }
    }
    return changed;
}

/* B13(595 data-in) / B14(5020 data-out) */
static uint8_t get_key(void) {
    return palReadPad(GPIOB, 13) ? 0 : 0x80;
}

static void select_key(uint8_t mode) {
    select_key_ready();
    if (mode == 0) {
        KEY_SDI_OFF();
        for (uint8_t i = 0; i < MATRIX_ROWS * MATRIX_COLS; i++) {
            CLOCK_PULSE();
        }
        KEY_SDI_ON();
        CLOCK_PULSE();
    } else {
        KEY_SDI_OFF();
        CLOCK_PULSE();
    }
    get_key_ready();
}
