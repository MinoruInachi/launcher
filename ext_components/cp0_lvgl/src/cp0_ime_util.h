/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* Pure helpers shared by the fcitx5 IME bridge and its tests.
 * No GLib, LVGL or platform dependencies live here. */

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* fcitx5 / X11 key state bits (fcitx::KeyState). */
#define CP0_IME_KEYSTATE_SHIFT (1u << 0)
#define CP0_IME_KEYSTATE_LOCK  (1u << 1)
#define CP0_IME_KEYSTATE_CTRL  (1u << 2)
#define CP0_IME_KEYSTATE_ALT   (1u << 3)
#define CP0_IME_KEYSTATE_NUM   (1u << 4)
#define CP0_IME_KEYSTATE_SUPER (1u << 6)

/* fcitx5 capability bits (fcitx::CapabilityFlag). */
#define CP0_IME_CAP_CLIENT_SIDE_INPUT_PANEL (1ull << 39)

/* fcitx5 text format flags (fcitx::TextFormatFlag). */
#define CP0_IME_FORMAT_UNDERLINE (1 << 3)
#define CP0_IME_FORMAT_HIGHLIGHT (1 << 4)

/* Linux evdev key code -> XKB keycode, as fcitx5 expects. */
#define CP0_IME_EVDEV_OFFSET 8u

/* KBD_MOD_* bitmask -> fcitx5 state bitmask. */
uint32_t cp0_ime_state_from_mods(uint32_t mods);

/* Applies the X11 convention that a modifier key's own bit is absent from
 * the state of its press and present in the state of its release. */
uint32_t cp0_ime_state_for_key(uint32_t keysym, uint32_t state, int release);

/* Unicode code point -> X keysym (Latin-1 direct, otherwise 0x01000000|cp). */
uint32_t cp0_ime_keysym_from_codepoint(uint32_t codepoint);

/* X keysym -> Unicode code point for printable keysyms, 0 otherwise. */
uint32_t cp0_ime_codepoint_from_keysym(uint32_t keysym);

/* Keysym for non-printing keys identified by evdev code (Return, arrows,
 * modifiers, ...). Returns 0 for keys that produce text. */
uint32_t cp0_ime_keysym_for_special_evdev(uint32_t evdev_code);

/* US-layout fallback for letter/digit/space evdev codes when an event
 * carries no text (for example RPC-injected keys). Returns 0 if unknown. */
uint32_t cp0_ime_keysym_for_text_evdev(uint32_t evdev_code, int shifted);

/* Reverse of cp0_ime_keysym_for_special_evdev: evdev code for a forwarded
 * special keysym, 0 if the keysym is not a special key. */
uint32_t cp0_ime_evdev_for_special_keysym(uint32_t keysym);

/* Picks the keysym fcitx5 should see for one key event.
 *   evdev_code   Linux key code (0 means "no physical key")
 *   codepoint    Unicode code point carried by the event (0 if none)
 *   utf8         UTF-8 text carried by the event (may be NULL)
 *   native_sym   platform keysym when it is an XKB keysym, else 0
 *   shifted      non-zero when Shift is held
 * Returns 0 when the event should not be offered to the IME. */
uint32_t cp0_ime_pick_keysym(uint32_t evdev_code, uint32_t codepoint,
                             const char *utf8, uint32_t native_sym, int shifted);

/* Chooses which candidates fit on one line of max_width pixels so that the
 * selected candidate is visible. widths[i] is the pixel width of candidate i
 * including its label; gap is the spacing between candidates. Writes the
 * inclusive range to first/last. Returns the number of candidates shown. */
int cp0_ime_candidate_window(const int *widths, int count, int cursor,
                             int max_width, int gap, int *first, int *last);

#ifdef __cplusplus
}
#endif
