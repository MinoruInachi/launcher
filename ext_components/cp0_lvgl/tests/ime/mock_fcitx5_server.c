/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */

/* A tiny stand-in for fcitx5's DBus frontend, used by the IME tests.
 *
 * It owns org.fcitx.Fcitx5 on the session bus and implements the subset of
 * org.fcitx.Fcitx.InputMethod1 / org.fcitx.Fcitx.InputContext1 that cp0_lvgl
 * uses, with the same signatures as fcitx5 (src/frontend/dbusfrontend):
 *   CreateInputContext a(ss) -> oay, ProcessKeyEvent uuubu -> b,
 *   signals CommitString s, CurrentIM sss, UpdateClientSideUI
 *   a(si)ia(si)a(si)a(ss)iibb, ForwardKey uub.
 * Like fcitx5, CommitString is sent before the ProcessKeyEvent reply and the
 * panel update is sent later from an idle callback.
 *
 * The "engine" is a toy romaji -> hiragana converter with a 3-word dictionary,
 * toggled by Ctrl+Space. */

#include <gio/gio.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define XK_BackSpace 0xff08
#define XK_Return 0xff0d
#define XK_Escape 0xff1b
#define XK_Left 0xff51
#define XK_Right 0xff53
#define XK_space 0x20
#define STATE_CTRL (1u << 2)
/* Test-only: a key carrying this state bit makes the mock stall before it
 * processes the key, as a busy fcitx5 / mozc_server would. */
#define STATE_MOCK_SLOW (1u << 24)
#define MOCK_SLOW_US 400000

static const char introspection_xml[] =
    "<node>"
    " <interface name='org.fcitx.Fcitx.InputMethod1'>"
    "  <method name='CreateInputContext'>"
    "   <arg type='a(ss)' direction='in'/>"
    "   <arg type='o' direction='out'/><arg type='ay' direction='out'/>"
    "  </method>"
    "  <method name='Version'><arg type='u' direction='out'/></method>"
    " </interface>"
    " <interface name='org.fcitx.Fcitx.InputContext1'>"
    "  <method name='FocusIn'/><method name='FocusOut'/><method name='Reset'/>"
    "  <method name='DestroyIC'/>"
    "  <method name='SetCapability'><arg type='t' direction='in'/></method>"
    "  <method name='SelectCandidate'><arg type='i' direction='in'/></method>"
    "  <method name='ProcessKeyEvent'>"
    "   <arg type='u' direction='in'/><arg type='u' direction='in'/>"
    "   <arg type='u' direction='in'/><arg type='b' direction='in'/>"
    "   <arg type='u' direction='in'/><arg type='b' direction='out'/>"
    "  </method>"
    "  <signal name='CommitString'><arg type='s'/></signal>"
    "  <signal name='CurrentIM'><arg type='s'/><arg type='s'/><arg type='s'/></signal>"
    "  <signal name='UpdateClientSideUI'>"
    "   <arg type='a(si)'/><arg type='i'/><arg type='a(si)'/><arg type='a(si)'/>"
    "   <arg type='a(ss)'/><arg type='i'/><arg type='i'/><arg type='b'/><arg type='b'/>"
    "  </signal>"
    "  <signal name='ForwardKey'><arg type='u'/><arg type='u'/><arg type='b'/></signal>"
    " </interface>"
    "</node>";

struct romaji {
    const char *in;
    const char *out;
};

static const struct romaji table[] = {
    {"kya", "きゃ"}, {"kyu", "きゅ"}, {"kyo", "きょ"}, {"sha", "しゃ"}, {"shu", "しゅ"},
    {"sho", "しょ"}, {"chi", "ち"}, {"tsu", "つ"}, {"shi", "し"}, {"nya", "にゃ"},
    {"ja", "じゃ"}, {"ju", "じゅ"}, {"jo", "じょ"}, {"ka", "か"}, {"ki", "き"},
    {"ku", "く"}, {"ke", "け"}, {"ko", "こ"}, {"sa", "さ"}, {"si", "し"},
    {"su", "す"}, {"se", "せ"}, {"so", "そ"}, {"ta", "た"}, {"ti", "ち"},
    {"tu", "つ"}, {"te", "て"}, {"to", "と"}, {"na", "な"}, {"ni", "に"},
    {"nu", "ぬ"}, {"ne", "ね"}, {"no", "の"}, {"ha", "は"}, {"hi", "ひ"},
    {"fu", "ふ"}, {"hu", "ふ"}, {"he", "へ"}, {"ho", "ほ"}, {"ma", "ま"},
    {"mi", "み"}, {"mu", "む"}, {"me", "め"}, {"mo", "も"}, {"ya", "や"},
    {"yu", "ゆ"}, {"yo", "よ"}, {"ra", "ら"}, {"ri", "り"}, {"ru", "る"},
    {"re", "れ"}, {"ro", "ろ"}, {"wa", "わ"}, {"wo", "を"}, {"ga", "が"},
    {"gi", "ぎ"}, {"gu", "ぐ"}, {"ge", "げ"}, {"go", "ご"}, {"za", "ざ"},
    {"ji", "じ"}, {"zu", "ず"}, {"ze", "ぜ"}, {"zo", "ぞ"}, {"da", "だ"},
    {"de", "で"}, {"do", "ど"}, {"ba", "ば"}, {"bi", "び"}, {"bu", "ぶ"},
    {"be", "べ"}, {"bo", "ぼ"}, {"pa", "ぱ"}, {"pi", "ぴ"}, {"pu", "ぷ"},
    {"pe", "ぺ"}, {"po", "ぽ"}, {"nn", "ん"}, {"a", "あ"}, {"i", "い"},
    {"u", "う"}, {"e", "え"}, {"o", "お"}, {"-", "ー"},
};

struct dictionary_entry {
    const char *reading;
    const char *candidates[6];
};

static const struct dictionary_entry dictionary[] = {
    {"にほんご", {"日本語", "日本後", "ニホンゴ", "にほんご", NULL}},
    {"かんじ", {"漢字", "感じ", "幹事", "監事", "完治", NULL}},
    {"へんかん", {"変換", "返還", "偏官", "へんかん", NULL}},
};

typedef struct {
    gboolean japanese;
    GString *kana;    /* converted part of the pre-edit */
    GString *pending; /* romaji not yet converted */
    const struct dictionary_entry *entry;
    int cursor;
    char *client;
    GDBusConnection *connection;
    guint ui_idle;
} state_t;

static state_t st;
static guint ic_registration;
static GDBusNodeInfo *introspection;
static FILE *log_file;

static void log_line(const char *format, ...)
{
    if (!log_file) return;
    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

static void emit(const char *signal, GVariant *parameters)
{
    if (!st.client) return;
    g_dbus_connection_emit_signal(st.connection, st.client,
                                  "/org/freedesktop/portal/inputcontext/1",
                                  "org.fcitx.Fcitx.InputContext1", signal, parameters,
                                  NULL);
}

static void convert_pending(gboolean flush_n)
{
    for (;;) {
        gboolean matched = FALSE;
        for (size_t i = 0; i < G_N_ELEMENTS(table); ++i) {
            if (g_str_has_suffix(st.pending->str, table[i].in)) {
                const size_t keep = st.pending->len - strlen(table[i].in);
                g_string_append(st.kana, table[i].out);
                g_string_truncate(st.pending, keep);
                matched = TRUE;
                break;
            }
        }
        if (!matched) break;
    }
    /* "n" followed by a consonant (or flushed) is ん */
    if (st.pending->len >= 2 && st.pending->str[0] == 'n' &&
        !strchr("aiueoy", st.pending->str[1])) {
        g_string_append(st.kana, "ん");
        g_string_erase(st.pending, 0, 1);
        convert_pending(flush_n);
    }
    if (flush_n && strcmp(st.pending->str, "n") == 0) {
        g_string_append(st.kana, "ん");
        g_string_truncate(st.pending, 0);
    }
}

static gboolean has_preedit(void) { return st.kana->len || st.pending->len; }

static void clear_composition(void)
{
    g_string_truncate(st.kana, 0);
    g_string_truncate(st.pending, 0);
    st.entry = NULL;
    st.cursor = 0;
}

static int candidate_count(void)
{
    int count = 0;
    while (st.entry && count < 6 && st.entry->candidates[count]) ++count;
    return count;
}

static gboolean send_ui(gpointer data)
{
    (void)data;
    st.ui_idle = 0;
    GVariantBuilder preedit, aux_up, aux_down, candidates;
    g_variant_builder_init(&preedit, G_VARIANT_TYPE("a(si)"));
    g_variant_builder_init(&aux_up, G_VARIANT_TYPE("a(si)"));
    g_variant_builder_init(&aux_down, G_VARIANT_TYPE("a(si)"));
    g_variant_builder_init(&candidates, G_VARIANT_TYPE("a(ss)"));
    int cursor = -1;
    if (st.entry) {
        g_variant_builder_add(&preedit, "(si)", st.entry->candidates[st.cursor], 1 << 4);
        for (int i = 0; i < candidate_count(); ++i) {
            char label[8];
            g_snprintf(label, sizeof(label), "%d.", i + 1);
            g_variant_builder_add(&candidates, "(ss)", label, st.entry->candidates[i]);
        }
        cursor = st.cursor;
    } else if (has_preedit()) {
        GString *text = g_string_new(st.kana->str);
        g_string_append(text, st.pending->str);
        g_variant_builder_add(&preedit, "(si)", text->str, 1 << 3);
        g_string_free(text, TRUE);
    }
    emit("UpdateClientSideUI",
         g_variant_new("(a(si)ia(si)a(si)a(ss)iibb)", &preedit, -1, &aux_up, &aux_down,
                       &candidates, cursor, 0, FALSE, FALSE));
    return G_SOURCE_REMOVE;
}

static void schedule_ui(void)
{
    /* fcitx5 batches panel updates and flushes them after the key event. */
    if (!st.ui_idle) st.ui_idle = g_idle_add(send_ui, NULL);
}

static void send_current_im(void)
{
    if (st.japanese)
        emit("CurrentIM", g_variant_new("(sss)", "Mozc", "mozc", "ja"));
    else
        emit("CurrentIM",
             g_variant_new("(sss)", "Keyboard - English (US)", "keyboard-us", "en"));
}

static void commit(const char *text)
{
    log_line("commit %s", text);
    emit("CommitString", g_variant_new("(s)", text));
}

static gboolean process_key(guint32 sym, guint32 state, gboolean release)
{
    if (release) return FALSE;
    if (sym == XK_space && (state & STATE_CTRL)) {
        st.japanese = !st.japanese;
        clear_composition();
        send_current_im();
        schedule_ui();
        return TRUE;
    }
    if (!st.japanese) return FALSE;

    if (sym >= 'a' && sym <= 'z' && !(state & STATE_CTRL)) {
        if (st.entry) {
            commit(st.entry->candidates[st.cursor]);
            clear_composition();
        }
        g_string_append_c(st.pending, (char)sym);
        convert_pending(FALSE);
        schedule_ui();
        return TRUE;
    }
    if (sym == '.' || sym == ',') {
        if (has_preedit()) return TRUE;
        commit(sym == '.' ? "。" : "、");
        return TRUE;
    }
    if (!has_preedit()) return FALSE;

    switch (sym) {
    case XK_space:
        convert_pending(TRUE);
        if (!st.entry) {
            for (size_t i = 0; i < G_N_ELEMENTS(dictionary); ++i)
                if (strcmp(dictionary[i].reading, st.kana->str) == 0)
                    st.entry = &dictionary[i];
            st.cursor = 0;
        } else {
            st.cursor = (st.cursor + 1) % candidate_count();
        }
        schedule_ui();
        return TRUE;
    case XK_Right:
        if (st.entry) st.cursor = (st.cursor + 1) % candidate_count();
        schedule_ui();
        return TRUE;
    case XK_Left:
        if (st.entry) st.cursor = (st.cursor + candidate_count() - 1) % candidate_count();
        schedule_ui();
        return TRUE;
    case XK_Return:
        convert_pending(TRUE);
        commit(st.entry ? st.entry->candidates[st.cursor] : st.kana->str);
        clear_composition();
        schedule_ui();
        return TRUE;
    case XK_Escape:
        if (st.entry)
            st.entry = NULL;
        else
            clear_composition();
        schedule_ui();
        return TRUE;
    case XK_BackSpace:
        if (st.entry) {
            st.entry = NULL;
        } else if (st.pending->len) {
            g_string_truncate(st.pending, st.pending->len - 1);
        } else if (st.kana->len) {
            const char *end = st.kana->str + st.kana->len;
            const char *prev = g_utf8_find_prev_char(st.kana->str, end);
            g_string_truncate(st.kana, prev - st.kana->str);
        }
        schedule_ui();
        return TRUE;
    default:
        return TRUE;
    }
}

static void ic_method(GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *method, GVariant *parameters,
                      GDBusMethodInvocation *invocation, gpointer data)
{
    (void)connection;
    (void)path;
    (void)interface;
    (void)data;
    if (!st.client || strcmp(sender, st.client) != 0) {
        g_dbus_method_invocation_return_dbus_error(invocation, "org.fcitx.Error",
                                                   "wrong sender");
        return;
    }
    if (strcmp(method, "ProcessKeyEvent") == 0) {
        guint32 sym, code, state, time;
        gboolean release;
        g_variant_get(parameters, "(uuubu)", &sym, &code, &state, &release, &time);
        if (state & STATE_MOCK_SLOW) {
            log_line("stalling %d us", MOCK_SLOW_US);
            g_usleep(MOCK_SLOW_US);
            state &= ~STATE_MOCK_SLOW;
        }
        const gboolean handled = process_key(sym, state, release);
        log_line("key sym=0x%x code=%u state=0x%x %s -> %d", sym, code, state,
                 release ? "up" : "down", handled);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", handled));
        return;
    }
    log_line("call %s", method);
    if (strcmp(method, "FocusIn") == 0) {
        send_current_im();
    } else if (strcmp(method, "Reset") == 0 || strcmp(method, "FocusOut") == 0) {
        clear_composition();
        schedule_ui();
    } else if (strcmp(method, "SetCapability") == 0) {
        guint64 cap = 0;
        g_variant_get(parameters, "(t)", &cap);
        log_line("capability 0x%" G_GINT64_MODIFIER "x", cap);
    }
    g_dbus_method_invocation_return_value(invocation, NULL);
}

static const GDBusInterfaceVTable ic_vtable = {ic_method, NULL, NULL, {0}};

static void im_method(GDBusConnection *connection, const char *sender, const char *path,
                      const char *interface, const char *method, GVariant *parameters,
                      GDBusMethodInvocation *invocation, gpointer data)
{
    (void)path;
    (void)interface;
    (void)parameters;
    (void)data;
    if (strcmp(method, "Version") == 0) {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u)", 1u));
        return;
    }
    g_free(st.client);
    st.client = g_strdup(sender);
    st.connection = connection;
    st.japanese = FALSE;
    clear_composition();
    if (!ic_registration) {
        ic_registration = g_dbus_connection_register_object(
            connection, "/org/freedesktop/portal/inputcontext/1",
            introspection->interfaces[1], &ic_vtable, NULL, NULL, NULL);
    }
    log_line("create %s", sender);
    GVariantBuilder uuid;
    g_variant_builder_init(&uuid, G_VARIANT_TYPE("ay"));
    for (int i = 0; i < 16; ++i) g_variant_builder_add(&uuid, "y", (guchar)i);
    g_dbus_method_invocation_return_value(
        invocation, g_variant_new("(oay)", "/org/freedesktop/portal/inputcontext/1", &uuid));
}

static const GDBusInterfaceVTable im_vtable = {im_method, NULL, NULL, {0}};

static void on_bus(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)name;
    (void)data;
    g_dbus_connection_register_object(connection, "/org/freedesktop/portal/inputmethod",
                                      introspection->interfaces[0], &im_vtable, NULL,
                                      NULL, NULL);
}

static void on_name(GDBusConnection *connection, const char *name, gpointer data)
{
    (void)connection;
    (void)data;
    log_line("owned %s", name);
    printf("ready\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    if (argc > 1) log_file = fopen(argv[1], "w");
    st.kana = g_string_new(NULL);
    st.pending = g_string_new(NULL);
    introspection = g_dbus_node_info_new_for_xml(introspection_xml, NULL);
    g_bus_own_name(G_BUS_TYPE_SESSION, "org.fcitx.Fcitx5", G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_bus, on_name, NULL, NULL, NULL);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);
    return 0;
}
