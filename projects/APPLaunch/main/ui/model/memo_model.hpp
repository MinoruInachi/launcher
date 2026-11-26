/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

// Pure logic for the Memo page: file naming, list parsing, titles, key
// mapping and list selection. No LVGL or filesystem access lives here.

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

struct key_item;

namespace memo {

constexpr std::size_t kMaxMemoBytes = 64 * 1024;
constexpr std::size_t kMaxMemoCharacters = 4000;
constexpr std::size_t kMaxListedMemos = 200;
constexpr std::size_t kTitleCodepoints = 40;

// memo-YYYYMMDD-HHMMSS.txt or memo-YYYYMMDD-HHMMSS-N.txt (N = 2..999).
bool is_memo_filename(const std::string &name);

// Picks memo-YYYYMMDD-HHMMSS.txt for `local`, adding -2, -3, ... when the
// name is already taken. Returns "" if no free name exists.
std::string make_memo_filename(const std::tm &local, const std::vector<std::string> &existing);

// Memo file names from a filesystem DirList reply ("F\tname\n" lines),
// newest first, at most kMaxListedMemos.
std::vector<std::string> parse_memo_dir_list(const std::string &dir_list);

// First non-blank line, trimmed and cut to max_codepoints (with "..." when
// shortened). Returns "" for a blank memo.
std::string memo_title(const std::string &text, std::size_t max_codepoints = kTitleCodepoints);

// "2026-11-26 17:05" from a memo file name, "" if the name does not parse.
std::string memo_timestamp_label(const std::string &name);

// Replaces invalid UTF-8 and control characters other than '\n' so a file
// edited elsewhere cannot break the text area. CR LF becomes LF, tab a space.
std::string sanitize_memo_text(const std::string &text);

enum class ListAction { NONE, UP, DOWN, OPEN, NEW, DELETE, EXIT };
enum class EditorAction {
    NONE,
    INSERT_TEXT,
    NEWLINE,
    BACKSPACE,
    DELETE_FORWARD,
    LEFT,
    RIGHT,
    UP,
    DOWN,
    SAVE,
    CLOSE,
};

// Both views act on key press and auto-repeat only: IME commits arrive as
// presses without a release, and acting on presses keeps the release of a
// key that switched views from triggering the next view.
ListAction list_action(const key_item *item);
EditorAction editor_action(const key_item *item);

class MemoListModel
{
public:
    // Row 0 is "New memo"; rows 1.. are the memo files.
    void set_files(std::vector<std::string> files);
    const std::vector<std::string> &files() const { return files_; }
    std::size_t row_count() const { return files_.size() + 1; }
    std::size_t selected_row() const { return selected_; }
    bool new_memo_selected() const { return selected_ == 0; }
    // File name of the selected memo, "" for the "New memo" row.
    std::string selected_file() const;

    bool move_up();
    bool move_down();
    void select_file(const std::string &name);

    // First DELETE arms the confirmation, the second one returns true.
    // Any other action disarms it.
    bool request_delete();
    void cancel_delete() { delete_armed_ = false; }
    bool delete_armed() const { return delete_armed_; }

private:
    std::vector<std::string> files_;
    std::size_t selected_ = 0;
    bool delete_armed_ = false;
};

} // namespace memo
