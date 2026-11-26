/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct key_item;

/* LVGL thread only. Delivers a synthesized key to the active screen
 * (LV_EVENT_KEYBOARD) and the global key handler, bypassing the screensaver,
 * key filter and IME stages. Used for IME commits and forwarded keys. */
void cp0_keyboard_deliver_key(const struct key_item *item);

#ifdef __cplusplus
}
#endif
