/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

#include "cp0_fcitx5_client.hpp"

#include <gio/gio.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cp0::ime {
namespace {

constexpr const char *kService = "org.fcitx.Fcitx5";
constexpr const char *kInputMethodPath = "/org/freedesktop/portal/inputmethod";
constexpr const char *kInputMethodInterface = "org.fcitx.Fcitx.InputMethod1";
constexpr const char *kInputContextInterface = "org.fcitx.Fcitx.InputContext1";

bool debug_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("CP0_IME_DEBUG");
        return value && value[0] && std::strcmp(value, "0") != 0;
    }();
    return enabled;
}

#define IME_LOG(...)                                                            \
    do {                                                                        \
        std::fprintf(stderr, "[IME] " __VA_ARGS__);                             \
        std::fputc('\n', stderr);                                               \
    } while (0)
#define IME_DEBUG(...)                                                          \
    do {                                                                        \
        if (debug_enabled()) IME_LOG(__VA_ARGS__);                              \
    } while (0)

/* Runs a GLib call with `context` as the thread-default context so that the
 * callbacks it registers are dispatched from that context. */
class ScopedThreadDefault
{
public:
    explicit ScopedThreadDefault(GMainContext *context) : context_(context)
    {
        g_main_context_push_thread_default(context_);
    }
    ~ScopedThreadDefault() { g_main_context_pop_thread_default(context_); }
    ScopedThreadDefault(const ScopedThreadDefault &) = delete;
    ScopedThreadDefault &operator=(const ScopedThreadDefault &) = delete;

private:
    GMainContext *context_;
};

void read_segments(GVariant *array, std::vector<TextSegment> &out)
{
    GVariantIter iter;
    g_variant_iter_init(&iter, array);
    const char *text = nullptr;
    gint32 format = 0;
    while (g_variant_iter_next(&iter, "(&si)", &text, &format))
        out.push_back(TextSegment{text ? text : "", format});
}

} // namespace

bool PanelState::empty() const
{
    auto blank = [](const std::vector<TextSegment> &segments) {
        for (const auto &segment : segments)
            if (!segment.text.empty()) return false;
        return true;
    };
    return blank(preedit) && blank(aux_up) && blank(aux_down) && candidates.empty();
}

bool parse_client_side_ui(GVariant *parameters, PanelState &out)
{
    out = PanelState{};
    if (!parameters ||
        !g_variant_is_of_type(parameters,
                              G_VARIANT_TYPE("(a(si)ia(si)a(si)a(ss)iibb)")))
        return false;

    GVariant *preedit = nullptr;
    GVariant *aux_up = nullptr;
    GVariant *aux_down = nullptr;
    GVariant *candidates = nullptr;
    gint32 preedit_cursor = -1;
    gint32 candidate_cursor = -1;
    gint32 layout_hint = 0;
    gboolean has_prev = FALSE;
    gboolean has_next = FALSE;
    g_variant_get(parameters, "(@a(si)i@a(si)@a(si)@a(ss)iibb)", &preedit,
                  &preedit_cursor, &aux_up, &aux_down, &candidates,
                  &candidate_cursor, &layout_hint, &has_prev, &has_next);

    read_segments(preedit, out.preedit);
    read_segments(aux_up, out.aux_up);
    read_segments(aux_down, out.aux_down);

    GVariantIter iter;
    g_variant_iter_init(&iter, candidates);
    const char *label = nullptr;
    const char *text = nullptr;
    while (g_variant_iter_next(&iter, "(&s&s)", &label, &text))
        out.candidates.push_back(Candidate{label ? label : "", text ? text : ""});

    out.preedit_cursor = preedit_cursor;
    out.candidate_cursor = candidate_cursor;
    out.layout_hint = layout_hint;
    out.has_prev = has_prev;
    out.has_next = has_next;

    g_variant_unref(preedit);
    g_variant_unref(aux_up);
    g_variant_unref(aux_down);
    g_variant_unref(candidates);
    return true;
}

Fcitx5Client::~Fcitx5Client() { stop(); }

bool Fcitx5Client::start(GMainContext *context, const std::string &program,
                         uint64_t capability, Callbacks callbacks)
{
    stop();
    if (!context) return false;
    context_ = g_main_context_ref(context);
    program_ = program;
    capability_ = capability;
    callbacks_ = std::move(callbacks);

    ScopedThreadDefault scope(context_);
    GError *error = nullptr;
    bus_ = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!bus_) {
        IME_LOG("session bus unavailable: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        g_main_context_unref(context_);
        context_ = nullptr;
        return false;
    }
    watch_id_ = g_bus_watch_name_on_connection(
        bus_, kService, G_BUS_NAME_WATCHER_FLAGS_NONE,
        &Fcitx5Client::on_name_appeared, &Fcitx5Client::on_name_vanished, this,
        nullptr);
    IME_DEBUG("watching %s", kService);
    return true;
}

void Fcitx5Client::stop()
{
    if (bus_ && !ic_path_.empty()) {
        call_ic("DestroyIC", nullptr);
        g_dbus_connection_flush_sync(bus_, nullptr, nullptr);
    }
    drop_input_context();
    if (watch_id_) {
        g_bus_unwatch_name(watch_id_);
        watch_id_ = 0;
    }
    owner_.clear();
    if (bus_) {
        g_object_unref(bus_);
        bus_ = nullptr;
    }
    if (context_) {
        /* Let pending idle callbacks (unwatch notifications) run while the
         * object is still alive. */
        while (g_main_context_iteration(context_, FALSE)) {
        }
        g_main_context_unref(context_);
        context_ = nullptr;
    }
    callbacks_ = Callbacks{};
}

void Fcitx5Client::on_name_appeared(GDBusConnection *, const char *name,
                                    const char *owner, void *data)
{
    auto *self = static_cast<Fcitx5Client *>(data);
    IME_DEBUG("%s appeared as %s", name, owner);
    if (self->owner_ == owner && !self->ic_path_.empty()) return;
    self->drop_input_context();
    self->owner_ = owner ? owner : "";
    self->create_input_context();
}

void Fcitx5Client::on_name_vanished(GDBusConnection *, const char *name, void *data)
{
    auto *self = static_cast<Fcitx5Client *>(data);
    const bool was_ready = self->ready();
    IME_DEBUG("%s vanished", name);
    self->drop_input_context();
    self->owner_.clear();
    if (was_ready && self->callbacks_.availability) self->callbacks_.availability(false);
}

void Fcitx5Client::create_input_context()
{
    if (!bus_ || owner_.empty()) return;
    cancellable_ = g_cancellable_new();

    GVariantBuilder args;
    g_variant_builder_init(&args, G_VARIANT_TYPE("a(ss)"));
    g_variant_builder_add(&args, "(ss)", "program", program_.c_str());

    ScopedThreadDefault scope(context_);
    g_dbus_connection_call(bus_, owner_.c_str(), kInputMethodPath,
                           kInputMethodInterface, "CreateInputContext",
                           g_variant_new("(a(ss))", &args), G_VARIANT_TYPE("(oay)"),
                           G_DBUS_CALL_FLAGS_NONE, 5000, cancellable_,
                           &Fcitx5Client::on_ic_created, this);
}

void Fcitx5Client::on_ic_created(GObject *source, GAsyncResult *result, void *data)
{
    GError *error = nullptr;
    GVariant *reply =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!reply) {
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            IME_LOG("CreateInputContext failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        return; /* `data` may be gone when the call was cancelled */
    }

    auto *self = static_cast<Fcitx5Client *>(data);
    const char *path = nullptr;
    g_variant_get(reply, "(&o@ay)", &path, nullptr);
    self->ic_path_ = path ? path : "";
    g_variant_unref(reply);
    g_clear_object(&self->cancellable_);
    if (self->ic_path_.empty()) return;

    {
        ScopedThreadDefault scope(self->context_);
        self->signal_id_ = g_dbus_connection_signal_subscribe(
            self->bus_, self->owner_.c_str(), kInputContextInterface, nullptr,
            self->ic_path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
            &Fcitx5Client::on_signal, self, nullptr);
    }
    self->set_capability(self->capability_);
    IME_LOG("input context ready at %s", self->ic_path_.c_str());
    if (self->callbacks_.availability) self->callbacks_.availability(true);
}

void Fcitx5Client::drop_input_context()
{
    cancel_resync();
    if (cancellable_) {
        g_cancellable_cancel(cancellable_);
        g_clear_object(&cancellable_);
    }
    if (bus_ && signal_id_) g_dbus_connection_signal_unsubscribe(bus_, signal_id_);
    signal_id_ = 0;
    ic_path_.clear();
}

void Fcitx5Client::on_signal(GDBusConnection *, const char *, const char *,
                             const char *, const char *signal,
                             GVariant *parameters, void *data)
{
    auto *self = static_cast<Fcitx5Client *>(data);
    if (self->resyncing_ && std::strcmp(signal, "CurrentIM") != 0) {
        /* Emitted for a key that timed out and already reached the page. */
        IME_DEBUG("resync: dropping stale %s", signal);
        return;
    }
    if (std::strcmp(signal, "CommitString") == 0) {
        const char *text = nullptr;
        if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(s)"))) {
            g_variant_get(parameters, "(&s)", &text);
            IME_DEBUG("commit '%s'", text);
            if (text && self->callbacks_.commit) self->callbacks_.commit(text);
        }
    } else if (std::strcmp(signal, "UpdateClientSideUI") == 0) {
        PanelState panel;
        if (parse_client_side_ui(parameters, panel) && self->callbacks_.panel)
            self->callbacks_.panel(panel);
    } else if (std::strcmp(signal, "CurrentIM") == 0) {
        if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) {
            const char *name = nullptr;
            const char *unique = nullptr;
            const char *language = nullptr;
            g_variant_get(parameters, "(&s&s&s)", &name, &unique, &language);
            IME_DEBUG("current IM %s (%s)", unique, language);
            if (self->callbacks_.current_im)
                self->callbacks_.current_im(CurrentInputMethod{name, unique, language});
        }
    } else if (std::strcmp(signal, "ForwardKey") == 0) {
        if (g_variant_is_of_type(parameters, G_VARIANT_TYPE("(uub)"))) {
            guint32 keysym = 0;
            guint32 state = 0;
            gboolean release = FALSE;
            g_variant_get(parameters, "(uub)", &keysym, &state, &release);
            if (self->callbacks_.forward_key)
                self->callbacks_.forward_key(keysym, state, release);
        }
    }
}

void Fcitx5Client::call_ic(const char *method, GVariant *parameters)
{
    if (!bus_ || ic_path_.empty()) {
        if (parameters) g_variant_unref(g_variant_ref_sink(parameters));
        return;
    }
    /* Fire-and-forget: messages on one connection keep their order, so a
     * later synchronous ProcessKeyEvent still observes this call first. */
    g_dbus_connection_call(bus_, owner_.c_str(), ic_path_.c_str(),
                           kInputContextInterface, method, parameters, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr, nullptr);
}

void Fcitx5Client::start_resync()
{
    if (!bus_ || ic_path_.empty()) return;
    cancel_resync();
    resyncing_ = true;
    resync_cancellable_ = g_cancellable_new();
    ScopedThreadDefault scope(context_);
    g_dbus_connection_call(bus_, owner_.c_str(), ic_path_.c_str(),
                           kInputContextInterface, "Reset", nullptr, nullptr,
                           G_DBUS_CALL_FLAGS_NONE, -1, resync_cancellable_,
                           &Fcitx5Client::on_resync_reset, this);
}

void Fcitx5Client::cancel_resync()
{
    if (resync_cancellable_) {
        g_cancellable_cancel(resync_cancellable_);
        g_clear_object(&resync_cancellable_);
    }
    resyncing_ = false;
}

void Fcitx5Client::on_resync_reset(GObject *source, GAsyncResult *result, void *data)
{
    GError *error = nullptr;
    GVariant *reply =
        g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
    if (!reply && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_clear_error(&error);
        return; /* `data` may be gone when the call was cancelled */
    }

    auto *self = static_cast<Fcitx5Client *>(data);
    g_clear_object(&self->resync_cancellable_);
    if (!reply) {
        const bool timed_out = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
        IME_LOG("resync Reset failed: %s", error ? error->message : "unknown");
        g_clear_error(&error);
        self->resyncing_ = false;
        /* Still hung: keep waiting. Other errors (the input context is gone)
         * end the resync; name tracking recreates the context if needed. */
        if (timed_out) self->start_resync();
        return;
    }
    g_variant_unref(reply);
    self->resyncing_ = false;
    IME_LOG("fcitx5 responsive again; resync done");
}

Fcitx5Client::KeyResult Fcitx5Client::process_key(uint32_t keysym, uint32_t keycode,
                                                  uint32_t state, bool release,
                                                  uint32_t time_ms, int timeout_ms)
{
    if (!ready() || resyncing_) return KeyResult::Failed;
    GError *error = nullptr;
    GVariant *reply = g_dbus_connection_call_sync(
        bus_, owner_.c_str(), ic_path_.c_str(), kInputContextInterface,
        "ProcessKeyEvent",
        g_variant_new("(uuubu)", keysym, keycode, state, release ? TRUE : FALSE,
                      time_ms),
        G_VARIANT_TYPE("(b)"), G_DBUS_CALL_FLAGS_NONE, timeout_ms, nullptr, &error);
    KeyResult result = KeyResult::Failed;
    if (reply) {
        gboolean value = FALSE;
        g_variant_get(reply, "(b)", &value);
        result = value ? KeyResult::Handled : KeyResult::NotHandled;
        g_variant_unref(reply);
    } else {
        IME_LOG("ProcessKeyEvent failed: %s", error ? error->message : "unknown");
        if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) {
            result = KeyResult::TimedOut;
            start_resync();
        }
        g_clear_error(&error);
    }
    IME_DEBUG("key sym=0x%x code=%u state=0x%x %s -> %s", keysym, keycode, state,
              release ? "up" : "down",
              result == KeyResult::Handled ? "handled" : "pass");

    /* fcitx5 emits CommitString/ForwardKey before it replies, so the signals
     * are already queued on our context. Dispatch them now to keep committed
     * text ahead of the next pass-through key. */
    while (context_ && g_main_context_iteration(context_, FALSE)) {
    }
    return result;
}

void Fcitx5Client::focus_in() { call_ic("FocusIn", nullptr); }

void Fcitx5Client::focus_out() { call_ic("FocusOut", nullptr); }

void Fcitx5Client::reset() { call_ic("Reset", nullptr); }

void Fcitx5Client::set_capability(uint64_t capability)
{
    capability_ = capability;
    call_ic("SetCapability", g_variant_new("(t)", static_cast<guint64>(capability)));
}

void Fcitx5Client::select_candidate(int index)
{
    call_ic("SelectCandidate", g_variant_new("(i)", index));
}

} // namespace cp0::ime
