/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* Protocol test for cp0::ime::Fcitx5Client against mock_fcitx5_server.
 * Run under its own session bus:
 *   dbus-run-session -- test_fcitx5_client /path/to/mock_fcitx5_server */

#include "../../src/ime/cp0_fcitx5_client.hpp"
#include "../../src/cp0_ime_util.h"

#include <gio/gio.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace cp0::ime;

namespace {

GMainContext *context;
Fcitx5Client client;
std::vector<std::string> commits;
std::vector<PanelState> panels;
std::vector<CurrentInputMethod> ims;
int ready_changes = 0;
bool is_ready = false;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                         #cond);                                                 \
            std::abort();                                                        \
        }                                                                        \
    } while (0)

template <typename Pred> bool pump_until(Pred pred, int timeout_ms = 3000)
{
    for (int waited = 0; waited < timeout_ms; waited += 5) {
        while (g_main_context_iteration(context, FALSE)) {
        }
        if (pred()) return true;
        g_usleep(5000);
    }
    return pred();
}

void pump_for(int ms)
{
    pump_until([] { return false; }, ms);
}

GSubprocess *spawn_server(const char *path)
{
    GError *error = nullptr;
    GSubprocess *process = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE, &error, path,
                                            "/tmp/mock_fcitx5_client_test.log", nullptr);
    CHECK(process);
    GDataInputStream *out =
        g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    char *line = g_data_input_stream_read_line(out, nullptr, nullptr, &error);
    CHECK(line && std::string(line) == "ready");
    g_free(line);
    g_object_unref(out);
    return process;
}

constexpr uint32_t kMockSlow = 1u << 24; // see STATE_MOCK_SLOW in the mock

bool key(uint32_t keysym, uint32_t state = 0, bool release = false)
{
    return client.process_key(keysym, 0, state, release, 0, 1000) ==
           Fcitx5Client::KeyResult::Handled;
}

std::string preedit_text(const PanelState &panel)
{
    std::string text;
    for (const auto &segment : panel.preedit) text += segment.text;
    return text;
}

} // namespace

int main(int argc, char **argv)
{
    CHECK(argc == 2);
    GSubprocess *server = spawn_server(argv[1]);

    context = g_main_context_new();
    Fcitx5Client::Callbacks callbacks;
    callbacks.commit = [](const std::string &text) { commits.push_back(text); };
    callbacks.panel = [](const PanelState &panel) { panels.push_back(panel); };
    callbacks.current_im = [](const CurrentInputMethod &im) { ims.push_back(im); };
    callbacks.availability = [](bool ready) {
        is_ready = ready;
        ++ready_changes;
    };
    CHECK(client.start(context, "cp0-ime-test", CP0_IME_CAP_CLIENT_SIDE_INPUT_PANEL,
                       callbacks));
    CHECK(pump_until([] { return is_ready; }));
    CHECK(client.ready());

    client.focus_in();
    CHECK(pump_until([] { return !ims.empty(); }));
    CHECK(ims.back().unique_name == "keyboard-us");

    // Direct mode: letters pass through untouched.
    CHECK(!key('a'));
    CHECK(commits.empty());

    // Ctrl+Space switches to the Japanese engine.
    CHECK(key(0x20, CP0_IME_KEYSTATE_CTRL));
    CHECK(pump_until([] { return ims.back().language == "ja"; }));

    // Typing romaji builds an underlined pre-edit.
    for (char c : std::string("nihongo")) CHECK(key(static_cast<uint32_t>(c)));
    CHECK(pump_until([] { return !panels.empty() && preedit_text(panels.back()) == "にほんご"; }));
    CHECK(panels.back().preedit[0].format & CP0_IME_FORMAT_UNDERLINE);
    CHECK(panels.back().candidates.empty());

    // Space converts and opens the candidate list.
    CHECK(key(0x20));
    CHECK(pump_until([] { return !panels.back().candidates.empty(); }));
    CHECK(panels.back().candidates.size() == 4);
    CHECK(panels.back().candidates[0].label == "1.");
    CHECK(panels.back().candidates[0].text == "日本語");
    CHECK(panels.back().candidate_cursor == 0);
    CHECK(panels.back().preedit[0].format & CP0_IME_FORMAT_HIGHLIGHT);

    CHECK(key(0x20));
    CHECK(pump_until([] { return panels.back().candidate_cursor == 1; }));

    // Return commits. The commit must be dispatched before process_key()
    // returns so that it stays ahead of the next pass-through key.
    CHECK(key(0xff0d));
    CHECK(commits.size() == 1 && commits[0] == "日本後");
    CHECK(pump_until([] { return panels.back().empty(); }));

    // Punctuation commits immediately.
    CHECK(key('.'));
    CHECK(commits.size() == 2 && commits[1] == "。");

    // Keys the engine does not want are reported as not handled.
    CHECK(!key(0xff08));          // BackSpace with nothing composed
    CHECK(!key('a', 0, true));    // releases

    // Reset discards an unfinished composition.
    CHECK(key('k'));
    CHECK(key('a'));
    client.reset();
    CHECK(pump_until([] { return panels.back().empty(); }));
    CHECK(commits.size() == 2);

    // A key fcitx5 answers too late reaches the page, so the commit fcitx5
    // emits for it afterwards must be dropped instead of typed twice.
    CHECK(client.process_key('.', 0, kMockSlow, false, 0, 100) ==
          Fcitx5Client::KeyResult::TimedOut);
    CHECK(client.resyncing());
    // While resyncing, keys pass through at once instead of blocking.
    const gint64 before = g_get_monotonic_time();
    CHECK(client.process_key('a', 0, 0, false, 0, 1000) == Fcitx5Client::KeyResult::Failed);
    CHECK(g_get_monotonic_time() - before < 50000);
    CHECK(pump_until([] { return !client.resyncing(); }));
    pump_for(50);
    CHECK(commits.size() == 2); // the late "。" was dropped
    // Back to normal afterwards.
    CHECK(key('.'));
    CHECK(commits.size() == 3 && commits[2] == "。");

    // fcitx5 restarting: the client notices and recreates the context.
    g_subprocess_force_exit(server);
    g_subprocess_wait(server, nullptr, nullptr);
    g_object_unref(server);
    CHECK(pump_until([] { return !is_ready; }));
    CHECK(!client.ready());
    CHECK(!key('a')); // no fcitx5: keys pass through
    server = spawn_server(argv[1]);
    CHECK(pump_until([] { return is_ready; }));
    client.focus_in();
    CHECK(key(0x20, CP0_IME_KEYSTATE_CTRL));
    CHECK(key('k'));
    CHECK(key('a'));
    CHECK(key(0xff0d));
    CHECK(commits.size() == 4 && commits[3] == "か");

    client.stop();
    pump_for(50);
    g_subprocess_force_exit(server);
    g_object_unref(server);
    g_main_context_unref(context);
    std::puts("test_fcitx5_client: ok");
    return 0;
}
