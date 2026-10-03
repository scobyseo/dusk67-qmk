/* Copyright 2026
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

#include QMK_KEYBOARD_H

/* Base keymap transcribed from dusk67_layout.vil (layer 0 = base,
 * layer 1 = Fn layer reached by MO(1) on the top-row right key).
 * Rows 9-13 are the unused matrix tail (all KC_NO) and are kept so the
 * layout matches the 14x8 matrix the VIA/Vial apps expect. */

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
    [0] = LAYOUT(
        QK_GESC, KC_1, KC_2, KC_3, KC_4, KC_5, KC_6, KC_7,
        KC_TAB, KC_Q, KC_W, KC_E, KC_R, KC_T, KC_Y, MO(1),
        KC_LEFT_SHIFT, KC_A, KC_S, KC_D, KC_F, KC_G, KC_H, KC_CAPS_LOCK,
        KC_LEFT_CTRL, KC_Z, KC_X, KC_C, KC_V, KC_B, KC_LEFT_ALT, KC_LEFT_GUI,
        KC_SPACE, KC_N, KC_M, KC_COMMA, KC_NO, KC_NO, KC_LNG1, KC_APPLICATION,
        KC_RIGHT, KC_DOWN, KC_LEFT, KC_DOT, KC_SLASH, KC_RIGHT_SHIFT, KC_UP, KC_PGDN,
        KC_PGUP, KC_J, KC_K, KC_L, KC_SCLN, KC_QUOT, KC_ENTER, KC_NO,
        KC_BSPC, KC_U, KC_I, KC_O, KC_P, KC_LBRC, KC_RBRC, KC_DEL,
        KC_GRAVE, KC_NO, KC_BSLS, KC_8, KC_9, KC_0, KC_MINS, KC_EQL,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO
    ),
    [1] = LAYOUT(
        KC_GRAVE, KC_F1, KC_F2, KC_F3, KC_F4, KC_F5, KC_F6, KC_F7,
        KC_TRNS, KC_TRNS, KC_UP, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS,
        KC_TRNS, KC_LEFT, KC_DOWN, KC_RIGHT, KC_PGDN, KC_TRNS, KC_LEFT, KC_TRNS,
        KC_RIGHT_CTRL, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_PGUP, KC_RIGHT_ALT, KC_RIGHT_GUI,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_PGDN, KC_NO, KC_NO, KC_LNG2, KC_TRNS,
        KC_VOLU, KC_VOLD, KC_MUTE, KC_PGUP, KC_MUTE, KC_TRNS, KC_TRNS, KC_TRNS,
        KC_TRNS, KC_DOWN, KC_UP, KC_RIGHT, KC_VOLD, KC_VOLU, KC_TRNS, KC_NO,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_HOME, KC_END, KC_TRNS,
        KC_TRNS, KC_TRNS, KC_TRNS, KC_F8, KC_F9, KC_F10, KC_F11, KC_F12,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO,
        KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO, KC_NO
    ),
};
