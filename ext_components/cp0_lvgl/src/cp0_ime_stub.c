/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* No-op IME API used when CONFIG_CP0_LVGL_IME_FCITX5 is disabled, so apps
 * can call cp0_ime_* unconditionally. */

#include "cp0_ime.h"

#if !defined(CP0_LVGL_IME_FCITX5) || !CP0_LVGL_IME_FCITX5

void cp0_ime_init(void) {}
void cp0_ime_deinit(void) {}
int cp0_ime_filter_key(const struct key_item *item)
{
    (void)item;
    return 0;
}
void cp0_ime_push_suppress(void) {}
void cp0_ime_pop_suppress(void) {}
cp0_ime_state_t cp0_ime_get_state(void) { return CP0_IME_STATE_UNAVAILABLE; }

#endif
