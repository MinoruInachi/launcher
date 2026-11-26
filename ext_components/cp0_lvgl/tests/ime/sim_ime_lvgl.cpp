/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* End-to-end check of the IME bridge on a headless 320x170 LVGL display:
 * the real keypad path (cp0_keyboard_lvgl_input.c + keyboard queue) feeds the
 * real bridge, which talks to mock_fcitx5_server over a private session bus.
 * A text "page" appends key_item.utf8 exactly like launcher pages do.
 * Screenshots are written as PNG for visual review.
 *
 *   dbus-run-session -- sim_ime_lvgl <mock_server> <font.ttc> <out_dir> */

#include "cp0_ime.h"
#include "input_keys.h"
#include "keyboard_input.h"
#include "lvgl/lvgl.h"

extern "C" {
#include "../../src/cp0_keyboard_queue.h"
#include "../../src/cp0/cp0_keyboard_lvgl_input.h"
}

#include <gio/gio.h>
#include <png.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

/* ---- globals normally provided by the device keyboard thread ---- */
struct keyboard_queue_t keyboard_queue;
pthread_mutex_t keyboard_mutex = PTHREAD_MUTEX_INITIALIZER;
volatile int LVGL_RUN_FLAGE = 1;
volatile uint32_t LV_EVENT_KEYBOARD;
static cp0_keyboard_input_context_t input_context = KBD_INPUT_CONTEXT_NAVIGATION;
extern "C" void cp0_keyboard_set_input_context(cp0_keyboard_input_context_t context)
{
    input_context = context;
}
extern "C" cp0_keyboard_input_context_t cp0_keyboard_get_input_context(void)
{
    return input_context;
}
extern "C" const char *kbd_state_name(int state)
{
    return state == KBD_KEY_PRESSED ? "pressed" : state == KBD_KEY_RELEASED ? "released" : "repeat";
}

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                 \
            std::exit(1);                                                        \
        }                                                                        \
    } while (0)

namespace {

constexpr int kWidth = 320;
constexpr int kHeight = 170;
uint32_t framebuffer[kWidth * kHeight];
uint32_t draw_buffer[kWidth * kHeight];
std::string field_text;
lv_obj_t *field_label;
std::string out_dir;

uint32_t tick_ms()
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint32_t>(now.tv_sec * 1000u + now.tv_nsec / 1000000u);
}

void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    const int width = lv_area_get_width(area);
    for (int y = area->y1; y <= area->y2; ++y)
        std::memcpy(&framebuffer[y * kWidth + area->x1],
                    pixels + (y - area->y1) * width * 4, width * 4);
    lv_display_flush_ready(display);
}

void run_for(int ms)
{
    const uint32_t end = tick_ms() + ms;
    while (static_cast<int32_t>(tick_ms() - end) < 0) {
        lv_timer_handler();
        g_usleep(2000);
    }
}

void screenshot(const char *name)
{
    run_for(120);
    lv_refr_now(nullptr);
    const std::string path = out_dir + "/" + name + ".png";
    FILE *file = std::fopen(path.c_str(), "wb");
    CHECK(file);
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
    png_infop info = png_create_info_struct(png);
    png_init_io(png, file);
    const int scale = 3; /* upscale for readability */
    png_set_IHDR(png, info, kWidth * scale, kHeight * scale, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);
    std::string row(kWidth * scale * 3, '\0');
    for (int y = 0; y < kHeight * scale; ++y) {
        for (int x = 0; x < kWidth * scale; ++x) {
            const uint32_t pixel = framebuffer[(y / scale) * kWidth + x / scale];
            row[x * 3 + 0] = static_cast<char>((pixel >> 16) & 0xff);
            row[x * 3 + 1] = static_cast<char>((pixel >> 8) & 0xff);
            row[x * 3 + 2] = static_cast<char>(pixel & 0xff);
        }
        png_write_row(png, reinterpret_cast<png_const_bytep>(row.data()));
    }
    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    std::fclose(file);
    std::printf("screenshot %s\n", path.c_str());
}

/* ---- a minimal text page, written like the launcher's input pages ---- */

void page_key_cb(lv_event_t *event)
{
    auto *item = static_cast<key_item *>(lv_event_get_param(event));
    if (!item || item->key_state == KBD_KEY_RELEASED) return;
    if (item->key_code == KEY_BACKSPACE) {
        while (!field_text.empty() && (static_cast<unsigned char>(field_text.back()) & 0xc0) == 0x80)
            field_text.pop_back();
        if (!field_text.empty()) field_text.pop_back();
    } else if (item->utf8[0] && static_cast<unsigned char>(item->utf8[0]) >= 0x20 &&
               item->utf8[0] != 0x7f) {
        field_text += item->utf8;
    }
    lv_label_set_text(field_label, (field_text + "|").c_str());
}

void build_page(const lv_font_t *font)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x000000), 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8fb4ff), 0);
    lv_label_set_text(title, "Bluetooth name");
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 8, 8);

    lv_obj_t *box = lv_obj_create(screen);
    lv_obj_set_size(box, 304, 40);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, 32);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x202020), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x606060), 0);
    lv_obj_set_style_pad_all(box, 6, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    field_label = lv_label_create(box);
    lv_obj_set_style_text_font(field_label, font, 0);
    lv_obj_set_style_text_color(field_label, lv_color_hex(0xffffff), 0);
    lv_label_set_text(field_label, "|");

    lv_obj_add_event_cb(screen, page_key_cb, static_cast<lv_event_code_t>(LV_EVENT_KEYBOARD),
                        nullptr);
}

/* ---- key injection through the real queue + keypad read path ---- */

void send(uint32_t code, const char *utf8, uint32_t mods, int state)
{
    key_item item{};
    item.key_code = code;
    item.key_state = state;
    item.mods = mods;
    item.input_context = input_context;
    if (utf8) std::snprintf(item.utf8, sizeof(item.utf8), "%s", utf8);
    if (utf8 && utf8[0] && !utf8[1]) item.codepoint = static_cast<unsigned char>(utf8[0]);
    item.keysym = item.codepoint;
    CHECK(cp0_keyboard_queue_push(&item) == 0);
    cp0_keyboard_dispatch_pending();
}

void tap(uint32_t code, const char *utf8 = nullptr, uint32_t mods = 0)
{
    send(code, utf8, mods, KBD_KEY_PRESSED);
    send(code, utf8, mods, KBD_KEY_RELEASED);
}

uint32_t code_for(char c)
{
    static const char row_q[] = "qwertyuiop", row_a[] = "asdfghjkl", row_z[] = "zxcvbnm";
    if (const char *p = std::strchr(row_q, c)) return KEY_Q + (p - row_q);
    if (const char *p = std::strchr(row_a, c)) return KEY_A + (p - row_a);
    if (const char *p = std::strchr(row_z, c)) return KEY_Z + (p - row_z);
    if (c >= 'A' && c <= 'Z') return code_for(static_cast<char>(c - 'A' + 'a'));
    if (c == ' ') return KEY_SPACE;
    if (c == '.') return KEY_DOT;
    return 0;
}

void type(const char *text)
{
    for (const char *c = text; *c; ++c) {
        const char utf8[2] = {*c, 0};
        const bool upper = *c >= 'A' && *c <= 'Z';
        tap(code_for(*c), utf8, upper ? KBD_MOD_SHIFT : 0);
        run_for(10);
    }
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 4);
    out_dir = argv[3];
    setenv("CP0_IME_FONT", argv[2], 1);

    GError *error = nullptr;
    GSubprocess *server = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, argv[1],
                                           "/tmp/mock_fcitx5_sim.log", nullptr);
    CHECK(server);
    GDataInputStream *out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(server));
    char *line = g_data_input_stream_read_line(out, nullptr, nullptr, &error);
    CHECK(line && std::string(line) == "ready");
    g_free(line);

    lv_init();
    lv_tick_set_cb(tick_ms);
    lv_display_t *display = lv_display_create(kWidth, kHeight);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(display, draw_buffer, nullptr, sizeof(draw_buffer),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush_cb);
    LV_EVENT_KEYBOARD = lv_event_register_id();
    cp0_keyboard_queue_init();

    lv_font_t *font = lv_freetype_font_create(argv[2], LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                              16, LV_FREETYPE_FONT_STYLE_NORMAL);
    CHECK(font);
    build_page(font);
    cp0_keyboard_create_lvgl_input_devices(); /* also starts the IME bridge */

    const uint32_t deadline = tick_ms() + 3000;
    while (cp0_ime_get_state() == CP0_IME_STATE_UNAVAILABLE &&
           static_cast<int32_t>(tick_ms() - deadline) < 0)
        run_for(20);
    CHECK(cp0_ime_get_state() == CP0_IME_STATE_DIRECT);

    /* Navigation context: the IME stays out of the way. */
    tap(KEY_A, "a");
    CHECK(field_text == "a");
    tap(KEY_BACKSPACE, "\x7f");
    CHECK(field_text.empty());

    /* Text field: direct typing still works (badge shows "A"). */
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
    type("Hello ");
    CHECK(field_text == "Hello ");
    screenshot("01_direct");

    /* Ctrl+Space -> Japanese */
    tap(KEY_SPACE, " ", KBD_MOD_CTRL);
    run_for(80);
    CHECK(cp0_ime_get_state() == CP0_IME_STATE_COMPOSING);
    CHECK(field_text == "Hello "); /* the trigger key never reaches the page */

    type("nihongo");
    CHECK(field_text == "Hello "); /* composing, nothing committed yet */
    screenshot("02_preedit");

    tap(KEY_SPACE, " ");
    screenshot("03_candidates");
    tap(KEY_ENTER, "\r");
    CHECK(field_text == "Hello 日本語");

    type("kanji");
    tap(KEY_SPACE, " ");
    tap(KEY_SPACE, " ");
    tap(KEY_SPACE, " ");
    screenshot("04_candidate_3");
    tap(KEY_ENTER, "\r");
    CHECK(field_text == "Hello 日本語幹事");

    type(".");
    CHECK(field_text == "Hello 日本語幹事。");

    /* Backspace with nothing composed goes to the page. */
    tap(KEY_BACKSPACE, "\x7f");
    CHECK(field_text == "Hello 日本語幹事");

    /* Leaving the text field mid-composition discards it and hides the panel. */
    type("henkan");
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_NAVIGATION);
    run_for(600);
    screenshot("05_left_text_field");
    CHECK(field_text == "Hello 日本語幹事");

    /* Suppression (e.g. password field) detaches the IME. */
    cp0_keyboard_set_input_context(KBD_INPUT_CONTEXT_TEXT);
    cp0_ime_push_suppress();
    type("pw");
    CHECK(field_text == "Hello 日本語幹事pw");
    cp0_ime_pop_suppress();

    std::printf("final field: %s\n", field_text.c_str());
    cp0_ime_deinit();
    g_subprocess_force_exit(server);
    std::puts("sim_ime_lvgl: ok");
    return 0;
}
