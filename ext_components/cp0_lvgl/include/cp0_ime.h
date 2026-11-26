/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* Input method (IME) bridge to fcitx5.
 *
 * When CONFIG_CP0_LVGL_IME_FCITX5 is enabled, cp0_lvgl talks to a running
 * fcitx5 daemon over the D-Bus session bus. While the keyboard input context
 * is KBD_INPUT_CONTEXT_TEXT, key events are offered to fcitx5 first; keys
 * fcitx5 consumes never reach the page, and committed text is delivered to the
 * page as ordinary LV_EVENT_KEYBOARD items (key_code 0, key_state PRESSED,
 * utf8 = one code point, sym_name "IME_COMMIT"). Pre-edit text and candidates
 * are drawn by cp0_lvgl on lv_layer_top().
 *
 * Pages need no changes. Optional calls below let a page opt out (password
 * fields) or show the current mode.
 *
 * All functions must be called from the LVGL thread. */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

struct key_item;

typedef enum {
    CP0_IME_STATE_UNAVAILABLE = 0, /* feature off, or fcitx5 not running */
    CP0_IME_STATE_DIRECT = 1,      /* fcitx5 ready, keyboard (Latin) mode */
    CP0_IME_STATE_COMPOSING = 2,   /* fcitx5 ready, an input method is active */
} cp0_ime_state_t;

/* Called by the keyboard layer. Safe to call when the feature is compiled out. */
void cp0_ime_init(void);
void cp0_ime_deinit(void);
/* Returns non-zero when the IME consumed the key. */
int cp0_ime_filter_key(const struct key_item *item);

/* Nesting counter: while > 0 the IME is detached from text fields, for example
 * on password or hex-only inputs. */
void cp0_ime_push_suppress(void);
void cp0_ime_pop_suppress(void);

cp0_ime_state_t cp0_ime_get_state(void);

#ifdef __cplusplus
}
#endif
