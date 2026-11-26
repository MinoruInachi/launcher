/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_memo.hpp"

#include <algorithm>

namespace {

constexpr uint32_t kBackground = 0x0D1117;
constexpr uint32_t kRow = 0x161B22;
constexpr uint32_t kRowSelected = 0x1F3A5F;
constexpr uint32_t kAccent = 0x1F6FEB;
constexpr uint32_t kLink = 0x58A6FF;
constexpr uint32_t kText = 0xE6EDF3;
constexpr uint32_t kDim = 0x8B949E;
constexpr uint32_t kHint = 0x7D8590;
constexpr uint32_t kError = 0xFF6B6B;
constexpr uint32_t kOk = 0x69AD80;

constexpr const char *kListHint = "Enter:Open  N:New  Del:Delete  Esc:Exit";
constexpr const char *kEditorHint = "Tab:Save  Esc:Back  Ctrl+Space:\xE3\x81\x82/A";

lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *object = lv_obj_create(parent);
    if (!object) return nullptr;
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_CLICKABLE);
    return object;
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (!label) return nullptr;
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_label_set_text(label, text);
    return label;
}

} // namespace

void UIMemoPage::create_ui()
{
    if (!ui_APP_Container) return;
    text_font_ = launcher_fonts().get("NotoSansCJK-Regular.ttc", 14);
    hint_font_ = launcher_fonts().get("NotoSansCJK-Regular.ttc", 12);
    if (!text_font_) text_font_ = &lv_font_montserrat_14;
    if (!hint_font_) hint_font_ = &lv_font_montserrat_12;

    background_ = make_plain(ui_APP_Container);
    if (!background_) return;
    lv_obj_set_size(background_, 320, 150);
    lv_obj_set_pos(background_, 0, 0);
    lv_obj_set_style_bg_color(background_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(background_, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(background_, static_owned_delete_cb, LV_EVENT_DELETE, this);

    list_container_ = make_plain(background_);
    editor_container_ = make_plain(background_);
    if (!list_container_ || !editor_container_) {
        lv_obj_delete(background_);
        return;
    }
    for (lv_obj_t *container : {list_container_, editor_container_}) {
        lv_obj_set_size(container, 320, 150);
        lv_obj_set_pos(container, 0, 0);
        lv_obj_add_event_cb(container, static_owned_delete_cb, LV_EVENT_DELETE, this);
    }
    lv_obj_add_flag(editor_container_, LV_OBJ_FLAG_HIDDEN);

    textarea_ = lv_textarea_create(editor_container_);
    if (textarea_) {
        lv_obj_set_size(textarea_, 320, HINT_Y);
        lv_obj_set_pos(textarea_, 0, 0);
        lv_textarea_set_max_length(textarea_, memo::kMaxMemoCharacters);
        lv_textarea_set_placeholder_text(textarea_, "Type a memo...");
        lv_obj_set_style_radius(textarea_, 0, 0);
        lv_obj_set_style_border_width(textarea_, 0, 0);
        lv_obj_set_style_bg_color(textarea_, lv_color_hex(kBackground), 0);
        lv_obj_set_style_bg_opa(textarea_, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(textarea_, 6, 0);
        lv_obj_set_style_pad_ver(textarea_, 4, 0);
        lv_obj_set_style_text_font(textarea_, text_font_, 0);
        lv_obj_set_style_text_color(textarea_, lv_color_hex(kText), 0);
        lv_obj_set_style_text_line_space(textarea_, 2, 0);
        lv_obj_set_style_text_color(textarea_, lv_color_hex(kDim), LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_set_style_text_font(textarea_, text_font_, LV_PART_TEXTAREA_PLACEHOLDER);
        // Cursor: a blinking bar. Keys are delivered by the page rather than
        // an input group, so the text area is marked focused by hand; the
        // theme styles the focused cursor too, hence both selectors.
        for (const lv_style_selector_t cursor : {static_cast<lv_style_selector_t>(LV_PART_CURSOR),
                                                 static_cast<lv_style_selector_t>(
                                                     LV_PART_CURSOR | LV_STATE_FOCUSED)}) {
            lv_obj_set_style_bg_opa(textarea_, LV_OPA_TRANSP, cursor);
            lv_obj_set_style_border_side(textarea_, LV_BORDER_SIDE_LEFT, cursor);
            lv_obj_set_style_border_width(textarea_, 2, cursor);
            lv_obj_set_style_border_color(textarea_, lv_color_hex(kLink), cursor);
            lv_obj_set_style_border_opa(textarea_, LV_OPA_COVER, cursor);
            lv_obj_set_style_anim_duration(textarea_, 500, cursor);
        }
        lv_obj_set_style_width(textarea_, 3, LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_color(textarea_, lv_color_hex(kAccent), LV_PART_SCROLLBAR);
        lv_obj_add_state(textarea_, LV_STATE_FOCUSED);
        lv_obj_add_event_cb(textarea_, static_owned_delete_cb, LV_EVENT_DELETE, this);
    }

    editor_hint_ = make_label(editor_container_, kEditorHint, hint_font_, kHint);
    if (editor_hint_) {
        lv_obj_set_pos(editor_hint_, 6, HINT_Y + 3);
        lv_obj_set_width(editor_hint_, 308);
        lv_obj_add_event_cb(editor_hint_, static_owned_delete_cb, LV_EVENT_DELETE, this);
    }
}

void UIMemoPage::update_editor_hint()
{
    if (!editor_hint_) return;
    const bool has_status = !status_message_.empty();
    lv_label_set_text(editor_hint_, has_status ? status_message_.c_str() : kEditorHint);
    lv_obj_set_style_text_color(
        editor_hint_, lv_color_hex(has_status ? (status_error_ ? kError : kOk) : kHint), 0);
}

void UIMemoPage::build_list()
{
    if (!list_container_) return;
    lv_obj_clean(list_container_);

    const std::size_t rows = list_.row_count();
    const std::size_t selected = list_.selected_row();
    if (selected < list_top_) list_top_ = selected;
    if (selected >= list_top_ + VISIBLE_ROWS) list_top_ = selected + 1 - VISIBLE_ROWS;
    if (list_top_ + VISIBLE_ROWS > rows) list_top_ = rows > VISIBLE_ROWS ? rows - VISIBLE_ROWS : 0;

    for (std::size_t visual = 0; visual < static_cast<std::size_t>(VISIBLE_ROWS); ++visual) {
        const std::size_t index = list_top_ + visual;
        if (index >= rows) break;
        const bool active = index == selected;

        lv_obj_t *row = make_plain(list_container_);
        if (!row) return;
        lv_obj_set_size(row, 308, ROW_H - 3);
        lv_obj_set_pos(row, 6, 2 + static_cast<int>(visual) * ROW_H);
        lv_obj_set_style_radius(row, 4, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(active ? kRowSelected : kRow), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, active ? 1 : 0, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(kAccent), 0);

        if (index == 0) {
            lv_obj_t *label = make_label(row, "+ New memo", text_font_, kLink);
            if (label) lv_obj_align(label, LV_ALIGN_LEFT_MID, 8, 0);
            continue;
        }
        const std::string &name = list_.files()[index - 1];
        const std::string stamp = memo::memo_timestamp_label(name);
        lv_obj_t *title = make_label(row, title_for(name).c_str(), text_font_,
                                     active ? 0xFFFFFF : kText);
        if (title) {
            lv_obj_set_width(title, 206);
            lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
        }
        // "MM-DD HH:MM" keeps the column narrow; the year is in the file name.
        lv_obj_t *time = make_label(row, stamp.size() > 5 ? stamp.substr(5).c_str() : "",
                                    hint_font_, kDim);
        if (time) lv_obj_align(time, LV_ALIGN_RIGHT_MID, -6, 0);
    }

    if (rows > static_cast<std::size_t>(VISIBLE_ROWS)) {
        // Scroll position: a thin bar on the right edge of the list area.
        const int track = VISIBLE_ROWS * ROW_H;
        const int height = std::max(8, track * VISIBLE_ROWS / static_cast<int>(rows));
        const int top = static_cast<int>((track - height) * list_top_ /
                                         (rows - static_cast<std::size_t>(VISIBLE_ROWS)));
        lv_obj_t *bar = make_plain(list_container_);
        if (bar) {
            lv_obj_set_size(bar, 2, height);
            lv_obj_set_pos(bar, 317, 2 + top);
            lv_obj_set_style_bg_color(bar, lv_color_hex(kAccent), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        }
    }

    const bool has_status = !status_message_.empty();
    lv_obj_t *hint = make_label(list_container_, has_status ? status_message_.c_str() : kListHint,
                                hint_font_,
                                has_status ? (status_error_ ? kError : kOk) : kHint);
    if (hint) {
        lv_obj_set_pos(hint, 6, HINT_Y + 3);
        lv_obj_set_width(hint, 308);
    }
}
