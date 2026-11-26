/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "ui_app_memo.hpp"

#include "../keyboard_text_input.hpp"
#include "sample_log.h"

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <utility>

namespace {

constexpr const char *kDiscardPrompt = "Save failed - Esc again to discard";

std::string home_directory()
{
    const char *home = std::getenv("HOME");
    return home && home[0] ? home : "/root";
}

int filesystem_call(std::list<std::string> arguments, std::string *data = nullptr)
{
    int result = -1;
    try {
        cp0_signal_filesystem_api(std::move(arguments), [&](int code, std::string output) {
            result = code;
            if (data) *data = std::move(output);
        });
    } catch (...) {
        result = -1;
    }
    return result;
}

} // namespace

UIMemoPage::UIMemoPage() : AppPage()
{
    set_page_title("MEMO");
    memo_dir_ = home_directory() + "/Documents/Memo";
    try {
        create_ui();
        if (!root_screen_ || !background_) return;
        lv_obj_add_event_cb(root_screen_, UIMemoPage::static_event_cb, LV_EVENT_ALL, this);
        save_input_mode();
        set_input_mode(KBD_INPUT_CONTEXT_NAVIGATION);
        reload_files();
        build_list();
    } catch (...) {
        SLOGE("[MEMO] page construction failed");
    }
}

UIMemoPage::~UIMemoPage()
{
    try {
        if (view_state_ == ViewState::EDITOR && textarea_ && !persist_editor())
            SLOGE("[MEMO] unable to save %s on exit", editing_file_.c_str());
    } catch (...) {
    }
    restore_input_mode();
    detach_callbacks();
    if (root_screen_)
        lv_obj_remove_event_cb_with_user_data(root_screen_, UIMemoPage::static_event_cb, this);
}

// ==================== storage ====================

bool UIMemoPage::ensure_memo_dir()
{
    const std::string documents = home_directory() + "/Documents";
    if (filesystem_call({"EnsureDirForUser", documents}) != 0) return false;
    return filesystem_call({"EnsureDirForUser", memo_dir_}) == 0;
}

std::string UIMemoPage::memo_path(const std::string &name) const
{
    return memo_dir_ + "/" + name;
}

void UIMemoPage::reload_files()
{
    std::string listing;
    if (filesystem_call({"DirList", memo_dir_}, &listing) != 0) listing.clear();
    list_.set_files(memo::parse_memo_dir_list(listing));
}

int UIMemoPage::read_memo(const std::string &name, std::string &text) const
{
    std::string raw;
    const int code = filesystem_call(
        {"ReadFile", memo_path(name), std::to_string(memo::kMaxMemoBytes)}, &raw);
    if (code == 0) text = memo::sanitize_memo_text(raw);
    return code;
}

const std::string &UIMemoPage::title_for(const std::string &name)
{
    auto found = title_cache_.find(name);
    if (found != title_cache_.end()) return found->second;
    std::string text;
    std::string title;
    if (read_memo(name, text) == 0) {
        title = memo::memo_title(text);
        if (title.empty()) title = "(empty)";
    } else {
        title = "(unreadable)";
    }
    return title_cache_.emplace(name, std::move(title)).first->second;
}

bool UIMemoPage::write_memo(const std::string &name, const std::string &text)
{
    if (!ensure_memo_dir()) {
        SLOGE("[MEMO] cannot create %s", memo_dir_.c_str());
        return false;
    }
    const int code = filesystem_call({"WriteFile", memo_path(name), text});
    if (code != 0) SLOGE("[MEMO] WriteFile %s failed: %d", name.c_str(), code);
    return code == 0;
}

bool UIMemoPage::remove_memo(const std::string &name)
{
    title_cache_.erase(name);
    if (filesystem_call({"Remove", memo_path(name)}) == 0) return true;
    std::string exists;
    return filesystem_call({"Exists", memo_path(name)}, &exists) == 0 && exists == "0";
}

// ==================== editor flow ====================

void UIMemoPage::set_status(std::string message, bool error)
{
    status_message_ = std::move(message);
    status_error_ = error;
    if (view_state_ == ViewState::EDITOR)
        update_editor_hint();
    else
        build_list();
}

bool UIMemoPage::persist_editor()
{
    if (!textarea_) return true;
    const char *raw = lv_textarea_get_text(textarea_);
    const std::string text = raw ? raw : "";
    if (text == saved_text_) return true;

    if (memo::memo_title(text).empty()) {
        // A blank memo is not worth a file: drop it.
        if (!editing_file_.empty() && !remove_memo(editing_file_)) return false;
        editing_file_.clear();
        saved_text_ = text;
        return true;
    }

    if (editing_file_.empty()) {
        reload_files();
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        const std::string name = memo::make_memo_filename(local, list_.files());
        if (name.empty()) return false;
        if (!write_memo(name, text)) return false;
        editing_file_ = name;
    } else if (!write_memo(editing_file_, text)) {
        return false;
    }
    saved_text_ = text;
    title_cache_[editing_file_] = memo::memo_title(text);
    return true;
}

void UIMemoPage::open_editor(const std::string &name)
{
    std::string text;
    if (!name.empty()) {
        const int code = read_memo(name, text);
        if (code != 0) {
            set_status(code == -EFBIG ? "Memo is too large to open" : "Unable to open memo", true);
            return;
        }
    }
    editing_file_ = name;
    saved_text_ = text;
    status_message_.clear();
    status_error_ = false;
    view_state_ = ViewState::EDITOR;
    if (list_container_) lv_obj_add_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    if (editor_container_) lv_obj_remove_flag(editor_container_, LV_OBJ_FLAG_HIDDEN);
    if (textarea_) {
        lv_textarea_set_text(textarea_, text.c_str());
        lv_textarea_set_cursor_pos(textarea_, LV_TEXTAREA_CURSOR_LAST);
    }
    update_editor_hint();
    set_input_mode(KBD_INPUT_CONTEXT_TEXT);
}

bool UIMemoPage::close_editor()
{
    if (!persist_editor()) {
        status_message_ = kDiscardPrompt;
        status_error_ = true;
        update_editor_hint();
        return false;
    }
    const std::string selected = editing_file_;
    view_state_ = ViewState::LIST;
    status_message_.clear();
    set_input_mode(KBD_INPUT_CONTEXT_NAVIGATION);
    if (editor_container_) lv_obj_add_flag(editor_container_, LV_OBJ_FLAG_HIDDEN);
    if (textarea_) lv_textarea_set_text(textarea_, "");
    editing_file_.clear();
    saved_text_.clear();
    reload_files();
    list_.select_file(selected);
    if (list_container_) lv_obj_remove_flag(list_container_, LV_OBJ_FLAG_HIDDEN);
    build_list();
    return true;
}

void UIMemoPage::delete_selected()
{
    const std::string name = list_.selected_file();
    if (name.empty()) return;
    const bool removed = remove_memo(name);
    reload_files();
    set_status(removed ? "Deleted" : "Unable to delete memo", !removed);
}

// ==================== input ====================

void UIMemoPage::save_input_mode()
{
    if (input_context_saved_) return;
    previous_input_context_ = cp0_keyboard_get_input_context();
    previous_keypad_intercept_ = cp0_keyboard_get_lvgl_keypad_intercept();
    input_context_saved_ = true;
    cp0_keyboard_set_lvgl_keypad_intercept(1);
}

void UIMemoPage::set_input_mode(cp0_keyboard_input_context_t context)
{
    if (input_context_saved_) cp0_keyboard_set_input_context(context);
}

void UIMemoPage::restore_input_mode()
{
    if (!input_context_saved_) return;
    cp0_keyboard_set_input_context(previous_input_context_);
    cp0_keyboard_set_lvgl_keypad_intercept(previous_keypad_intercept_);
    input_context_saved_ = false;
}

void UIMemoPage::handle_list_key(const key_item *item)
{
    const memo::ListAction action = memo::list_action(item);
    if (action == memo::ListAction::NONE) return;
    if (action != memo::ListAction::DELETE && list_.delete_armed()) {
        list_.cancel_delete();
        status_message_.clear();
    }
    switch (action) {
    case memo::ListAction::UP:
        list_.move_up();
        status_message_.clear();
        build_list();
        break;
    case memo::ListAction::DOWN:
        list_.move_down();
        status_message_.clear();
        build_list();
        break;
    case memo::ListAction::OPEN:
        open_editor(list_.selected_file());
        break;
    case memo::ListAction::NEW:
        open_editor("");
        break;
    case memo::ListAction::DELETE:
        if (list_.request_delete())
            delete_selected();
        else if (list_.delete_armed())
            set_status("Press Del again to delete", true);
        break;
    case memo::ListAction::EXIT:
        if (navigate_home) navigate_home();
        break;
    case memo::ListAction::NONE:
        break;
    }
}

void UIMemoPage::handle_editor_key(const key_item *item)
{
    if (!textarea_) return;
    const memo::EditorAction action = memo::editor_action(item);
    if (action == memo::EditorAction::NONE) return;
    const bool had_status = !status_message_.empty();
    const bool discard_armed = status_error_ && status_message_ == kDiscardPrompt;
    if (action != memo::EditorAction::CLOSE && action != memo::EditorAction::SAVE) {
        status_message_.clear();
        status_error_ = false;
    }

    switch (action) {
    case memo::EditorAction::INSERT_TEXT: {
        const std::string text = launcher_ui::text_input::key_text(item->key_code, item);
        if (!text.empty()) lv_textarea_add_text(textarea_, text.c_str());
        break;
    }
    case memo::EditorAction::NEWLINE:
        lv_textarea_add_char(textarea_, '\n');
        break;
    case memo::EditorAction::BACKSPACE:
        lv_textarea_delete_char(textarea_);
        break;
    case memo::EditorAction::DELETE_FORWARD:
        lv_textarea_delete_char_forward(textarea_);
        break;
    case memo::EditorAction::LEFT:
        lv_textarea_cursor_left(textarea_);
        break;
    case memo::EditorAction::RIGHT:
        lv_textarea_cursor_right(textarea_);
        break;
    case memo::EditorAction::UP:
        lv_textarea_cursor_up(textarea_);
        break;
    case memo::EditorAction::DOWN:
        lv_textarea_cursor_down(textarea_);
        break;
    case memo::EditorAction::SAVE: {
        const bool ok = persist_editor();
        status_message_ = ok ? "Saved" : "Save failed";
        status_error_ = !ok;
        update_editor_hint();
        return;
    }
    case memo::EditorAction::CLOSE:
        if (discard_armed) {
            // Second Esc after a failed save: leave without saving.
            saved_text_ = lv_textarea_get_text(textarea_);
        }
        close_editor();
        return;
    case memo::EditorAction::NONE:
        return;
    }
    if (had_status) update_editor_hint();
}

// ==================== events ====================

void UIMemoPage::static_event_cb(lv_event_t *event) noexcept
{
    try {
        if (!event) return;
        auto *page = static_cast<UIMemoPage *>(lv_event_get_user_data(event));
        if (!page || lv_event_get_current_target(event) != page->root_screen_) return;
        const lv_event_code_t code = lv_event_get_code(event);
        if (code == LV_EVENT_DELETE && lv_event_get_target(event) == page->root_screen_) {
            if (page->view_state_ == ViewState::EDITOR && page->textarea_) page->persist_editor();
            page->restore_input_mode();
            return;
        }
        if (code != static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD)) return;
        const key_item *item = launcher_ui::events::keyboard_item(event);
        if (!item) return;
        if (page->view_state_ == ViewState::EDITOR)
            page->handle_editor_key(item);
        else
            page->handle_list_key(item);
    } catch (...) {
    }
}

void UIMemoPage::static_owned_delete_cb(lv_event_t *event) noexcept
{
    if (!event || lv_event_get_target(event) != lv_event_get_current_target(event)) return;
    auto *self = static_cast<UIMemoPage *>(lv_event_get_user_data(event));
    if (!self) return;
    lv_obj_t *deleted = static_cast<lv_obj_t *>(lv_event_get_target(event));
    if (deleted == self->textarea_) {
        // Last chance to keep the text when the screen goes away first.
        try {
            if (self->view_state_ == ViewState::EDITOR) self->persist_editor();
        } catch (...) {
        }
        self->textarea_ = nullptr;
    } else if (deleted == self->background_) {
        self->background_ = nullptr;
        self->list_container_ = nullptr;
        self->editor_container_ = nullptr;
        self->textarea_ = nullptr;
        self->editor_hint_ = nullptr;
    } else if (deleted == self->list_container_) {
        self->list_container_ = nullptr;
    } else if (deleted == self->editor_container_) {
        self->editor_container_ = nullptr;
        self->textarea_ = nullptr;
        self->editor_hint_ = nullptr;
    } else if (deleted == self->editor_hint_) {
        self->editor_hint_ = nullptr;
    }
}

void UIMemoPage::detach_callbacks()
{
    for (lv_obj_t *object : {textarea_, editor_hint_, editor_container_, list_container_, background_})
        if (object) lv_obj_remove_event_cb_with_user_data(object, static_owned_delete_cb, this);
    textarea_ = nullptr;
    editor_hint_ = nullptr;
    editor_container_ = nullptr;
    list_container_ = nullptr;
    background_ = nullptr;
}
