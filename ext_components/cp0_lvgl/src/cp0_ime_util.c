/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_ime_util.h"

#include "cp0_keyboard_text.h"
#include "input_keys.h"
#include "keyboard_input.h"

/* X11 keysyms used below (values from X11/keysymdef.h). */
#define XK_BackSpace 0xff08u
#define XK_Tab 0xff09u
#define XK_Return 0xff0du
#define XK_Escape 0xff1bu
#define XK_Muhenkan 0xff22u
#define XK_Henkan 0xff23u
#define XK_Zenkaku_Hankaku 0xff2au
#define XK_Home 0xff50u
#define XK_Left 0xff51u
#define XK_Up 0xff52u
#define XK_Right 0xff53u
#define XK_Down 0xff54u
#define XK_Prior 0xff55u
#define XK_Next 0xff56u
#define XK_End 0xff57u
#define XK_Insert 0xff63u
#define XK_KP_Enter 0xff8du
#define XK_F1 0xffbeu
#define XK_Shift_L 0xffe1u
#define XK_Shift_R 0xffe2u
#define XK_Control_L 0xffe3u
#define XK_Control_R 0xffe4u
#define XK_Caps_Lock 0xffe5u
#define XK_Alt_L 0xffe9u
#define XK_Alt_R 0xffeau
#define XK_Super_L 0xffebu
#define XK_Super_R 0xffecu
#define XK_Delete 0xffffu
#define XK_space 0x0020u

struct special_key {
    uint32_t evdev;
    uint32_t keysym;
};

static const struct special_key special_keys[] = {
    {KEY_ESC, XK_Escape},
    {KEY_BACKSPACE, XK_BackSpace},
    {KEY_TAB, XK_Tab},
    {KEY_ENTER, XK_Return},
    {KEY_KPENTER, XK_KP_Enter},
    {KEY_SPACE, XK_space},
    {KEY_HOME, XK_Home},
    {KEY_END, XK_End},
    {KEY_UP, XK_Up},
    {KEY_DOWN, XK_Down},
    {KEY_LEFT, XK_Left},
    {KEY_RIGHT, XK_Right},
    {KEY_PAGEUP, XK_Prior},
    {KEY_PAGEDOWN, XK_Next},
    {KEY_INSERT, XK_Insert},
    {KEY_DELETE, XK_Delete},
    {KEY_LEFTSHIFT, XK_Shift_L},
    {KEY_RIGHTSHIFT, XK_Shift_R},
    {KEY_LEFTCTRL, XK_Control_L},
    {KEY_RIGHTCTRL, XK_Control_R},
    {KEY_CAPSLOCK, XK_Caps_Lock},
    {KEY_LEFTALT, XK_Alt_L},
    {KEY_RIGHTALT, XK_Alt_R},
    {KEY_LEFTMETA, XK_Super_L},
    {KEY_RIGHTMETA, XK_Super_R},
#ifdef KEY_HENKAN
    {KEY_HENKAN, XK_Henkan},
#endif
#ifdef KEY_MUHENKAN
    {KEY_MUHENKAN, XK_Muhenkan},
#endif
#ifdef KEY_ZENKAKUHANKAKU
    {KEY_ZENKAKUHANKAKU, XK_Zenkaku_Hankaku},
#endif
};

uint32_t cp0_ime_state_from_mods(uint32_t mods)
{
    uint32_t state = 0;
    if (mods & KBD_MOD_SHIFT) state |= CP0_IME_KEYSTATE_SHIFT;
    if (mods & KBD_MOD_CAPS) state |= CP0_IME_KEYSTATE_LOCK;
    if (mods & KBD_MOD_CTRL) state |= CP0_IME_KEYSTATE_CTRL;
    if (mods & KBD_MOD_ALT) state |= CP0_IME_KEYSTATE_ALT;
    if (mods & KBD_MOD_NUM) state |= CP0_IME_KEYSTATE_NUM;
    if (mods & KBD_MOD_LOGO) state |= CP0_IME_KEYSTATE_SUPER;
    return state;
}

uint32_t cp0_ime_state_for_key(uint32_t keysym, uint32_t state, int release)
{
    uint32_t own = 0;
    switch (keysym) {
    case XK_Shift_L: case XK_Shift_R: own = CP0_IME_KEYSTATE_SHIFT; break;
    case XK_Control_L: case XK_Control_R: own = CP0_IME_KEYSTATE_CTRL; break;
    case XK_Alt_L: case XK_Alt_R: own = CP0_IME_KEYSTATE_ALT; break;
    case XK_Super_L: case XK_Super_R: own = CP0_IME_KEYSTATE_SUPER; break;
    default: return state;
    }
    return release ? (state | own) : (state & ~own);
}

static int printable(uint32_t codepoint)
{
    return codepoint >= 0x20 && codepoint != 0x7f &&
           !(codepoint >= 0x80 && codepoint < 0xa0) && codepoint <= 0x10ffff &&
           !(codepoint >= 0xd800 && codepoint <= 0xdfff);
}

uint32_t cp0_ime_keysym_from_codepoint(uint32_t codepoint)
{
    if (!printable(codepoint)) return 0;
    if (codepoint < 0x100) return codepoint;
    return 0x01000000u | codepoint;
}

uint32_t cp0_ime_codepoint_from_keysym(uint32_t keysym)
{
    if (keysym >= 0x20 && keysym < 0x7f) return keysym;
    if (keysym >= 0xa0 && keysym <= 0xff) return keysym;
    if ((keysym & 0xff000000u) == 0x01000000u) {
        const uint32_t codepoint = keysym & 0x00ffffffu;
        return printable(codepoint) ? codepoint : 0;
    }
    return 0;
}

uint32_t cp0_ime_keysym_for_special_evdev(uint32_t evdev_code)
{
    for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); ++i)
        if (special_keys[i].evdev == evdev_code) return special_keys[i].keysym;
    if (evdev_code >= KEY_F1 && evdev_code <= KEY_F10)
        return XK_F1 + (evdev_code - KEY_F1);
    if (evdev_code == KEY_F11) return XK_F1 + 10;
    if (evdev_code == KEY_F12) return XK_F1 + 11;
    return 0;
}

uint32_t cp0_ime_evdev_for_special_keysym(uint32_t keysym)
{
    if (keysym == XK_space) return 0; /* space is text */
    for (size_t i = 0; i < sizeof(special_keys) / sizeof(special_keys[0]); ++i)
        if (special_keys[i].keysym == keysym) return special_keys[i].evdev;
    if (keysym >= XK_F1 && keysym < XK_F1 + 10) return KEY_F1 + (keysym - XK_F1);
    if (keysym == XK_F1 + 10) return KEY_F11;
    if (keysym == XK_F1 + 11) return KEY_F12;
    return 0;
}

uint32_t cp0_ime_keysym_for_text_evdev(uint32_t evdev_code, int shifted)
{
    static const char row_q[] = "qwertyuiop";
    static const char row_a[] = "asdfghjkl";
    static const char row_z[] = "zxcvbnm";
    static const char digits[] = "1234567890";
    static const char digits_shifted[] = "!@#$%^&*()";
    char character = 0;
    if (evdev_code >= KEY_Q && evdev_code <= KEY_P)
        character = row_q[evdev_code - KEY_Q];
    else if (evdev_code >= KEY_A && evdev_code <= KEY_L)
        character = row_a[evdev_code - KEY_A];
    else if (evdev_code >= KEY_Z && evdev_code <= KEY_M)
        character = row_z[evdev_code - KEY_Z];
    else if (evdev_code >= KEY_1 && evdev_code <= KEY_0)
        return (uint32_t)(shifted ? digits_shifted : digits)[evdev_code - KEY_1];
    else if (evdev_code == KEY_MINUS)
        return shifted ? '_' : '-';
    else if (evdev_code == KEY_COMMA)
        return shifted ? '<' : ',';
    else if (evdev_code == KEY_DOT)
        return shifted ? '>' : '.';
    if (!character) return 0;
    return (uint32_t)(shifted ? character - 'a' + 'A' : character);
}

uint32_t cp0_ime_pick_keysym(uint32_t evdev_code, uint32_t codepoint,
                             const char *utf8, uint32_t native_sym, int shifted)
{
    /* Text injected without a physical key (RPC text, IME commits) is raw. */
    if (evdev_code == 0) return 0;

    const uint32_t special = cp0_ime_keysym_for_special_evdev(evdev_code);
    if (special) return special;

    uint32_t keysym = cp0_ime_keysym_from_codepoint(codepoint);
    if (keysym) return keysym;

    if (utf8 && utf8[0]) {
        uint32_t decoded = 0;
        size_t length = 0;
        if (cp0_keyboard_utf8_decode_one(utf8, &decoded, &length) == 0) {
            keysym = cp0_ime_keysym_from_codepoint(decoded);
            if (keysym) return keysym;
        }
    }

    /* Vendor keysyms (XF86 media/brightness keys, 0x1008xxxx) belong to the
     * launcher's global shortcuts, never to the IME. */
    if (native_sym && native_sym < 0x10000000u) return native_sym;
    if (native_sym) return 0;
    return cp0_ime_keysym_for_text_evdev(evdev_code, shifted);
}

int cp0_ime_candidate_window(const int *widths, int count, int cursor,
                             int max_width, int gap, int *first, int *last)
{
    if (first) *first = 0;
    if (last) *last = -1;
    if (!widths || count <= 0) return 0;
    if (cursor < 0) cursor = 0;
    if (cursor >= count) cursor = count - 1;
    if (gap < 0) gap = 0;

    /* Leftmost start that still keeps the cursor candidate on screen. */
    int start = cursor;
    int used = widths[cursor];
    while (start > 0 && used + gap + widths[start - 1] <= max_width) {
        --start;
        used += gap + widths[start];
    }
    /* Prefer page-like behaviour: begin at 0 when everything up to the
     * cursor fits, so the window does not jitter while moving right. */
    int prefix = 0;
    for (int i = 0; i <= cursor; ++i) prefix += widths[i] + (i ? gap : 0);
    if (prefix <= max_width) {
        start = 0;
        used = prefix;
    }

    int end = cursor;
    while (end + 1 < count && used + gap + widths[end + 1] <= max_width) {
        ++end;
        used += gap + widths[end];
    }
    if (first) *first = start;
    if (last) *last = end;
    return end - start + 1;
}
