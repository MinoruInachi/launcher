/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* LVGL side of the fcitx5 bridge: decides when the IME is focused, turns
 * key_item events into ProcessKeyEvent calls, delivers commits to the page
 * and draws the pre-edit / candidate panel on lv_layer_top(). */

#include "cp0_ime.h"

#include "cp0_fcitx5_client.hpp"

#include "../cp0_ime_util.h"
#include "../cp0_keyboard_deliver.h"
#include "../cp0_keyboard_key_contract.h"
#include "../cp0_keyboard_text.h"
#include "cp0_keyboard_navigation_contract.h"
#include "keyboard_input.h"
#include "lvgl/lvgl.h"

#include <gio/gio.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

using cp0::ime::Candidate;
using cp0::ime::CurrentInputMethod;
using cp0::ime::Fcitx5Client;
using cp0::ime::PanelState;
using cp0::ime::TextSegment;

constexpr uint32_t kIdlePeriodMs = 500;
constexpr uint32_t kFocusedPeriodMs = 40;
constexpr uint32_t kRetryStartMs = 5000;
constexpr int kDefaultKeyTimeoutMs = 800;
constexpr int kPanelPadX = 4;
constexpr int kCandidateGap = 6;
constexpr int kCandidatePadX = 3;

const lv_color_t kPanelBg = lv_color_hex(0x16181d);
const lv_color_t kPanelBorder = lv_color_hex(0x3d7eff);
const lv_color_t kText = lv_color_hex(0xf2f2f2);
const lv_color_t kDim = lv_color_hex(0x9aa0aa);
const lv_color_t kAccent = lv_color_hex(0x3d7eff);

uint32_t monotonic_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000u + now.tv_nsec / 1000000u);
}

bool env_disabled(const char *name)
{
    const char *value = std::getenv(name);
    return value && (std::strcmp(value, "0") == 0 || std::strcmp(value, "off") == 0);
}

int env_int(const char *name, int fallback, int min, int max)
{
    const char *value = std::getenv(name);
    if (!value || !value[0]) return fallback;
    const int parsed = std::atoi(value);
    return parsed < min || parsed > max ? fallback : parsed;
}

struct Runtime
{
    GMainContext *context = nullptr;
    Fcitx5Client client;
    lv_timer_t *timer = nullptr;
    bool started = false;
    int key_timeout_ms = kDefaultKeyTimeoutMs;
    uint32_t next_start_ms = 0;
    bool available = false;
    bool focused = false;
    int suppress = 0;
    bool composing_im = false;
    std::string im_badge = "A";
    PanelState panel;

    const lv_font_t *font = nullptr;
    lv_font_t *owned_font = nullptr;
    lv_obj_t *panel_obj = nullptr;
    lv_obj_t *preedit_row = nullptr;
    lv_obj_t *candidate_row = nullptr;
    lv_obj_t *badge = nullptr;
};

Runtime *rt = nullptr;

/* Name fcitx5 shows for the input context (APPLaunch, ZClaw, ...). */
std::string program_name()
{
#if defined(__GLIBC__)
    if (program_invocation_short_name && program_invocation_short_name[0])
        return program_invocation_short_name;
#elif defined(__APPLE__)
    if (const char *name = getprogname()) return name;
#endif
    return "cp0_lvgl";
}

/* ---------- fonts ---------- */

const lv_font_t *load_font()
{
    const int size = env_int("CP0_IME_FONT_SIZE", 14, 8, 48);
#if LV_USE_FREETYPE
    std::vector<std::string> candidates;
    if (const char *path = std::getenv("CP0_IME_FONT")) candidates.emplace_back(path);
    candidates.emplace_back("share/font/NotoSansCJK-Regular.ttc");
    candidates.emplace_back("/usr/share/APPLaunch/share/font/NotoSansCJK-Regular.ttc");
    candidates.emplace_back("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc");
    for (const auto &path : candidates) {
        if (path.empty() || access(path.c_str(), R_OK) != 0) continue;
        lv_font_t *font = lv_freetype_font_create(
            path.c_str(), LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
            static_cast<uint32_t>(size), LV_FREETYPE_FONT_STYLE_NORMAL);
        if (font) {
            rt->owned_font = font;
            return font;
        }
    }
    std::fprintf(stderr, "[IME] no CJK font found; set CP0_IME_FONT\n");
#else
    (void)size;
#endif
    return LV_FONT_DEFAULT;
}

/* ---------- panel drawing ---------- */

lv_obj_t *make_box(lv_obj_t *parent)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

lv_obj_t *make_text(lv_obj_t *parent, const std::string &text, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_style_text_font(label, rt->font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(label, text.c_str());
    return label;
}

int text_width(const std::string &text)
{
    lv_point_t size{};
    lv_text_get_size(&size, text.c_str(), rt->font, 0, 0, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    return size.x;
}

/* The display (and lv_layer_top) may be torn down before the keypad. */
void on_ui_deleted(lv_event_t *event)
{
    if (!rt) return;
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (target == rt->panel_obj) rt->panel_obj = rt->preedit_row = rt->candidate_row = nullptr;
    if (target == rt->badge) rt->badge = nullptr;
}

void ensure_ui()
{
    if (rt->panel_obj && rt->badge) return;
    if (rt->panel_obj) lv_obj_delete(rt->panel_obj);
    if (rt->badge) lv_obj_delete(rt->badge);
    lv_obj_t *layer = lv_layer_top();

    rt->panel_obj = make_box(layer);
    lv_obj_set_width(rt->panel_obj, lv_pct(100));
    lv_obj_set_height(rt->panel_obj, LV_SIZE_CONTENT);
    lv_obj_align(rt->panel_obj, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(rt->panel_obj, kPanelBg, 0);
    lv_obj_set_style_bg_opa(rt->panel_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(rt->panel_obj, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(rt->panel_obj, 1, 0);
    lv_obj_set_style_border_color(rt->panel_obj, kPanelBorder, 0);
    lv_obj_set_style_pad_hor(rt->panel_obj, kPanelPadX, 0);
    lv_obj_set_style_pad_ver(rt->panel_obj, 2, 0);
    lv_obj_set_style_pad_row(rt->panel_obj, 1, 0);
    lv_obj_set_flex_flow(rt->panel_obj, LV_FLEX_FLOW_COLUMN);

    rt->preedit_row = make_box(rt->panel_obj);
    rt->candidate_row = make_box(rt->panel_obj);
    for (lv_obj_t *row : {rt->preedit_row, rt->candidate_row}) {
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }
    lv_obj_set_style_pad_column(rt->candidate_row, kCandidateGap, 0);

    rt->badge = make_box(layer);
    lv_obj_set_size(rt->badge, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(rt->badge, LV_OPA_80, 0);
    lv_obj_set_style_radius(rt->badge, 3, 0);
    lv_obj_set_style_pad_hor(rt->badge, 3, 0);
    lv_obj_set_style_pad_ver(rt->badge, 0, 0);
    lv_obj_set_style_text_font(rt->badge, rt->font, 0);
    lv_obj_t *badge_label = lv_label_create(rt->badge);
    lv_obj_set_style_text_color(badge_label, kText, 0);
    lv_label_set_text(badge_label, "");

    lv_obj_add_flag(rt->panel_obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(rt->badge, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(rt->panel_obj, on_ui_deleted, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(rt->badge, on_ui_deleted, LV_EVENT_DELETE, nullptr);
}

void destroy_ui()
{
    if (rt->panel_obj) lv_obj_delete(rt->panel_obj); /* on_ui_deleted clears */
    if (rt->badge) lv_obj_delete(rt->badge);
    rt->panel_obj = rt->preedit_row = rt->candidate_row = rt->badge = nullptr;
}

void fill_preedit_row()
{
    lv_obj_clean(rt->preedit_row);
    bool any = false;
    for (const auto &segment : rt->panel.aux_up) {
        if (segment.text.empty()) continue;
        make_text(rt->preedit_row, segment.text, kDim);
        any = true;
    }
    for (const auto &segment : rt->panel.preedit) {
        if (segment.text.empty()) continue;
        const bool highlight = segment.format & CP0_IME_FORMAT_HIGHLIGHT;
        lv_obj_t *label = make_text(rt->preedit_row, segment.text, kText);
        if (highlight) {
            lv_obj_set_style_bg_color(label, kAccent, 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_decor(label, LV_TEXT_DECOR_UNDERLINE, 0);
        }
        any = true;
    }
    if (any)
        lv_obj_remove_flag(rt->preedit_row, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(rt->preedit_row, LV_OBJ_FLAG_HIDDEN);
}

std::string candidate_text(const Candidate &candidate)
{
    std::string label = candidate.label;
    while (!label.empty() && label.back() == ' ') label.pop_back();
    return label.empty() ? candidate.text : label + candidate.text;
}

void fill_candidate_row()
{
    lv_obj_clean(rt->candidate_row);
    const auto &candidates = rt->panel.candidates;
    std::string aux_down;
    for (const auto &segment : rt->panel.aux_down) aux_down += segment.text;

    if (candidates.empty() && aux_down.empty()) {
        lv_obj_add_flag(rt->candidate_row, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(rt->candidate_row, LV_OBJ_FLAG_HIDDEN);

    const int screen = lv_display_get_horizontal_resolution(nullptr);
    const int arrow_width = text_width(">") + kCandidateGap;
    int max_width = screen - 2 * kPanelPadX;

    std::vector<std::string> texts;
    std::vector<int> widths;
    for (const auto &candidate : candidates) {
        texts.push_back(candidate_text(candidate));
        widths.push_back(text_width(texts.back()) + 2 * kCandidatePadX);
    }

    int first = 0;
    int last = -1;
    if (!candidates.empty()) {
        const int cursor = rt->panel.candidate_cursor < 0 ? 0 : rt->panel.candidate_cursor;
        cp0_ime_candidate_window(widths.data(), static_cast<int>(widths.size()), cursor,
                                 max_width - 2 * arrow_width, kCandidateGap, &first, &last);
    }
    const bool more_left = rt->panel.has_prev || first > 0;
    const bool more_right =
        rt->panel.has_next || (last >= 0 && last + 1 < static_cast<int>(candidates.size()));

    if (more_left) make_text(rt->candidate_row, "<", kDim);
    for (int i = first; i <= last; ++i) {
        lv_obj_t *label = make_text(rt->candidate_row, texts[i], kText);
        lv_obj_set_style_pad_hor(label, kCandidatePadX, 0);
        lv_obj_set_style_radius(label, 2, 0);
        if (i == rt->panel.candidate_cursor) {
            lv_obj_set_style_bg_color(label, kAccent, 0);
            lv_obj_set_style_bg_opa(label, LV_OPA_COVER, 0);
        }
    }
    if (more_right) make_text(rt->candidate_row, ">", kDim);
    if (!aux_down.empty()) make_text(rt->candidate_row, aux_down, kDim);
}

void render()
{
    if (!rt) return;
    const bool show_panel = rt->focused && !rt->panel.empty();
    const bool show_badge = rt->focused && !env_disabled("CP0_IME_BADGE");
    if (!show_panel && !show_badge && !rt->panel_obj && !rt->badge) return;
    ensure_ui();

    if (show_panel) {
        fill_preedit_row();
        fill_candidate_row();
        lv_obj_remove_flag(rt->panel_obj, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(rt->panel_obj);
    } else {
        lv_obj_add_flag(rt->panel_obj, LV_OBJ_FLAG_HIDDEN);
    }

    if (show_badge) {
        lv_obj_t *label = lv_obj_get_child(rt->badge, 0);
        lv_label_set_text(label, rt->im_badge.c_str());
        lv_obj_set_style_bg_color(rt->badge, rt->composing_im ? kAccent : lv_color_hex(0x444a55), 0);
        int offset = 0;
        if (show_panel) {
            lv_obj_update_layout(rt->panel_obj);
            offset = lv_obj_get_height(rt->panel_obj);
        }
        lv_obj_align(rt->badge, LV_ALIGN_BOTTOM_RIGHT, -1, -offset - 1);
        lv_obj_remove_flag(rt->badge, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(rt->badge);
    } else {
        lv_obj_add_flag(rt->badge, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---------- delivery to the page ---------- */

void deliver_codepoint(uint32_t codepoint)
{
    struct key_item item;
    std::memset(&item, 0, sizeof(item));
    item.key_state = KBD_KEY_PRESSED;
    item.input_context = cp0_keyboard_get_input_context();
    std::snprintf(item.sym_name, sizeof(item.sym_name), "IME_COMMIT");

    uint32_t evdev = 0;
    if (codepoint == '\n' || codepoint == '\r') evdev = KEY_ENTER;
    if (codepoint == '\t') evdev = KEY_TAB;
    if (evdev) {
        item.key_code = evdev;
        item.semantic_key = cp0_keyboard_semantic_key(evdev, item.input_context);
        const char *control = cp0_keyboard_control_utf8(evdev);
        if (control) std::snprintf(item.utf8, sizeof(item.utf8), "%s", control);
    } else {
        if (codepoint < 0x20 || codepoint == 0x7f) return;
        if (cp0_keyboard_utf32_to_utf8(codepoint, item.utf8, sizeof(item.utf8)) < 0) return;
        item.codepoint = codepoint;
        item.keysym = cp0_ime_keysym_from_codepoint(codepoint);
    }
    cp0_keyboard_deliver_key(&item);
}

void deliver_commit(const std::string &text)
{
    if (!rt->focused) return; /* never type into a page that left text mode */
    const char *cursor = text.c_str();
    while (*cursor) {
        uint32_t codepoint = 0;
        size_t length = 0;
        if (cp0_keyboard_utf8_decode_one(cursor, &codepoint, &length) != 0) break;
        deliver_codepoint(codepoint);
        cursor += length;
    }
}

void deliver_forward_key(uint32_t keysym, uint32_t state, bool release)
{
    if (!rt->focused) return;
    struct key_item item;
    std::memset(&item, 0, sizeof(item));
    item.key_state = release ? KBD_KEY_RELEASED : KBD_KEY_PRESSED;
    item.input_context = cp0_keyboard_get_input_context();
    item.keysym = keysym;
    if (state & CP0_IME_KEYSTATE_SHIFT) item.mods |= KBD_MOD_SHIFT;
    if (state & CP0_IME_KEYSTATE_CTRL) item.mods |= KBD_MOD_CTRL;
    if (state & CP0_IME_KEYSTATE_ALT) item.mods |= KBD_MOD_ALT;
    std::snprintf(item.sym_name, sizeof(item.sym_name), "IME_FORWARD");

    const uint32_t evdev = cp0_ime_evdev_for_special_keysym(keysym);
    if (evdev) {
        item.key_code = evdev;
        item.semantic_key = cp0_keyboard_semantic_key(evdev, item.input_context);
        const char *control = cp0_keyboard_control_utf8(evdev);
        if (control) std::snprintf(item.utf8, sizeof(item.utf8), "%s", control);
    } else {
        const uint32_t codepoint = cp0_ime_codepoint_from_keysym(keysym);
        if (!codepoint) return;
        if (release) return; /* text keys only act on press */
        cp0_keyboard_utf32_to_utf8(codepoint, item.utf8, sizeof(item.utf8));
        item.codepoint = codepoint;
    }
    cp0_keyboard_deliver_key(&item);
}

/* ---------- focus ---------- */

void set_focused(bool focused)
{
    if (rt->focused == focused) return;
    rt->focused = focused;
    if (focused) {
        rt->client.focus_in();
    } else {
        rt->client.reset();
        rt->client.focus_out();
        rt->panel = PanelState{};
    }
    if (rt->timer)
        lv_timer_set_period(rt->timer, focused ? kFocusedPeriodMs : kIdlePeriodMs);
    render();
}

void update_focus()
{
    const bool want = rt->available && rt->suppress == 0 &&
                      cp0_keyboard_get_input_context() == KBD_INPUT_CONTEXT_TEXT;
    set_focused(want);
}

void on_current_im(const CurrentInputMethod &im)
{
    rt->composing_im = im.unique_name.rfind("keyboard-", 0) != 0;
    if (!rt->composing_im)
        rt->im_badge = "A";
    else if (im.language.rfind("ja", 0) == 0)
        rt->im_badge = "\xE3\x81\x82"; /* あ */
    else
        rt->im_badge = im.name.empty() ? im.unique_name.substr(0, 2) : im.name.substr(0, 3);
    render();
}

void start_client()
{
    rt->started = true;
    Fcitx5Client::Callbacks callbacks;
    callbacks.commit = deliver_commit;
    callbacks.panel = [](const PanelState &panel) {
        rt->panel = panel;
        render();
    };
    callbacks.current_im = on_current_im;
    callbacks.forward_key = deliver_forward_key;
    callbacks.availability = [](bool ready) {
        rt->available = ready;
        if (!ready) {
            rt->focused = false;
            rt->composing_im = false;
            rt->im_badge = "A";
            rt->panel = PanelState{};
            render();
        }
        update_focus();
    };
    const uint64_t capability = CP0_IME_CAP_CLIENT_SIDE_INPUT_PANEL;
    if (!rt->client.start(rt->context, program_name(), capability, std::move(callbacks))) {
        rt->started = false;
        rt->next_start_ms = monotonic_ms() + kRetryStartMs;
    }
}

void timer_cb(lv_timer_t *)
{
    if (!rt) return;
    if (!rt->started && static_cast<int32_t>(monotonic_ms() - rt->next_start_ms) >= 0)
        start_client();
    while (g_main_context_iteration(rt->context, FALSE)) {
    }
    if (rt) update_focus();
}

} // namespace

extern "C" void cp0_ime_init(void)
{
    if (rt || env_disabled("CP0_IME")) return;
    rt = new Runtime();
    rt->context = g_main_context_new();
    rt->font = load_font();
    rt->key_timeout_ms = env_int("CP0_IME_KEY_TIMEOUT_MS", kDefaultKeyTimeoutMs, 50, 10000);
    rt->next_start_ms = monotonic_ms();
    rt->timer = lv_timer_create(timer_cb, kIdlePeriodMs, nullptr);
    lv_timer_ready(rt->timer);
}

extern "C" void cp0_ime_deinit(void)
{
    if (!rt) return;
    Runtime *runtime = rt;
    if (runtime->timer) lv_timer_delete(runtime->timer);
    runtime->timer = nullptr;
    runtime->client.stop();
    destroy_ui();
#if LV_USE_FREETYPE
    if (runtime->owned_font) lv_freetype_font_delete(runtime->owned_font);
#endif
    g_main_context_unref(runtime->context);
    rt = nullptr;
    delete runtime;
}

extern "C" int cp0_ime_filter_key(const struct key_item *item)
{
    if (!rt || !item || !rt->available) return 0;
    update_focus();
    if (!rt->focused) return 0;

    uint32_t native_sym = item->keysym;
#if defined(HAL_PLATFORM_SDL)
    native_sym = 0; /* SDL keycodes are not X keysyms */
#endif
    const bool release = item->key_state == KBD_KEY_RELEASED;
    const uint32_t keysym =
        cp0_ime_pick_keysym(item->key_code, item->codepoint, item->utf8, native_sym,
                            (item->mods & KBD_MOD_SHIFT) != 0);
    if (!keysym) return 0;
    const uint32_t state =
        cp0_ime_state_for_key(keysym, cp0_ime_state_from_mods(item->mods), release);
    const auto result =
        rt->client.process_key(keysym, item->key_code + CP0_IME_EVDEV_OFFSET, state,
                               release, monotonic_ms(), rt->key_timeout_ms);
    if (result == Fcitx5Client::KeyResult::TimedOut) {
        /* The client is resyncing: hide the stale composition. Keys go
         * straight to the page, without waiting, until fcitx5 answers. */
        std::fprintf(stderr, "[IME] fcitx5 did not answer within %d ms; bypassing it\n",
                     rt->key_timeout_ms);
        rt->panel = PanelState{};
        render();
    }
    return result == Fcitx5Client::KeyResult::Handled ? 1 : 0;
}

extern "C" void cp0_ime_push_suppress(void)
{
    if (!rt) return;
    ++rt->suppress;
    update_focus();
}

extern "C" void cp0_ime_pop_suppress(void)
{
    if (!rt || rt->suppress == 0) return;
    --rt->suppress;
    update_focus();
}

extern "C" cp0_ime_state_t cp0_ime_get_state(void)
{
    if (!rt || !rt->available) return CP0_IME_STATE_UNAVAILABLE;
    return rt->composing_im ? CP0_IME_STATE_COMPOSING : CP0_IME_STATE_DIRECT;
}
