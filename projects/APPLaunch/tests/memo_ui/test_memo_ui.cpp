/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

// Drives the real Memo page (UIMemoPage on the real AppPage shell and LVGL)
// with LV_EVENT_KEYBOARD items, including IME-style commits, and checks the
// memo files written through the real filesystem API dispatcher.

#define APP_PAGE_IMPLEMENTATION_UNIT
#include "../../main/ui/page_app/ui_app_memo.hpp"

#include "cp0_filesystem_api.hpp"
#include "cp0_resource_path_policy.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

decltype(cp0_signal_filesystem_api) cp0_signal_filesystem_api;
volatile uint32_t LV_EVENT_KEYBOARD = 0;
uint32_t lv_c_event[2 * CP0_C_EVENT_END];
static cp0_keyboard_input_context_t input_context = KBD_INPUT_CONTEXT_NAVIGATION;
static int intercept = 0;
extern "C" void cp0_keyboard_set_lvgl_keypad_intercept(int value) { intercept = value; }
extern "C" int cp0_keyboard_get_lvgl_keypad_intercept() { return intercept; }
extern "C" void cp0_keyboard_set_input_context(cp0_keyboard_input_context_t value) { input_context = value; }
extern "C" cp0_keyboard_input_context_t cp0_keyboard_get_input_context() { return input_context; }
extern "C" void cp0_time_str(char *out, int size)
{
    std::snprintf(out, static_cast<size_t>(size), "12:00");
}
extern "C" int cp0_wifi_status_read(cp0_wifi_status_t *status)
{
    *status = {};
    return -1;
}
extern "C" cp0_battery_info_t cp0_battery_read(void) { return {}; }

namespace {

std::string memo_dir;

void snapshot(const char *name)
{
    const char *directory = std::getenv("MEMO_UI_SNAPSHOT_DIR");
    if (!directory) return;
    lv_obj_update_layout(lv_screen_active());
    auto *buffer = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    assert(buffer);
    auto *file = std::fopen((std::string(directory) + "/" + name + ".ppm").c_str(), "wb");
    assert(file);
    std::fprintf(file, "P6\n%u %u\n255\n", buffer->header.w, buffer->header.h);
    for (unsigned y = 0; y < buffer->header.h; ++y) {
        const auto *row = buffer->data + y * buffer->header.stride;
        for (unsigned x = 0; x < buffer->header.w; ++x) {
            const unsigned char rgb[] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
    lv_draw_buf_destroy(buffer);
}

void send(uint32_t code, int state, const char *utf8 = "", uint32_t mods = 0)
{
    key_item item{};
    item.key_code = code;
    item.key_state = state;
    item.mods = mods;
    item.input_context = input_context;
    item.semantic_key = code;
    std::snprintf(item.utf8, sizeof(item.utf8), "%s", utf8);
    lv_obj_send_event(lv_screen_active(), static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD), &item);
}

void tap(uint32_t code, const char *utf8 = "", uint32_t mods = 0)
{
    send(code, KBD_KEY_PRESSED, utf8, mods);
    send(code, KBD_KEY_RELEASED, utf8, mods);
}

// What the fcitx5 bridge delivers for a commit: one press per code point,
// key_code 0, no release.
void ime_commit(const char *utf8)
{
    send(0, KBD_KEY_PRESSED, utf8);
}

std::vector<std::string> memo_files()
{
    std::vector<std::string> names;
    if (DIR *directory = opendir(memo_dir.c_str())) {
        while (dirent *entry = readdir(directory))
            if (entry->d_name[0] != '.') names.push_back(entry->d_name);
        closedir(directory);
    }
    return names;
}

std::string read_file(const std::string &path)
{
    std::string text;
    if (FILE *file = std::fopen(path.c_str(), "rb")) {
        char buffer[4096];
        size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, count);
        std::fclose(file);
    }
    return text;
}

void remove_tree(const std::string &path)
{
    if (DIR *directory = opendir(path.c_str())) {
        while (dirent *entry = readdir(directory)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string child = path + "/" + name;
            struct stat status{};
            if (lstat(child.c_str(), &status) == 0 && S_ISDIR(status.st_mode))
                remove_tree(child);
            else
                unlink(child.c_str());
        }
        closedir(directory);
    }
    rmdir(path.c_str());
}

std::string editor_text(UIMemoPage &page)
{
    return page.textarea_ ? lv_textarea_get_text(page.textarea_) : "";
}

} // namespace

int main(int argc, char **argv)
{
    assert(argc == 2); // share/font directory
    const std::string font_dir = argv[1];
    char home_template[] = "/tmp/memo-ui-home-XXXXXX";
    const char *home = mkdtemp(home_template);
    assert(home);
    setenv("HOME", home, 1);
    memo_dir = std::string(home) + "/Documents/Memo";

    cp0_filesystem_api::Operations operations;
    // Resolve like the device: only names the resource policy classifies as
    // fonts map into share/font (a .ttc once fell through and lost CJK glyphs).
    operations.resolve_path = [&](const std::string &name) {
        return cp0::filesystem::classify_resource(name) == cp0::filesystem::ResourceKind::font
                   ? font_dir + "/" + name
                   : std::string();
    };
    operations.ensure_directory = [](const std::string &path) {
        struct stat status{};
        return (mkdir(path.c_str(), 0755) == 0 || errno == EEXIST) && stat(path.c_str(), &status) == 0 &&
                       S_ISDIR(status.st_mode)
                   ? 0
                   : -1;
    };
    cp0_signal_filesystem_api.append([&](std::list<std::string> args, auto done) {
        cp0_filesystem_api::dispatch(args, operations, std::move(done));
    });

    lv_init();
    lv_display_create(320, 170);
    LV_EVENT_KEYBOARD = lv_event_register_id();
    input_context = KBD_INPUT_CONTEXT_NAVIGATION;

    {
        UIMemoPage page;
        lv_screen_load(page.screen());
        // The memo text uses the bundled CJK font, not the Latin-only fallback.
        lv_font_glyph_dsc_t glyph{};
        assert(lv_font_get_glyph_dsc(page.text_font_, &glyph, 0x725B /* 牛 */, 0));
        assert(lv_font_get_glyph_dsc(page.hint_font_, &glyph, 0x3042 /* あ */, 0));
        // List view: navigation keys, LVGL keypad path suppressed.
        assert(input_context == KBD_INPUT_CONTEXT_NAVIGATION && intercept == 1);
        assert(page.list_.row_count() == 1 && page.list_.new_memo_selected());
        snapshot("memo-01-empty-list");

        // Enter on "New memo" opens the editor in text mode.
        tap(KEY_ENTER, "\r");
        assert(page.view_state_ == UIMemoPage::ViewState::EDITOR);
        assert(input_context == KBD_INPUT_CONTEXT_TEXT);
        assert(editor_text(page).empty()); // the Enter release did not type

        tap(KEY_S, "S", KBD_MOD_SHIFT);
        tap(KEY_H, "h");
        tap(KEY_O, "o");
        tap(KEY_P, "p");
        tap(KEY_ENTER, "\r");
        ime_commit("牛");
        ime_commit("乳");
        tap(KEY_ENTER, "\r");
        ime_commit("卵");
        assert(editor_text(page) == "Shop\n牛乳\n卵");
        tap(KEY_BACKSPACE, "\x7f");
        assert(editor_text(page) == "Shop\n牛乳\n");
        ime_commit("パ");
        ime_commit("ン");
        // Ctrl combinations (IME toggle when fcitx5 is down) never type.
        tap(KEY_SPACE, " ", KBD_MOD_CTRL);
        assert(editor_text(page) == "Shop\n牛乳\nパン");
        snapshot("memo-02-editor");

        // Esc saves the new memo and returns to the list with it selected.
        tap(KEY_ESC, "\x1b");
        assert(page.view_state_ == UIMemoPage::ViewState::LIST);
        assert(input_context == KBD_INPUT_CONTEXT_NAVIGATION);
        auto files = memo_files();
        assert(files.size() == 1 && memo::is_memo_filename(files[0]));
        const std::string first = files[0];
        assert(read_file(memo_dir + "/" + first) == "Shop\n牛乳\nパン");
        assert(page.list_.row_count() == 2 && page.list_.selected_file() == first);
        assert(page.title_for(first) == "Shop");
        snapshot("memo-03-list");

        // Reopen, edit, save with Tab, leave.
        tap(KEY_ENTER, "\r");
        assert(editor_text(page) == "Shop\n牛乳\nパン");
        ime_commit("！");
        tap(KEY_TAB);
        assert(page.status_message_ == "Saved" && !page.status_error_);
        assert(read_file(memo_dir + "/" + first) == "Shop\n牛乳\nパン！");
        // Arrow keys move the cursor: two lines up lands on the first line.
        tap(KEY_UP);
        tap(KEY_UP);
        tap(KEY_LEFT);
        ime_commit("買");
        tap(KEY_ESC, "\x1b");
        const std::string edited = read_file(memo_dir + "/" + first);
        assert(edited.substr(0, edited.find('\n')).find("買") != std::string::npos);
        assert(edited.size() == std::string("Shop\n牛乳\nパン！買").size());

        // A new memo left blank creates no file.
        tap(KEY_UP);
        assert(page.list_.new_memo_selected());
        tap(KEY_N, "n");
        assert(page.view_state_ == UIMemoPage::ViewState::EDITOR);
        tap(KEY_SPACE, " ");
        tap(KEY_ESC, "\x1b");
        assert(memo_files().size() == 1);

        // Delete needs two presses; another key disarms it.
        tap(KEY_DOWN);
        assert(page.list_.selected_file() == first);
        tap(KEY_BACKSPACE, "\x7f");
        assert(page.list_.delete_armed() && memo_files().size() == 1);
        snapshot("memo-04-delete-confirm");
        tap(KEY_DOWN);
        assert(!page.list_.delete_armed());
        tap(KEY_BACKSPACE, "\x7f");
        tap(KEY_BACKSPACE, "\x7f");
        assert(memo_files().empty() && page.list_.row_count() == 1);

        // Clearing a saved memo removes its file.
        tap(KEY_ENTER, "\r");
        ime_commit("消");
        tap(KEY_ESC, "\x1b");
        assert(memo_files().size() == 1);
        tap(KEY_ENTER, "\r");
        tap(KEY_BACKSPACE, "\x7f");
        tap(KEY_ESC, "\x1b");
        assert(memo_files().empty());

        // Leaving the page mid-edit (Esc long-press home) still saves.
        tap(KEY_ENTER, "\r");
        ime_commit("途");
        ime_commit("中");
        assert(input_context == KBD_INPUT_CONTEXT_TEXT);
        lv_screen_load(lv_obj_create(nullptr));
    }
    // The page restored the keyboard state it found.
    assert(input_context == KBD_INPUT_CONTEXT_NAVIGATION && intercept == 0);
    auto files = memo_files();
    assert(files.size() == 1 && read_file(memo_dir + "/" + files[0]) == "途中");

    // A fresh page lists the saved memo.
    {
        UIMemoPage page;
        lv_screen_load(page.screen());
        assert(page.list_.row_count() == 2);
        assert(page.title_for(files[0]) == "途中");
    }

    remove_tree(home);
    std::puts("memo ui tests passed");
    return 0;
}
