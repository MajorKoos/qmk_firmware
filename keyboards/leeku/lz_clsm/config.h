// Copyright 2021 MajorKoos (@MajorKoos)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

/* LeeKu "L3" companion-chip lighting protocol (i2c).
 *
 * The board has no directly-driven WS2812 chain. RGB underglow, the single-level
 * backlight and the three lock LEDs are all handled by a companion MCU that is
 * spoken to over i2c using the "tinycmd" packet protocol implemented in lz_clsm.c.
 *
 * The i2c timeout is deliberately small. This is a V-USB (software USB) board: a
 * blocking i2c transfer runs in the same cooperative loop as usbPoll(), so a long
 * timeout would starve USB servicing and cause the keyboard to hang or repeat
 * keys. Keep this well under the ~50 ms V-USB polling window.
 */
#define L3_I2C_ADDRESS 0xB0
#define L3_I2C_TIMEOUT 5
