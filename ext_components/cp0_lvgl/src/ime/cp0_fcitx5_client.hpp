/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* Minimal client for the fcitx5 "DBus Frontend" (org.fcitx.Fcitx.InputMethod1
 * / org.fcitx.Fcitx.InputContext1). It owns one input context, asks fcitx5 to
 * hand the input panel to the client (ClientSideInputPanel) and reports
 * commits, panel updates and forwarded keys through callbacks.
 *
 * Threading: every method and callback runs on the thread that iterates the
 * GMainContext given to start(). The LVGL bridge iterates it from the LVGL
 * thread, so no locking is needed. */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GMainContext GMainContext;
typedef struct _GVariant GVariant;
typedef struct _GObject GObject;
typedef struct _GAsyncResult GAsyncResult;
typedef struct _GCancellable GCancellable;

namespace cp0::ime {

struct TextSegment
{
    std::string text;
    int format = 0;
};

struct Candidate
{
    std::string label;
    std::string text;
};

struct PanelState
{
    std::vector<TextSegment> preedit;
    int preedit_cursor = -1;
    std::vector<TextSegment> aux_up;
    std::vector<TextSegment> aux_down;
    std::vector<Candidate> candidates;
    int candidate_cursor = -1;
    int layout_hint = 0;
    bool has_prev = false;
    bool has_next = false;

    bool empty() const;
};

/* Decodes the UpdateClientSideUI signal body "(a(si)ia(si)a(si)a(ss)iibb)". */
bool parse_client_side_ui(GVariant *parameters, PanelState &out);

struct CurrentInputMethod
{
    std::string name;
    std::string unique_name;
    std::string language;
};

class Fcitx5Client
{
public:
    struct Callbacks
    {
        std::function<void(const std::string &)> commit;
        std::function<void(const PanelState &)> panel;
        std::function<void(const CurrentInputMethod &)> current_im;
        std::function<void(uint32_t keysym, uint32_t state, bool release)> forward_key;
        std::function<void(bool ready)> availability;
    };

    Fcitx5Client() = default;
    ~Fcitx5Client();
    Fcitx5Client(const Fcitx5Client &) = delete;
    Fcitx5Client &operator=(const Fcitx5Client &) = delete;

    /* Connects to the session bus and watches org.fcitx.Fcitx5. The input
     * context is created whenever fcitx5 appears (now or later). Returns
     * false only if the session bus is unreachable. */
    bool start(GMainContext *context, const std::string &program,
               uint64_t capability, Callbacks callbacks);
    void stop();

    bool ready() const { return !ic_path_.empty(); }

    enum class KeyResult
    {
        Handled,    /* fcitx5 consumed the key */
        NotHandled, /* the key belongs to the page */
        TimedOut,   /* no reply in time; the key belongs to the page */
        Failed,     /* not ready, resyncing or a D-Bus error */
    };

    /* Synchronous ProcessKeyEvent. Only Handled means the key was consumed;
     * every other result leaves the key to the page so typing keeps working
     * without the IME.
     *
     * After a timeout fcitx5 may still process the key later and emit a
     * commit for text the page already received. The client therefore starts
     * a resync: it sends Reset and, until that call replies, drops commits,
     * forwarded keys and panel updates and answers process_key() with Failed
     * without blocking. D-Bus keeps messages from one peer in order, so the
     * Reset reply arrives after every signal fcitx5 emitted for older keys. */
    KeyResult process_key(uint32_t keysym, uint32_t keycode, uint32_t state,
                          bool release, uint32_t time_ms, int timeout_ms);

    bool resyncing() const { return resyncing_; }

    void focus_in();
    void focus_out();
    void reset();
    void set_capability(uint64_t capability);
    void select_candidate(int index);

private:
    static void on_name_appeared(GDBusConnection *connection, const char *name,
                                 const char *owner, void *self);
    static void on_name_vanished(GDBusConnection *connection, const char *name,
                                 void *self);
    static void on_ic_created(GObject *source, GAsyncResult *result, void *self);
    static void on_resync_reset(GObject *source, GAsyncResult *result, void *self);
    static void on_signal(GDBusConnection *connection, const char *sender,
                          const char *path, const char *interface,
                          const char *signal, GVariant *parameters, void *self);

    void create_input_context();
    void drop_input_context();
    void call_ic(const char *method, GVariant *parameters);
    void start_resync();
    void cancel_resync();

    GMainContext *context_ = nullptr;
    GDBusConnection *bus_ = nullptr;
    unsigned watch_id_ = 0;
    unsigned signal_id_ = 0;
    GCancellable *cancellable_ = nullptr;
    GCancellable *resync_cancellable_ = nullptr;
    bool resyncing_ = false;
    std::string owner_;
    std::string ic_path_;
    std::string program_;
    uint64_t capability_ = 0;
    Callbacks callbacks_;
};

} // namespace cp0::ime
