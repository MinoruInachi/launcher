/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once
#include "../launcher_ui_app_page.hpp"
#include "../model/memo_model.hpp"
#include "keyboard_input.h"

#include <map>
#include <string>

// ============================================================
//  Memo  UIMemoPage
//  Screen: 320 x 170  (top bar 20px, ui_APP_Container 320x150)
//
//  Views:
//    LIST   -- "New memo" + saved memos, newest first (navigation keys)
//    EDITOR -- multi-line text area; the keyboard runs in text mode so
//              the input method (fcitx5) can attach for Japanese input
//
//  Memos are UTF-8 text files in ~/Documents/Memo/memo-YYYYMMDD-HHMMSS.txt.
//  Leaving the editor (Esc, Esc long-press home, page teardown) saves.
// ============================================================

class UIMemoPage : public AppPage
{
    enum class ViewState { LIST, EDITOR };

public:
    UIMemoPage();
    ~UIMemoPage() override;

private:
    // ==================== state ====================
    memo::MemoListModel list_;
    std::map<std::string, std::string> title_cache_;
    std::string memo_dir_;
    ViewState view_state_ = ViewState::LIST;
    std::string editing_file_; // "" while a new memo has not been saved yet
    std::string saved_text_;
    std::string status_message_;
    bool status_error_ = false;
    std::size_t list_top_ = 0;

    const lv_font_t *text_font_ = nullptr;
    const lv_font_t *hint_font_ = nullptr;
    lv_obj_t *background_ = nullptr;
    lv_obj_t *list_container_ = nullptr;
    lv_obj_t *editor_container_ = nullptr;
    lv_obj_t *textarea_ = nullptr;
    lv_obj_t *editor_hint_ = nullptr;

    bool input_context_saved_ = false;
    cp0_keyboard_input_context_t previous_input_context_ = KBD_INPUT_CONTEXT_NAVIGATION;
    int previous_keypad_intercept_ = 0;

    static constexpr int ROW_H = 31;
    static constexpr int VISIBLE_ROWS = 4;
    static constexpr int HINT_Y = 128;

    // ==================== storage ====================
    bool ensure_memo_dir();
    void reload_files();
    std::string memo_path(const std::string &name) const;
    int read_memo(const std::string &name, std::string &text) const;
    const std::string &title_for(const std::string &name);
    bool write_memo(const std::string &name, const std::string &text);
    bool remove_memo(const std::string &name);

    // ==================== views ====================
    void create_ui();
    void build_list();
    void open_editor(const std::string &name);
    bool close_editor();
    void update_editor_hint();
    void set_status(std::string message, bool error);
    bool persist_editor();
    void delete_selected();

    // ==================== input ====================
    void save_input_mode();
    void set_input_mode(cp0_keyboard_input_context_t context);
    void restore_input_mode();
    void handle_list_key(const key_item *item);
    void handle_editor_key(const key_item *item);

    static void static_event_cb(lv_event_t *event) noexcept;
    static void static_owned_delete_cb(lv_event_t *event) noexcept;
    void detach_callbacks();
};
