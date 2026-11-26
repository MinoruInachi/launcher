/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "memo_model.hpp"

#include "../keyboard_text_input.hpp"
#include "input_keys.h"
#include "keyboard_input.h"

#include <algorithm>
#include <cstdio>

namespace memo {
namespace {

constexpr const char kPrefix[] = "memo-";
constexpr const char kSuffix[] = ".txt";
constexpr std::size_t kPrefixLength = sizeof(kPrefix) - 1;
constexpr std::size_t kSuffixLength = sizeof(kSuffix) - 1;
constexpr std::size_t kStampLength = 15; // YYYYMMDD-HHMMSS

bool all_digits(const std::string &text, std::size_t begin, std::size_t count)
{
    if (begin + count > text.size()) return false;
    for (std::size_t index = begin; index < begin + count; ++index)
        if (text[index] < '0' || text[index] > '9') return false;
    return true;
}

// Decodes one UTF-8 sequence. Returns its length, or 0 when invalid.
std::size_t decode_utf8(const std::string &text, std::size_t at, uint32_t &codepoint)
{
    const auto byte = [&](std::size_t index) { return static_cast<unsigned char>(text[index]); };
    const unsigned char lead = byte(at);
    std::size_t length = 0;
    uint32_t minimum = 0;
    if (lead < 0x80) {
        codepoint = lead;
        return 1;
    } else if ((lead & 0xe0) == 0xc0) {
        length = 2;
        codepoint = lead & 0x1f;
        minimum = 0x80;
    } else if ((lead & 0xf0) == 0xe0) {
        length = 3;
        codepoint = lead & 0x0f;
        minimum = 0x800;
    } else if ((lead & 0xf8) == 0xf0) {
        length = 4;
        codepoint = lead & 0x07;
        minimum = 0x10000;
    } else {
        return 0;
    }
    if (at + length > text.size()) return 0;
    for (std::size_t index = 1; index < length; ++index) {
        if ((byte(at + index) & 0xc0) != 0x80) return 0;
        codepoint = (codepoint << 6) | (byte(at + index) & 0x3f);
    }
    if (codepoint < minimum || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff))
        return 0;
    return length;
}

bool is_space(char character)
{
    return character == ' ' || character == '\t' || character == '\r';
}

uint32_t navigation_key(const key_item *item)
{
    return item->semantic_key ? item->semantic_key : item->key_code;
}

bool fn_arrow(const key_item *item, EditorAction &action)
{
    if ((item->mods & KBD_MOD_FN) == 0) return false;
    switch (item->key_code) {
    case KEY_F: action = EditorAction::UP; return true;
    case KEY_X: action = EditorAction::DOWN; return true;
    case KEY_Z: action = EditorAction::LEFT; return true;
    case KEY_C: action = EditorAction::RIGHT; return true;
    default: return false;
    }
}

} // namespace

bool is_memo_filename(const std::string &name)
{
    if (name.size() < kPrefixLength + kStampLength + kSuffixLength) return false;
    if (name.compare(0, kPrefixLength, kPrefix) != 0) return false;
    if (name.compare(name.size() - kSuffixLength, kSuffixLength, kSuffix) != 0) return false;
    if (!all_digits(name, kPrefixLength, 8) || name[kPrefixLength + 8] != '-' ||
        !all_digits(name, kPrefixLength + 9, 6))
        return false;
    const std::size_t rest = kPrefixLength + kStampLength;
    const std::size_t rest_length = name.size() - kSuffixLength - rest;
    if (rest_length == 0) return true;
    // "-N" with N = 2..999, no leading zero.
    if (rest_length < 2 || rest_length > 4 || name[rest] != '-') return false;
    if (!all_digits(name, rest + 1, rest_length - 1) || name[rest + 1] == '0') return false;
    return !(rest_length == 2 && name[rest + 1] == '1');
}

std::string make_memo_filename(const std::tm &local, const std::vector<std::string> &existing)
{
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                  (local.tm_year + 1900) % 10000, (local.tm_mon + 1) % 100, local.tm_mday % 100,
                  local.tm_hour % 100, local.tm_min % 100, local.tm_sec % 100);
    const std::string base = std::string(kPrefix) + stamp;
    const auto taken = [&](const std::string &name) {
        return std::find(existing.begin(), existing.end(), name) != existing.end();
    };
    std::string name = base + kSuffix;
    if (!taken(name)) return name;
    for (int suffix = 2; suffix <= 999; ++suffix) {
        name = base + "-" + std::to_string(suffix) + kSuffix;
        if (!taken(name)) return name;
    }
    return "";
}

std::vector<std::string> parse_memo_dir_list(const std::string &dir_list)
{
    std::vector<std::string> names;
    std::size_t start = 0;
    while (start < dir_list.size()) {
        std::size_t end = dir_list.find('\n', start);
        if (end == std::string::npos) end = dir_list.size();
        const std::string line = dir_list.substr(start, end - start);
        start = end + 1;
        if (line.size() < 3 || line[0] != 'F' || line[1] != '\t') continue;
        // Memo names never contain '%', so no field decoding is needed.
        const std::string name = line.substr(2);
        if (is_memo_filename(name)) names.push_back(name);
    }
    // The timestamped names sort chronologically; -N variants follow their base.
    std::sort(names.begin(), names.end(), [](const std::string &left, const std::string &right) {
        const std::string left_stamp = left.substr(kPrefixLength, kStampLength);
        const std::string right_stamp = right.substr(kPrefixLength, kStampLength);
        if (left_stamp != right_stamp) return left_stamp > right_stamp;
        if (left.size() != right.size()) return left.size() > right.size();
        return left > right;
    });
    if (names.size() > kMaxListedMemos) names.resize(kMaxListedMemos);
    return names;
}

std::string memo_title(const std::string &text, std::size_t max_codepoints)
{
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::size_t first = start;
        std::size_t last = end;
        while (first < last && is_space(text[first])) ++first;
        while (last > first && is_space(text[last - 1])) --last;
        if (first < last) {
            std::string line = text.substr(first, last - first);
            std::size_t offset = 0;
            std::size_t count = 0;
            while (offset < line.size() && count < max_codepoints) {
                uint32_t codepoint = 0;
                const std::size_t length = decode_utf8(line, offset, codepoint);
                offset += length ? length : 1;
                ++count;
            }
            if (offset < line.size()) return line.substr(0, offset) + "...";
            return line;
        }
        if (end == text.size()) break;
        start = end + 1;
    }
    return "";
}

std::string memo_timestamp_label(const std::string &name)
{
    if (!is_memo_filename(name)) return "";
    const std::string stamp = name.substr(kPrefixLength, kStampLength);
    return stamp.substr(0, 4) + "-" + stamp.substr(4, 2) + "-" + stamp.substr(6, 2) + " " +
           stamp.substr(9, 2) + ":" + stamp.substr(11, 2);
}

std::string sanitize_memo_text(const std::string &text)
{
    std::string output;
    output.reserve(std::min(text.size(), kMaxMemoBytes));
    std::size_t characters = 0;
    std::size_t offset = 0;
    while (offset < text.size() && characters < kMaxMemoCharacters) {
        uint32_t codepoint = 0;
        const std::size_t length = decode_utf8(text, offset, codepoint);
        if (length == 0) {
            output += "\xEF\xBF\xBD"; // U+FFFD
            ++offset;
            ++characters;
            continue;
        }
        if (codepoint == '\r') {
            output.push_back('\n');
            ++characters;
            offset += (offset + 1 < text.size() && text[offset + 1] == '\n') ? 2 : 1;
            continue;
        }
        if (codepoint == '\t') {
            output.push_back(' ');
        } else if (codepoint == '\n' ||
                   !(codepoint < 0x20 || codepoint == 0x7f ||
                     (codepoint >= 0x80 && codepoint < 0xa0))) {
            output.append(text, offset, length);
        } else {
            offset += length;
            continue;
        }
        offset += length;
        ++characters;
    }
    return output;
}

ListAction list_action(const key_item *item)
{
    if (!item || item->key_state == KBD_KEY_RELEASED) return ListAction::NONE;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    switch (navigation_key(item)) {
    case KEY_UP: return ListAction::UP;
    case KEY_DOWN: return ListAction::DOWN;
    default: break;
    }
    if (repeat) return ListAction::NONE;
    switch (navigation_key(item)) {
    case KEY_ENTER:
    case KEY_KPENTER: return ListAction::OPEN;
    case KEY_N: return ListAction::NEW;
    case KEY_BACKSPACE:
    case KEY_DELETE:
    case KEY_D: return ListAction::DELETE;
    case KEY_ESC: return ListAction::EXIT;
    default: return ListAction::NONE;
    }
}

EditorAction editor_action(const key_item *item)
{
    if (!item || item->key_state == KBD_KEY_RELEASED) return EditorAction::NONE;
    const bool repeat = item->key_state == KBD_KEY_REPEATED;
    EditorAction arrow = EditorAction::NONE;
    if (fn_arrow(item, arrow)) return arrow;
    switch (item->key_code) {
    case KEY_ESC: return repeat ? EditorAction::NONE : EditorAction::CLOSE;
    case KEY_TAB: return repeat ? EditorAction::NONE : EditorAction::SAVE;
    case KEY_ENTER:
    case KEY_KPENTER: return EditorAction::NEWLINE;
    case KEY_BACKSPACE: return EditorAction::BACKSPACE;
    case KEY_DELETE: return EditorAction::DELETE_FORWARD;
    case KEY_LEFT: return EditorAction::LEFT;
    case KEY_RIGHT: return EditorAction::RIGHT;
    case KEY_UP: return EditorAction::UP;
    case KEY_DOWN: return EditorAction::DOWN;
    default: break;
    }
    if (item->mods & (KBD_MOD_CTRL | KBD_MOD_ALT | KBD_MOD_LOGO)) {
        if ((item->mods & KBD_MOD_CTRL) && item->key_code == KEY_S && !repeat)
            return EditorAction::SAVE;
        return EditorAction::NONE;
    }
    return launcher_ui::text_input::key_text(item->key_code, item).empty()
               ? EditorAction::NONE
               : EditorAction::INSERT_TEXT;
}

void MemoListModel::set_files(std::vector<std::string> files)
{
    const std::string previous = selected_file();
    const std::size_t previous_row = selected_;
    files_ = std::move(files);
    delete_armed_ = false;
    const auto found = std::find(files_.begin(), files_.end(), previous);
    if (!previous.empty() && found != files_.end())
        selected_ = static_cast<std::size_t>(found - files_.begin()) + 1;
    else
        // The selected memo is gone (deleted): stay on the same row.
        selected_ = std::min(previous_row, row_count() - 1);
}

std::string MemoListModel::selected_file() const
{
    return selected_ == 0 || selected_ > files_.size() ? std::string() : files_[selected_ - 1];
}

bool MemoListModel::move_up()
{
    delete_armed_ = false;
    if (selected_ == 0) return false;
    --selected_;
    return true;
}

bool MemoListModel::move_down()
{
    delete_armed_ = false;
    if (selected_ + 1 >= row_count()) return false;
    ++selected_;
    return true;
}

void MemoListModel::select_file(const std::string &name)
{
    delete_armed_ = false;
    const auto found = std::find(files_.begin(), files_.end(), name);
    selected_ = found == files_.end() ? 0 : static_cast<std::size_t>(found - files_.begin()) + 1;
}

bool MemoListModel::request_delete()
{
    if (new_memo_selected()) {
        delete_armed_ = false;
        return false;
    }
    if (!delete_armed_) {
        delete_armed_ = true;
        return false;
    }
    delete_armed_ = false;
    return true;
}

} // namespace memo
