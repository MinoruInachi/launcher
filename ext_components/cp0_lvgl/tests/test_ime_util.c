/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../src/cp0_ime_util.h"

#include "input_keys.h"
#include "keyboard_input.h"

#include <assert.h>
#include <stdio.h>

static void test_state_mapping(void)
{
    assert(cp0_ime_state_from_mods(0) == 0);
    assert(cp0_ime_state_from_mods(KBD_MOD_SHIFT) == CP0_IME_KEYSTATE_SHIFT);
    assert(cp0_ime_state_from_mods(KBD_MOD_CTRL | KBD_MOD_ALT) ==
           (CP0_IME_KEYSTATE_CTRL | CP0_IME_KEYSTATE_ALT));
    assert(cp0_ime_state_from_mods(KBD_MOD_LOGO) == CP0_IME_KEYSTATE_SUPER);
    /* Fn is a CardputerZero layer key, not an X modifier. */
    assert(cp0_ime_state_from_mods(KBD_MOD_FN) == 0);

    /* X convention: own modifier absent on press, present on release. */
    assert(cp0_ime_state_for_key(0xffe1, CP0_IME_KEYSTATE_SHIFT, 0) == 0);
    assert(cp0_ime_state_for_key(0xffe1, 0, 1) == CP0_IME_KEYSTATE_SHIFT);
    assert(cp0_ime_state_for_key('a', CP0_IME_KEYSTATE_CTRL, 0) == CP0_IME_KEYSTATE_CTRL);
}

static void test_keysyms(void)
{
    assert(cp0_ime_keysym_from_codepoint('a') == 'a');
    assert(cp0_ime_keysym_from_codepoint(0xe9) == 0xe9);
    assert(cp0_ime_keysym_from_codepoint(0x3042) == 0x01003042);
    assert(cp0_ime_keysym_from_codepoint('\r') == 0);
    assert(cp0_ime_keysym_from_codepoint(0x7f) == 0);
    assert(cp0_ime_codepoint_from_keysym(0x01003042) == 0x3042);
    assert(cp0_ime_codepoint_from_keysym('Z') == 'Z');
    assert(cp0_ime_codepoint_from_keysym(0xff0d) == 0);

    assert(cp0_ime_keysym_for_special_evdev(KEY_ENTER) == 0xff0d);
    assert(cp0_ime_keysym_for_special_evdev(KEY_BACKSPACE) == 0xff08);
    assert(cp0_ime_keysym_for_special_evdev(KEY_SPACE) == 0x20);
    assert(cp0_ime_keysym_for_special_evdev(KEY_LEFTSHIFT) == 0xffe1);
    assert(cp0_ime_keysym_for_special_evdev(KEY_F12) == 0xffc9);
    assert(cp0_ime_keysym_for_special_evdev(KEY_A) == 0);
    assert(cp0_ime_evdev_for_special_keysym(0xff0d) == KEY_ENTER);
    assert(cp0_ime_evdev_for_special_keysym(0xff51) == KEY_LEFT);
    assert(cp0_ime_evdev_for_special_keysym(0x20) == 0);
    assert(cp0_ime_evdev_for_special_keysym('a') == 0);

    assert(cp0_ime_keysym_for_text_evdev(KEY_A, 0) == 'a');
    assert(cp0_ime_keysym_for_text_evdev(KEY_M, 1) == 'M');
    assert(cp0_ime_keysym_for_text_evdev(KEY_1, 1) == '!');
    assert(cp0_ime_keysym_for_text_evdev(KEY_0, 0) == '0');
    assert(cp0_ime_keysym_for_text_evdev(KEY_DOT, 0) == '.');
    assert(cp0_ime_keysym_for_text_evdev(KEY_ESC, 0) == 0);
}

static void test_pick_keysym(void)
{
    /* raw text without a physical key is never offered to the IME */
    assert(cp0_ime_pick_keysym(0, 'a', "a", 0, 0) == 0);
    /* special keys win even when the event carries control text */
    assert(cp0_ime_pick_keysym(KEY_ENTER, 0, "\r", 0, 0) == 0xff0d);
    assert(cp0_ime_pick_keysym(KEY_BACKSPACE, 0, "\x7f", 0, 0) == 0xff08);
    /* text from the event (respects the keyboard layout) */
    assert(cp0_ime_pick_keysym(KEY_A, 'A', "A", 0, 1) == 'A');
    assert(cp0_ime_pick_keysym(KEY_A, 0, "a", 0, 0) == 'a');
    /* TCA8418 symbol layer: code 26 carries "!" */
    assert(cp0_ime_pick_keysym(26, '!', "!", 0x21, 0) == '!');
    /* native keysym, then US fallback */
    assert(cp0_ime_pick_keysym(KEY_A, 0, "", 0x61, 0) == 0x61);
    assert(cp0_ime_pick_keysym(KEY_A, 0, "", 0, 1) == 'A');
    /* media keys (XF86AudioRaiseVolume) stay with the launcher */
    assert(cp0_ime_pick_keysym(115, 0, "", 0x1008ff13, 0) == 0);
    /* Ctrl+letter often carries a control character as text */
    assert(cp0_ime_pick_keysym(KEY_J, 0, "\x0a", 0, 0) == 'j');
}

static void test_candidate_window(void)
{
    int first = -1, last = -1;
    const int widths[] = {40, 40, 40, 40, 40, 40, 40, 40, 40};
    /* 3 fit in 130 px with 5 px gaps: 40+5+40+5+40 = 130 */
    assert(cp0_ime_candidate_window(widths, 9, 0, 130, 5, &first, &last) == 3);
    assert(first == 0 && last == 2);
    assert(cp0_ime_candidate_window(widths, 9, 2, 130, 5, &first, &last) == 3);
    assert(first == 0 && last == 2);
    /* moving past the window scrolls so the cursor is the last visible one */
    assert(cp0_ime_candidate_window(widths, 9, 3, 130, 5, &first, &last) == 3);
    assert(first == 1 && last == 3);
    assert(cp0_ime_candidate_window(widths, 9, 8, 130, 5, &first, &last) == 3);
    assert(first == 6 && last == 8);
    /* a candidate wider than the line is still shown alone */
    const int wide[] = {20, 500, 20};
    assert(cp0_ime_candidate_window(wide, 3, 1, 100, 5, &first, &last) == 1);
    assert(first == 1 && last == 1);
    /* out-of-range cursor is clamped, empty list shows nothing */
    assert(cp0_ime_candidate_window(widths, 9, -1, 130, 5, &first, &last) == 3);
    assert(first == 0);
    assert(cp0_ime_candidate_window(widths, 0, 0, 130, 5, &first, &last) == 0);
}

int main(void)
{
    test_state_mapping();
    test_keysyms();
    test_pick_keysym();
    test_candidate_window();
    puts("test_ime_util: ok");
    return 0;
}
