/* Midnight Commander style command line above the F-key bar */
#include "fm.h"
#include <string.h>

static Panel *cmd_panel(FM *fm) { return active_panel(fm); }

static const char *cmd_dir(Panel *p)
{
    if (p->ssh_conn && p->ssh_conn->remote_path) return p->ssh_conn->remote_path;
    if (p->search_mode) return p->search_prev_cwd;
    return p->cwd;
}

/* prompt "dir$" – home shown as ~ */
void cmdline_update_prompt(FM *fm)
{
    if (!fm->cmd_prompt) return;
    Panel *p = cmd_panel(fm);
    const char *dir = cmd_dir(p);
    const char *home = g_get_home_dir();
    gchar *txt;
    if (p->ssh_conn)
        txt = g_strdup_printf("%s@%s:%s$", p->ssh_conn->user, p->ssh_conn->host, dir);
    else if (home && g_str_has_prefix(dir, home) &&
             (dir[strlen(home)] == '\0' || dir[strlen(home)] == '/'))
        txt = g_strdup_printf("~%s$", dir + strlen(home));
    else
        txt = g_strdup_printf("%s$", dir);
    gtk_label_set_text(GTK_LABEL(fm->cmd_prompt), txt);
    g_free(txt);
}

/* "/a/b/../c/." -> "/a/c" for server paths */
static gchar *remote_canon(const char *path)
{
    gchar **parts = g_strsplit(path, "/", -1);
    GPtrArray *st = g_ptr_array_new();
    for (int i = 0; parts[i]; i++) {
        if (!parts[i][0] || g_str_equal(parts[i], ".")) continue;
        if (g_str_equal(parts[i], "..")) { if (st->len) g_ptr_array_set_size(st, st->len - 1); continue; }
        g_ptr_array_add(st, parts[i]);
    }
    g_ptr_array_add(st, NULL);
    gchar *j = g_strjoinv("/", (gchar **)st->pdata);
    gchar *r = g_strconcat("/", j, NULL);
    g_free(j);
    g_ptr_array_free(st, TRUE);
    g_strfreev(parts);
    return r;
}

/* "cd [dir]" – handled internally like in mc */
static gboolean cmd_cd(FM *fm, const char *cmd)
{
    if (!(g_str_equal(cmd, "cd") || g_str_has_prefix(cmd, "cd ")))
        return FALSE;
    Panel *p = cmd_panel(fm);
    gchar *arg = g_strstrip(g_strdup(cmd + 2));
    gchar *target;
    if (!arg[0] || g_str_equal(arg, "~"))
        target = g_strdup(p->ssh_conn ? "/" : g_get_home_dir());
    else if (!p->ssh_conn && g_str_has_prefix(arg, "~/"))
        target = g_build_filename(g_get_home_dir(), arg + 2, NULL);
    else if (arg[0] == '/')
        target = g_strdup(arg);
    else
        target = g_build_filename(cmd_dir(p), arg, NULL);

    if (p->ssh_conn) {
        gchar *canon = remote_canon(target);
        g_free(target);
        target = canon;
    } else {
        gchar *canon = g_canonicalize_filename(target, NULL);
        g_free(target);
        target = canon;
        if (!g_file_test(target, G_FILE_TEST_IS_DIR)) {
            fm_status(fm, "cd: %s: No such directory", arg);
            g_free(target); g_free(arg);
            return TRUE;
        }
    }
    panel_load(p, target);
    g_free(target);
    g_free(arg);
    return TRUE;
}

/* run in the background (no terminal) in the panel's directory;
 * the result goes to the status message, both panels are reloaded */
typedef struct { FM *fm; gchar *cmd; } CmdRun;

/* ── history (Alt+P / Alt+N), kept in ~/.config/fm/cmd_history ── */
#define HIST_MAX 500
static GPtrArray *hist;          /* oldest first */
static int        hist_pos = -1; /* -1 = not browsing */
static gchar     *hist_draft;    /* text typed before browsing */

static gchar *hist_file(void)
{
    return g_build_filename(g_get_user_config_dir(), "fm", "cmd_history", NULL);
}

static void hist_load(void)
{
    if (hist) return;
    hist = g_ptr_array_new_with_free_func(g_free);
    gchar *path = hist_file(), *txt = NULL;
    if (g_file_get_contents(path, &txt, NULL, NULL)) {
        gchar **lines = g_strsplit(txt, "\n", -1);
        for (int i = 0; lines[i]; i++)
            if (lines[i][0]) g_ptr_array_add(hist, g_strdup(lines[i]));
        g_strfreev(lines);
    }
    g_free(txt); g_free(path);
}

static void hist_add(const char *cmd)
{
    hist_load();
    for (guint i = 0; i < hist->len; i++)       /* no duplicates: move to end */
        if (g_str_equal(hist->pdata[i], cmd)) { g_ptr_array_remove_index(hist, i); break; }
    g_ptr_array_add(hist, g_strdup(cmd));
    while (hist->len > HIST_MAX) g_ptr_array_remove_index(hist, 0);
    hist_pos = -1;
    g_clear_pointer(&hist_draft, g_free);

    GString *g = g_string_new(NULL);
    for (guint i = 0; i < hist->len; i++) {
        g_string_append(g, hist->pdata[i]);
        g_string_append_c(g, '\n');
    }
    gchar *path = hist_file(), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_file_set_contents(path, g->str, g->len, NULL);
    g_free(dir); g_free(path);
    g_string_free(g, TRUE);
}

static void hist_step(FM *fm, int dir)          /* -1 older, +1 newer */
{
    hist_load();
    if (!hist->len) return;
    GtkEditable *ed = GTK_EDITABLE(fm->cmdline);
    if (hist_pos < 0) {
        if (dir > 0) return;
        g_free(hist_draft);
        hist_draft = g_strdup(gtk_editable_get_text(ed));
        hist_pos = hist->len;
    }
    int np = hist_pos + dir;
    if (np < 0) return;
    if (np >= (int)hist->len) {
        hist_pos = -1;
        gtk_editable_set_text(ed, hist_draft ? hist_draft : "");
    } else {
        hist_pos = np;
        gtk_editable_set_text(ed, hist->pdata[np]);
    }
    gtk_editable_set_position(ed, -1);
}

static void cmd_report(FM *fm, const char *cmd, int code, gchar *out, gchar *err_out)
{
    {
        const char *msg = (err_out && *g_strstrip(err_out)) ? err_out
                        : (out && *g_strstrip(out)) ? out : NULL;
        gchar *line = msg ? g_strndup(msg, strcspn(msg, "\n")) : NULL;
        if (code == 0)
            fm_status(fm, "%s%s%s", cmd, line ? ": " : " – done", line ? line : "");
        else
            fm_status(fm, "%s – exit %d%s%s", cmd, code, line ? ": " : "", line ? line : "");
        g_free(line);
    }
    panel_reload(&fm->panels[0]);
    panel_reload(&fm->panels[1]);
}

static void on_cmd_done(GObject *src, GAsyncResult *res, gpointer data)
{
    CmdRun *r = data;
    GSubprocess *sp = G_SUBPROCESS(src);
    gchar *out = NULL, *err_out = NULL;
    GError *err = NULL;
    if (!g_subprocess_communicate_utf8_finish(sp, res, &out, &err_out, &err)) {
        fm_status(r->fm, "%s: %s", r->cmd, err ? err->message : "failed");
        g_clear_error(&err);
    } else {
        int code = g_subprocess_get_if_exited(sp) ? g_subprocess_get_exit_status(sp) : -1;
        cmd_report(r->fm, r->cmd, code, out, err_out);
    }
    g_free(out); g_free(err_out);
    g_free(r->cmd); g_free(r);
    g_object_unref(sp);
}


/* SFTP panel: same command over an SSH exec channel, in a thread */
typedef struct {
    FM *fm; SshConn *conn; gchar *dir, *cmd, *out, *err; int code;
} RemoteRun;

static void remote_thread(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void)t; (void)src; (void)c;
    RemoteRun *r = data;
    r->code = ssh_exec(r->conn, r->dir, r->cmd, &r->out, &r->err);
}

static void on_remote_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src; (void)res;
    RemoteRun *r = data;
    if (r->code < 0 && !r->out)
        fm_status(r->fm, "%s: cannot run the command over SSH", r->cmd);
    else
        cmd_report(r->fm, r->cmd, r->code, r->out, r->err);
    ssh_conn_unref(r->conn);
    g_free(r->dir); g_free(r->cmd); g_free(r->out); g_free(r->err);
    g_free(r);
}

static void cmd_exec(FM *fm, const char *cmd)
{
    Panel *p = cmd_panel(fm);
    if (p->ssh_conn) {
        RemoteRun *r = g_new0(RemoteRun, 1);
        r->fm = fm;
        r->conn = ssh_conn_ref(p->ssh_conn);
        r->dir = g_strdup(cmd_dir(p));
        r->cmd = g_strdup(cmd);
        r->code = -1;
        fm_status(fm, "Running on %s: %s", p->ssh_conn->host, cmd);
        GTask *t = g_task_new(NULL, NULL, on_remote_done, r);
        g_task_set_task_data(t, r, NULL);
        g_task_run_in_thread(t, remote_thread);
        g_object_unref(t);
        return;
    }
    GSubprocessLauncher *l = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
        G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_set_cwd(l, cmd_dir(p));
    GError *err = NULL;
    GSubprocess *sp = g_subprocess_launcher_spawn(l, &err, "sh", "-c", cmd, NULL);
    g_object_unref(l);
    if (!sp) {
        fm_status(fm, "Failed to run command: %s", err ? err->message : "?");
        g_clear_error(&err);
        return;
    }
    CmdRun *r = g_new0(CmdRun, 1);
    r->fm = fm;
    r->cmd = g_strdup(cmd);
    fm_status(fm, "Running: %s", cmd);
    g_subprocess_communicate_utf8_async(sp, NULL, NULL, on_cmd_done, r);
}

static void on_cmd_activate(GtkEntry *e, FM *fm)
{
    gchar *cmd = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(e))));
    if (cmd[0]) {
        hist_add(cmd);
        if (!cmd_cd(fm, cmd)) cmd_exec(fm, cmd);
        gtk_editable_set_text(GTK_EDITABLE(e), "");
    }
    g_free(cmd);
    panel_focus(cmd_panel(fm));
}

static gboolean on_cmd_key(GtkEventControllerKey *c, guint kv, guint kc,
                           GdkModifierType st, FM *fm)
{
    (void)c; (void)kc; (void)st;
    if (kv == GDK_KEY_Escape) {
        gtk_editable_set_text(GTK_EDITABLE(fm->cmdline), "");
        panel_focus(cmd_panel(fm));
        return TRUE;
    }
    return FALSE;
}

/* entry colours follow the light/dark appearance (mc look in light) */
void cmdline_apply_css(FM *fm)
{
    static GtkCssProvider *css;
    if (!css) {
        css = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(),
            GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 3);
    }
    gtk_css_provider_load_from_string(css, fm_dark(fm)
        ? "entry.fm-cmdline-entry { background: #1e1e1e; color: #e0e0e0;"
          " caret-color: #e0e0e0; }"
          "entry.fm-cmdline-entry text placeholder { color: #808080; }"
        : "entry.fm-cmdline-entry { background: #ffffff; color: #000000;"
          " caret-color: #000000; }"
          "entry.fm-cmdline-entry text placeholder { color: #808080; }");
}

GtkWidget *create_cmdline(FM *fm)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_add_css_class(box, "fm-cmdline");
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 2);

    cmdline_apply_css(fm);

    fm->cmdline = gtk_entry_new();
    gtk_widget_set_hexpand(fm->cmdline, TRUE);
    gtk_widget_add_css_class(fm->cmdline, "monospace");
    gtk_widget_add_css_class(fm->cmdline, "fm-cmdline-entry");
    gtk_box_append(GTK_BOX(box), fm->cmdline);
    g_signal_connect(fm->cmdline, "activate", G_CALLBACK(on_cmd_activate), fm);

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_cmd_key), fm);
    gtk_widget_add_controller(fm->cmdline, k);
    return box;
}

/* insert text at the cursor and move the focus to the command line */
void cmdline_insert(FM *fm, const char *text)
{
    if (!fm->cmdline || !text) return;
    GtkEditable *ed = GTK_EDITABLE(fm->cmdline);
    gtk_widget_grab_focus(fm->cmdline);
    gtk_editable_select_region(ed, -1, -1);
    int pos = gtk_editable_get_position(ed);
    gtk_editable_insert_text(ed, text, -1, &pos);
    gtk_editable_set_position(ed, pos);
}

static gint name_cmp(gconstpointer a, gconstpointer b)
{
    return g_ascii_strcasecmp(*(const char * const *)a, *(const char * const *)b);
}

/* ── Tab: complete the word before the cursor to a file name ── */
static void complete(FM *fm)
{
    Panel *p = cmd_panel(fm);
    GtkEditable *ed = GTK_EDITABLE(fm->cmdline);
    const char *txt = gtk_editable_get_text(ed);
    int pos = gtk_editable_get_position(ed);        /* in characters */
    const char *cur = g_utf8_offset_to_pointer(txt, pos);
    const char *ws = cur;                           /* word start */
    while (ws > txt && ws[-1] != ' ' && ws[-1] != '\t') ws--;
    gchar *word = g_strndup(ws, cur - ws);

    /* "dir/part": list dir (local only), else the panel's listing */
    const char *slash = strrchr(word, '/');
    const char *part = slash ? slash + 1 : word;
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    if (slash && !p->ssh_conn) {
        gchar *d = g_strndup(word, slash - word + 1);
        gchar *abs = d[0] == '/' ? g_strdup(d)
                   : g_str_has_prefix(d, "~/") ? g_build_filename(g_get_home_dir(), d + 2, NULL)
                   : g_build_filename(cmd_dir(p), d, NULL);
        GDir *gd = g_dir_open(abs, 0, NULL);
        const char *n;
        while (gd && (n = g_dir_read_name(gd)))
            if (g_str_has_prefix(n, part) && (part[0] == '.' || n[0] != '.')) {
                gchar *full = g_build_filename(abs, n, NULL);
                gboolean dir = g_file_test(full, G_FILE_TEST_IS_DIR);
                g_ptr_array_add(names, dir ? g_strconcat(n, "/", NULL) : g_strdup(n));
                g_free(full);
            }
        if (gd) g_dir_close(gd);
        g_free(abs); g_free(d);
    } else if (!slash && !p->search_mode) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
        for (guint i = 0; i < n; i++) {
            FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
            if (!g_str_equal(it->name, "..") && g_str_has_prefix(it->name, part))
                g_ptr_array_add(names, it->is_dir ? g_strconcat(it->name, "/", NULL)
                                                  : g_strdup(it->name));
            g_object_unref(it);
        }
    }

    if (names->len == 0) {
        gtk_widget_error_bell(fm->cmdline);
    } else {
        /* longest common prefix */
        const char *first = names->pdata[0];
        gsize lcp = strlen(first);
        for (guint i = 1; i < names->len; i++) {
            const char *o = names->pdata[i];
            gsize k = 0;
            while (k < lcp && first[k] == o[k]) k++;
            lcp = k;
        }
        while (lcp > 0 && !g_utf8_validate(first, lcp, NULL)) lcp--;
        GString *add = g_string_new(NULL);
        for (gsize k = strlen(part); k < lcp; k++) {
            if (strchr(" '\"\\$&;()|<>*?`!", first[k])) g_string_append_c(add, '\\');
            g_string_append_c(add, first[k]);
        }
        if (names->len == 1 && first[lcp - 1] != '/') g_string_append_c(add, ' ');
        if (add->len) {
            int ip = pos;
            gtk_editable_insert_text(ed, add->str, -1, &ip);
            gtk_editable_set_position(ed, ip);
        } else if (names->len > 1) {
            /* nothing more to add: list the candidates */
            g_ptr_array_sort(names, name_cmp);
            GString *l = g_string_new(NULL);
            for (guint i = 0; i < names->len && i < 40; i++) {
                if (i) g_string_append(l, "  ");
                g_string_append(l, (const char *)names->pdata[i]);
            }
            if (names->len > 40) g_string_append_printf(l, "  … (%u)", names->len);
            fm_status(fm, "%s", l->str);
            g_string_free(l, TRUE);
        }
        g_string_free(add, TRUE);
    }
    g_ptr_array_free(names, TRUE);
    g_free(word);
}

/* Called from the global key handler (capture phase).  Typing in a
 * panel goes to the command line; Ctrl+Enter inserts the file name. */
gboolean cmdline_handle_key(FM *fm, guint keyval, GdkModifierType state)
{
    if (!fm->cmdline) return FALSE;
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(fm->window));
    Panel *p = cmd_panel(fm);
    gboolean in_panel = f && (f == p->column_view ||
                              gtk_widget_is_ancestor(f, p->column_view));
    gboolean in_cmd = f && (f == fm->cmdline ||
                            gtk_widget_is_ancestor(f, fm->cmdline));
    gboolean ctl = (state & GDK_CONTROL_MASK) != 0;
    gboolean alt = (state & GDK_ALT_MASK) != 0;
    gboolean empty = gtk_editable_get_text(GTK_EDITABLE(fm->cmdline))[0] == '\0';

    /* in the command line: list navigation still moves the panel cursor */
    if (in_cmd) {
        if (keyval == GDK_KEY_Tab && !(state & (GDK_CONTROL_MASK | GDK_ALT_MASK |
                                                GDK_SHIFT_MASK))) {
            complete(fm);
            return TRUE;
        }
        if ((state & GDK_ALT_MASK) && !(state & GDK_CONTROL_MASK)) {
            guint kl = gdk_keyval_to_lower(keyval);
            if (kl == GDK_KEY_p) { hist_step(fm, -1); return TRUE; }
            if (kl == GDK_KEY_n) { hist_step(fm, +1); return TRUE; }
        }
        switch (keyval) {
        case GDK_KEY_Up: case GDK_KEY_Down:
        case GDK_KEY_Page_Up: case GDK_KEY_Page_Down: case GDK_KEY_Insert:
            panel_focus(p);
            on_tree_key_press(NULL, keyval, 0, state, p);
            return TRUE;
        default:
            return FALSE;
        }
    }
    if (!in_panel) return FALSE;

    /* Ctrl+Enter / Ctrl+Shift+Enter – file name / full path */
    if (ctl && !alt && !p->search_mode && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        gchar *s = (state & GDK_SHIFT_MASK) ? panel_cursor_fullpath(p)
                                            : panel_cursor_name(p);
        if (s && !g_str_equal(s, "..")) {
            gchar *q = g_shell_quote(s);
            gchar *ins = g_strconcat(q, " ", NULL);
            cmdline_insert(fm, ins);
            g_free(ins); g_free(q);
        }
        g_free(s);
        return TRUE;
    }
    if (ctl || alt) return FALSE;

    /* Enter / BackSpace with text typed – act on the command line */
    if (!empty && (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter)) {
        on_cmd_activate(GTK_ENTRY(fm->cmdline), fm);
        return TRUE;
    }
    if (!empty && keyval == GDK_KEY_BackSpace) {
        gtk_widget_grab_focus(fm->cmdline);
        gtk_editable_select_region(GTK_EDITABLE(fm->cmdline), -1, -1);
        int end = gtk_editable_get_position(GTK_EDITABLE(fm->cmdline));
        if (end > 0) gtk_editable_delete_text(GTK_EDITABLE(fm->cmdline), end - 1, end);
        return TRUE;
    }

    gunichar uc = gdk_keyval_to_unicode(keyval);
    if (!uc || !g_unichar_isprint(uc)) return FALSE;
    /* mc: with an empty line + - * and space keep their panel meaning */
    if (empty && (uc == ' ' || uc == '+' || uc == '-' || uc == '*'))
        return FALSE;
    gchar buf[8];
    buf[g_unichar_to_utf8(uc, buf)] = '\0';
    cmdline_insert(fm, buf);
    return TRUE;
}
