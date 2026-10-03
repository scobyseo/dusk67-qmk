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

/* VIA layout options carry 9 bits (split backspace / ISO enter / split
 * LShift / space row variant / 3-bit indicator colour) -> 2 bytes; see the
 * keyboard-level config.h. */

#define DYNAMIC_KEYMAP_LAYER_COUNT 6

/* Indicator LEDs: one physical LED (caps lock) on B15, colour set from the
 * VIA layout options ("CapsLock Color"). */
#define PHY_INDICATOR_NUM    1
#define WS2812_LED_COUNT     PHY_INDICATOR_NUM
#define INDICATOR_VAL        255
