#!/bin/sh
# SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
# SPDX-License-Identifier: MIT
#
# Headless end-to-end check of the fcitx5 IME bridge (keypad path + bridge +
# LVGL panel) against mock_fcitx5_server. Writes PNG screenshots.
#
# usage: run_lvgl_sim.sh <lvgl_dir> <lv_conf_dir> [out_dir]
#   lvgl_dir     LVGL 9.5 checkout (the directory that contains lvgl.h)
#   lv_conf_dir  directory with an lv_conf.h: LV_USE_FREETYPE 1, LV_USE_SDL 0,
#                LV_COLOR_DEPTH 32
# needs: gio-2.0, freetype2, libpng, dbus-run-session

set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
lvgl=$(CDPATH= cd -- "$1" && pwd)
conf=$(CDPATH= cd -- "$2" && pwd)
out=${3:-/tmp/cp0_ime_sim}
work=${TMPDIR:-/tmp}/cp0_ime_sim_build
font="$root/../../projects/APPLaunch/APPLaunch/share/font/NotoSansCJK-Regular.ttc"

mkdir -p "$work/inc" "$work/lvgl_obj" "$out"
ln -sfn "$lvgl" "$work/inc/lvgl"
flags="-DLV_CONF_INCLUDE_SIMPLE -I$conf -I$work/inc -I$root/include -I$root/src \
$(pkg-config --cflags freetype2 gio-2.0 libpng)"

if [ ! -f "$work/liblvgl.a" ]; then
    find "$lvgl/src" -name '*.c' | while read -r source; do
        object="$work/lvgl_obj/$(printf '%s' "$source" | md5sum | cut -c1-16).o"
        printf '%s %s\n' "$source" "$object"
    done > "$work/lvgl_list"
    # shellcheck disable=SC2086
    xargs -P "$(nproc)" -n 2 sh -c "${CC:-cc} -O1 -w $flags -c \"\$0\" -o \"\$1\"" \
        < "$work/lvgl_list"
    ar rcs "$work/liblvgl.a" "$work"/lvgl_obj/*.o
fi

cc=${CC:-cc}
cxx=${CXX:-c++}
# shellcheck disable=SC2086
$cc -std=c11 -Wall -Wextra -Werror "$root/tests/ime/mock_fcitx5_server.c" \
    $(pkg-config --cflags --libs gio-2.0) -o "$work/mock_fcitx5"
# shellcheck disable=SC2086
{
    $cc -std=gnu11 -c $flags "$root/src/cp0/cp0_keyboard_lvgl_input.c" -o "$work/input.o"
    $cc -std=gnu11 -Wall -Werror -c $flags "$root/src/cp0_keyboard_queue.c" -o "$work/queue.o"
    $cc -std=gnu11 -Wall -Werror -c $flags "$root/src/cp0_esc_state.c" -o "$work/esc.o"
    $cc -std=gnu11 -Wall -Werror -c $flags "$root/src/cp0_ime_util.c" -o "$work/util.o"
    $cc -std=gnu11 -Wall -Werror -c $flags "$root/src/cp0_keyboard_text.c" -o "$work/text.o"
    $cxx -std=c++17 -Wall -Wextra -Werror -DCP0_LVGL_IME_FCITX5=1 -c $flags \
        "$root/src/ime/cp0_ime_fcitx5.cpp" -o "$work/bridge.o"
    $cxx -std=c++17 -Wall -Wextra -Werror -c $flags \
        "$root/src/ime/cp0_fcitx5_client.cpp" -o "$work/client.o"
    $cxx -std=c++17 -Wall -Wextra -c $flags "$root/tests/ime/sim_ime_lvgl.cpp" -o "$work/sim.o"
    $cxx "$work"/input.o "$work"/queue.o "$work"/esc.o "$work"/util.o "$work"/text.o \
        "$work"/bridge.o "$work"/client.o "$work"/sim.o "$work/liblvgl.a" \
        $(pkg-config --libs freetype2 gio-2.0 libpng) -lpthread -o "$work/sim_ime_lvgl"
}
timeout 60s dbus-run-session -- "$work/sim_ime_lvgl" "$work/mock_fcitx5" "$font" "$out"
