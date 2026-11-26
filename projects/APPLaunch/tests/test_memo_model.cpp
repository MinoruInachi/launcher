/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "../main/ui/model/memo_model.hpp"

#include "input_keys.h"
#include "keyboard_input.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

key_item key(uint32_t code, int state = KBD_KEY_PRESSED, const char *utf8 = "", uint32_t mods = 0,
             uint32_t semantic = 0)
{
    key_item item{};
    item.key_code = code;
    item.key_state = state;
    item.mods = mods;
    item.semantic_key = semantic;
    std::snprintf(item.utf8, sizeof(item.utf8), "%s", utf8);
    return item;
}

void test_filenames()
{
    assert(memo::is_memo_filename("memo-20261126-170501.txt"));
    assert(memo::is_memo_filename("memo-20261126-170501-2.txt"));
    assert(memo::is_memo_filename("memo-20261126-170501-999.txt"));
    assert(!memo::is_memo_filename("memo-20261126-170501-1.txt"));
    assert(!memo::is_memo_filename("memo-20261126-170501-02.txt"));
    assert(!memo::is_memo_filename("memo-20261126-170501-1000.txt"));
    assert(!memo::is_memo_filename("memo-20261126-170501.txt.tmp"));
    assert(!memo::is_memo_filename("memo-2026112-170501.txt"));
    assert(!memo::is_memo_filename("memo-20261126_170501.txt"));
    assert(!memo::is_memo_filename("note-20261126-170501.txt"));
    assert(!memo::is_memo_filename("../memo-20261126-170501.txt"));
    assert(!memo::is_memo_filename(""));

    std::tm local{};
    local.tm_year = 2026 - 1900;
    local.tm_mon = 10;
    local.tm_mday = 26;
    local.tm_hour = 17;
    local.tm_min = 5;
    local.tm_sec = 1;
    assert(memo::make_memo_filename(local, {}) == "memo-20261126-170501.txt");
    assert(memo::make_memo_filename(local, {"memo-20261126-170501.txt"}) ==
           "memo-20261126-170501-2.txt");
    assert(memo::make_memo_filename(
               local, {"memo-20261126-170501.txt", "memo-20261126-170501-2.txt"}) ==
           "memo-20261126-170501-3.txt");
    assert(memo::memo_timestamp_label("memo-20261126-170501-2.txt") == "2026-11-26 17:05");
    assert(memo::memo_timestamp_label("other.txt").empty());
}

void test_dir_list()
{
    const std::string listing =
        "F\tmemo-20261125-090000.txt\n"
        "D\tmemo-20261127-090000.txt\n"
        "F\tmemo-20261126-170501.txt\n"
        "F\tmemo-20261126-170501-2.txt\n"
        "F\tmemo-20261126-170501-10.txt\n"
        "F\tmemo-20261126-170501.txt.tmp\n"
        "F\tnotes.txt\n"
        "F\tmemo-20261124-000000.txt";
    const auto names = memo::parse_memo_dir_list(listing);
    const std::vector<std::string> expected = {
        "memo-20261126-170501-10.txt", "memo-20261126-170501-2.txt",
        "memo-20261126-170501.txt",    "memo-20261125-090000.txt",
        "memo-20261124-000000.txt",
    };
    assert(names == expected);
    assert(memo::parse_memo_dir_list("").empty());
}

void test_title_and_sanitize()
{
    assert(memo::memo_title("") == "");
    assert(memo::memo_title("\n  \n") == "");
    assert(memo::memo_title("\n  買い物リスト  \n牛乳") == "買い物リスト");
    assert(memo::memo_title("あいうえおかきくけこ", 5) == "あいうえお...");
    assert(memo::memo_title("abc", 3) == "abc");

    assert(memo::sanitize_memo_text("a\r\nb\rc\td") == "a\nb\nc d");
    assert(memo::sanitize_memo_text("x\x01y\x7fz") == "xyz");
    assert(memo::sanitize_memo_text("ok\xff!") == "ok\xEF\xBF\xBD!");
    assert(memo::sanitize_memo_text("日本語\n") == "日本語\n");
    const std::string long_text(memo::kMaxMemoCharacters + 10, 'a');
    assert(memo::sanitize_memo_text(long_text).size() == memo::kMaxMemoCharacters);
}

void test_list_actions()
{
    auto check = [](key_item item, memo::ListAction expected) {
        assert(memo::list_action(&item) == expected);
    };
    check(key(KEY_UP), memo::ListAction::UP);
    check(key(KEY_F, KBD_KEY_PRESSED, "f", 0, KEY_UP), memo::ListAction::UP);
    check(key(KEY_DOWN, KBD_KEY_REPEATED), memo::ListAction::DOWN);
    check(key(KEY_ENTER), memo::ListAction::OPEN);
    check(key(KEY_ENTER, KBD_KEY_REPEATED), memo::ListAction::NONE);
    check(key(KEY_ENTER, KBD_KEY_RELEASED), memo::ListAction::NONE);
    check(key(KEY_N, KBD_KEY_PRESSED, "n"), memo::ListAction::NEW);
    check(key(KEY_BACKSPACE), memo::ListAction::DELETE);
    check(key(KEY_ESC), memo::ListAction::EXIT);
    check(key(KEY_ESC, KBD_KEY_RELEASED), memo::ListAction::NONE);
    assert(memo::list_action(nullptr) == memo::ListAction::NONE);
}

void test_editor_actions()
{
    auto check = [](key_item item, memo::EditorAction expected) {
        assert(memo::editor_action(&item) == expected);
    };
    check(key(KEY_A, KBD_KEY_PRESSED, "a"), memo::EditorAction::INSERT_TEXT);
    check(key(KEY_A, KBD_KEY_REPEATED, "a"), memo::EditorAction::INSERT_TEXT);
    check(key(KEY_A, KBD_KEY_RELEASED, "a"), memo::EditorAction::NONE);
    // IME commit: no physical key, one code point, press only.
    check(key(0, KBD_KEY_PRESSED, "漢"), memo::EditorAction::INSERT_TEXT);
    check(key(KEY_ENTER, KBD_KEY_PRESSED, "\r"), memo::EditorAction::NEWLINE);
    check(key(KEY_BACKSPACE, KBD_KEY_REPEATED), memo::EditorAction::BACKSPACE);
    check(key(KEY_DELETE), memo::EditorAction::DELETE_FORWARD);
    check(key(KEY_LEFT), memo::EditorAction::LEFT);
    check(key(KEY_Z, KBD_KEY_PRESSED, "z", KBD_MOD_FN), memo::EditorAction::LEFT);
    check(key(KEY_F, KBD_KEY_PRESSED, "f", KBD_MOD_FN), memo::EditorAction::UP);
    check(key(KEY_TAB), memo::EditorAction::SAVE);
    check(key(KEY_S, KBD_KEY_PRESSED, "s", KBD_MOD_CTRL), memo::EditorAction::SAVE);
    check(key(KEY_SPACE, KBD_KEY_PRESSED, " ", KBD_MOD_CTRL), memo::EditorAction::NONE);
    check(key(KEY_ESC), memo::EditorAction::CLOSE);
    check(key(KEY_ESC, KBD_KEY_REPEATED), memo::EditorAction::NONE);
    check(key(KEY_LEFTSHIFT), memo::EditorAction::NONE);
}

void test_list_model()
{
    memo::MemoListModel model;
    assert(model.row_count() == 1 && model.new_memo_selected());
    assert(!model.move_up());
    assert(!model.request_delete() && !model.delete_armed());

    model.set_files({"memo-20261126-170501.txt", "memo-20261125-090000.txt"});
    assert(model.row_count() == 3);
    assert(model.move_down() && model.selected_file() == "memo-20261126-170501.txt");
    assert(model.move_down() && model.selected_file() == "memo-20261125-090000.txt");
    assert(!model.move_down());

    assert(!model.request_delete() && model.delete_armed());
    assert(model.request_delete() && !model.delete_armed());
    assert(!model.request_delete());
    model.move_up();
    assert(!model.delete_armed());

    // Selection follows the file when the list is reloaded.
    model.select_file("memo-20261125-090000.txt");
    model.set_files({"memo-20261127-000000.txt", "memo-20261126-170501.txt",
                     "memo-20261125-090000.txt"});
    assert(model.selected_file() == "memo-20261125-090000.txt");
    // A deleted selection keeps the row, clamped to the list.
    model.set_files({"memo-20261127-000000.txt", "memo-20261126-170501.txt"});
    assert(model.selected_row() == 2);
    model.select_file("missing");
    assert(model.new_memo_selected());
}

} // namespace

int main()
{
    test_filenames();
    test_dir_list();
    test_title_and_sanitize();
    test_list_actions();
    test_editor_actions();
    test_list_model();
    return 0;
}
