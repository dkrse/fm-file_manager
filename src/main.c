#include "fm.h"
#include "dialogs.h"
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glib/gstdio.h>

static FM g_fm;

/* ─────────────────────────────────────────────────────────────────── *
 *  Utility
 * ─────────────────────────────────────────────────────────────────── */

void fm_status(FM *fm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(fm->statusbar), msg);
    g_free(msg);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel helpers
 * ─────────────────────────────────────────────────────────────────── */

void panel_update_status(Panel *p)
{
    guint n_marks = p->marks ? g_hash_table_size(p->marks) : 0;
    guint total = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));

    gchar *msg;
    if (n_marks > 0) {
        /* total size of marked files (directories are not walked) */
        gint64 bytes = 0;
        guint ns = g_list_model_get_n_items(G_LIST_MODEL(p->store));
        for (guint i = 0; i < ns; i++) {
            FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
            if (it->marked && !it->is_dir && it->size_raw > 0) bytes += it->size_raw;
            g_object_unref(it);
        }
        gchar *sz = fmt_size(bytes);
        msg = g_strdup_printf("%u marked (%s)  |  %u items",
                              n_marks, sz, total > 0 ? total - 1 : 0);
        g_free(sz);
    } else
        msg = g_strdup_printf("%u items", total > 0 ? total - 1 : 0);
    gtk_label_set_text(GTK_LABEL(p->status_label), msg);
    g_free(msg);
}

gchar *panel_cursor_name(Panel *p)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (p->cursor_pos >= n) return NULL;
    FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                           p->cursor_pos);
    if (!item) return NULL;
    gchar *name = g_strdup(item->name);
    g_object_unref(item);
    return name;
}

gchar *panel_cursor_fullpath(Panel *p)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (p->cursor_pos >= n) return NULL;
    FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                           p->cursor_pos);
    if (!item) return NULL;
    gchar *path;
    if (p->search_mode && item->dir_path)
        path = g_build_filename(item->dir_path, item->name, NULL);
    else if (p->arc_file)       /* display path only – not on disk */
        path = g_build_filename(p->arc_file, p->arc_dir, item->name, NULL);
    else
        path = g_build_filename(p->cwd, item->name, NULL);
    g_object_unref(item);
    return path;
}

GList *panel_selection(Panel *p)
{
    /* Collect marked items */
    GList *result = NULL;
    if (p->marks && g_hash_table_size(p->marks) > 0) {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
        for (guint i = 0; i < count; i++) {
            FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
            if (item) {
                if (item->marked && strcmp(item->name, "..") != 0)
                    result = g_list_prepend(result, g_strdup(item->name));
                g_object_unref(item);
            }
        }
        if (result) return g_list_reverse(result);
    }

    /* No marks → fall back to cursor item */
    gchar *name = panel_cursor_name(p);
    if (name && strcmp(name, "..") != 0)
        return g_list_prepend(NULL, name);
    g_free(name);
    return NULL;
}

const char *panel_dir(Panel *p)
{
    return p->ssh_conn ? p->ssh_conn->remote_path : p->cwd;
}

/* Full paths of marked items (or cursor item); remote paths for SFTP,
 * the real location for search results, "<archive>/<inner>" inside an
 * archive (not openable – see arcpanel.c). */
GList *panel_selection_paths(Panel *p)
{
    GList *out = NULL;
    gboolean have_marks = FALSE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));

    /* The items' own flags decide: the key set may hold names that are
     * hidden by the mask or keyed differently (search results). */
    for (guint i = 0; i < n && !have_marks; i++) {
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (!it) continue;
        have_marks = it->marked && strcmp(it->name, "..") != 0;
        g_object_unref(it);
    }

    for (guint i = 0; i < n; i++) {
        if (!have_marks && i != p->cursor_pos) continue;
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (!it) continue;
        if ((!have_marks || it->marked) && strcmp(it->name, "..") != 0 &&
            !(p->search_mode && !it->dir_path)) {          /* skip group headers */
            gchar *path;
            if (p->ssh_conn)
                path = fs_remote_join(p->ssh_conn->remote_path, it->name);
            else if (p->search_mode)
                path = g_build_filename(it->dir_path, it->name, NULL);
            else if (p->arc_file)   /* display path – callers gate archives */
                path = g_build_filename(p->arc_file, p->arc_dir, it->name, NULL);
            else
                path = g_build_filename(p->cwd, it->name, NULL);
            out = g_list_prepend(out, path);
        }
        g_object_unref(it);
    }
    return g_list_reverse(out);
}

/* Scroll offset of each visited directory, so that going back up shows
 * the list as it was (not just the right row somewhere on screen) */
void panel_scroll_remember(Panel *p)
{
    const char *dir = panel_dir(p);
    if (!dir || !p->column_view || p->search_mode) return;
    if (!p->scroll_pos)
        p->scroll_pos = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (g_hash_table_size(p->scroll_pos) > 256) g_hash_table_remove_all(p->scroll_pos);
    GtkAdjustment *va = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->column_view));
    double *v = g_new(double, 1);
    *v = va ? gtk_adjustment_get_value(va) : 0;
    g_hash_table_replace(p->scroll_pos, g_strdup(dir), v);
}

typedef struct { Panel *p; gchar *dir; double value; } ScrollRestore;

static gboolean scroll_restore_idle(gpointer d)
{
    ScrollRestore *r = d;
    const char *dir = panel_dir(r->p);
    if (dir && strcmp(dir, r->dir) == 0) {   /* still the same directory */
        GtkAdjustment *va = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(r->p->column_view));
        if (va) gtk_adjustment_set_value(va, r->value);
    }
    g_free(r->dir);
    g_free(r);
    return G_SOURCE_REMOVE;
}

/* Applied after the next layout – the pending scroll_to of the cursor
 * would override an offset set right away */
static void panel_scroll_restore(Panel *p)
{
    const char *dir = panel_dir(p);
    double *v = p->scroll_pos && dir ? g_hash_table_lookup(p->scroll_pos, dir) : NULL;
    if (!v) return;
    ScrollRestore *r = g_new(ScrollRestore, 1);
    r->p = p; r->dir = g_strdup(dir); r->value = *v;
    g_idle_add_full(G_PRIORITY_LOW, scroll_restore_idle, r, NULL);
}

void panel_select_name(Panel *p, const char *name)
{
    if (!name) return;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *fi = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        gboolean hit = fi && strcmp(fi->name, name) == 0;
        if (fi) g_object_unref(fi);
        if (!hit) continue;
        p->cursor_pos = i;
        p->inhibit_sel = TRUE;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i, NULL,
            p->fm->active == p->idx ? GTK_LIST_SCROLL_SELECT : GTK_LIST_SCROLL_NONE,
            NULL);
        p->inhibit_sel = FALSE;
        return;
    }
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel: load directory
 * ─────────────────────────────────────────────────────────────────── */

/* ─────────────────────────────────────────────────────────────────── *
 *  Directory reading off the main thread
 *
 *  readdir + fstatat run in a worker.  Local disks answer within a few
 *  ms and the caller simply waits (panel_load stays synchronous for its
 *  callers).  When a slow or network mount takes longer than
 *  DIR_WAIT_MS a small modal "Reading…" window appears; Esc / Cancel
 *  leaves the panel where it was and the worker result is discarded.
 *  The waiting part is generic (fm_wait_work) – archive listings use it
 *  too (arcpanel.c).
 * ─────────────────────────────────────────────────────────────────── */

#define DIR_WAIT_MS 150

/* Generic part: `fn(arg)` runs in a worker, the caller waits (see
 * fm_wait_work in fm.h).  Shared by the worker and the waiting side. */
typedef struct {
    GThreadFunc    fn;
    gpointer       arg;
    GDestroyNotify free_arg;
    GDestroyNotify free_result;
    gpointer       result;
    GMutex         m;
    GCond          c;
    gboolean       done;
    GMainLoop     *loop;      /* set when the main thread waits in a loop */
    gint           refs;
} WaitWork;

static void wait_work_unref(WaitWork *w)
{
    if (!g_atomic_int_dec_and_test(&w->refs)) return;
    if (w->result && w->free_result) w->free_result(w->result);
    if (w->free_arg) w->free_arg(w->arg);
    g_mutex_clear(&w->m);
    g_cond_clear(&w->c);
    g_free(w);
}

static gboolean wait_work_quit_idle(gpointer loop)
{
    g_main_loop_quit(loop);
    g_main_loop_unref(loop);
    return G_SOURCE_REMOVE;
}

static gpointer wait_work_thread(gpointer data)
{
    WaitWork *w = data;
    gpointer r = w->fn(w->arg);
    g_mutex_lock(&w->m);
    w->result = r;
    w->done = TRUE;
    g_cond_signal(&w->c);
    if (w->loop) g_idle_add(wait_work_quit_idle, g_main_loop_ref(w->loop));
    g_mutex_unlock(&w->m);
    wait_work_unref(w);
    return NULL;
}

typedef struct { GMainLoop *loop; gboolean cancelled; } DirWait;

static void dir_wait_cancel(DirWait *w)
{
    w->cancelled = TRUE;
    g_main_loop_quit(w->loop);
}

static gboolean dir_wait_key(GtkEventControllerKey *k, guint keyval, guint code,
                             GdkModifierType st, DirWait *w)
{
    (void)k; (void)code; (void)st;
    if (keyval != GDK_KEY_Escape) return FALSE;
    dir_wait_cancel(w);
    return TRUE;
}

static gboolean dir_wait_close(GtkWindow *win, DirWait *w)
{
    (void)win;
    dir_wait_cancel(w);
    return TRUE;
}

gpointer fm_wait_work(FM *fm, const char *title, const char *what,
                      GThreadFunc fn, gpointer arg, GDestroyNotify free_arg,
                      GDestroyNotify free_result, void (*cancel)(gpointer arg),
                      gboolean *cancelled)
{
    *cancelled = FALSE;
    WaitWork *d = g_new0(WaitWork, 1);
    d->fn          = fn;
    d->arg         = arg;
    d->free_arg    = free_arg;
    d->free_result = free_result;
    d->refs        = 2;
    g_mutex_init(&d->m);
    g_cond_init(&d->c);
    g_thread_unref(g_thread_new("fm-wait", wait_work_thread, d));

    gint64 until = g_get_monotonic_time() + DIR_WAIT_MS * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&d->m);
    while (!d->done && g_cond_wait_until(&d->c, &d->m, until))
        ;
    DirWait w = { NULL, FALSE };
    if (!d->done) {
        w.loop = g_main_loop_new(NULL, FALSE);
        d->loop = w.loop;
    }
    g_mutex_unlock(&d->m);

    if (w.loop) {
        GtkWidget *win = gtk_window_new();
        gtk_window_set_title(GTK_WINDOW(win), title);
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fm->window));
        gtk_window_set_modal(GTK_WINDOW(win), TRUE);
        gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
        gtk_widget_set_margin_top(box, 16);
        gtk_widget_set_margin_bottom(box, 16);
        gtk_widget_set_margin_start(box, 20);
        gtk_widget_set_margin_end(box, 20);
        gchar *msg = g_strdup_printf("Reading %s…", what);
        GtkWidget *lbl = gtk_label_new(msg);
        g_free(msg);
        gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl), 60);
        GtkWidget *spin = gtk_spinner_new();
        gtk_spinner_start(GTK_SPINNER(spin));
        GtkWidget *btn = gtk_button_new_with_label("Cancel");
        gtk_widget_set_halign(btn, GTK_ALIGN_END);
        g_signal_connect_swapped(btn, "clicked", G_CALLBACK(dir_wait_cancel), &w);
        gtk_box_append(GTK_BOX(box), lbl);
        gtk_box_append(GTK_BOX(box), spin);
        gtk_box_append(GTK_BOX(box), btn);
        gtk_window_set_child(GTK_WINDOW(win), box);
        GtkEventController *kc = gtk_event_controller_key_new();
        g_signal_connect(kc, "key-pressed", G_CALLBACK(dir_wait_key), &w);
        gtk_widget_add_controller(win, kc);
        g_signal_connect(win, "close-request", G_CALLBACK(dir_wait_close), &w);
        gtk_window_present(GTK_WINDOW(win));
        gtk_widget_grab_focus(btn);

        g_main_loop_run(w.loop);

        g_signal_handlers_disconnect_by_data(win, &w);
        gtk_window_destroy(GTK_WINDOW(win));
        fm_refocus_if_lost(fm);
        g_mutex_lock(&d->m);
        d->loop = NULL;
        g_mutex_unlock(&d->m);
        g_main_loop_unref(w.loop);
    }

    gpointer result = NULL;
    if (w.cancelled) {
        *cancelled = TRUE;              /* worker frees its result */
        if (cancel) cancel(d->arg);     /* arg lives until the worker ends */
    } else {
        result    = d->result;
        d->result = NULL;
    }
    wait_work_unref(d);
    return result;
}

/* Directory-specific part */
typedef struct { struct stat st; char name[]; } DirEnt;

typedef struct {
    gchar    *path;
    gboolean  hidden;
} DirLoad;

static void dir_load_free(gpointer data)
{
    DirLoad *d = data;
    g_free(d->path);
    g_free(d);
}

/* → GPtrArray of DirEnt*, NULL = opendir failed */
static gpointer dir_load_thread(gpointer data)
{
    DirLoad *d = data;
    GPtrArray *ents = NULL;
    DIR *dir = opendir(d->path);
    if (dir) {
        ents = g_ptr_array_new_with_free_func(g_free);
        int dfd = dirfd(dir);
        struct dirent *de;
        struct stat st;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                continue;
            if (!d->hidden && de->d_name[0] == '.') continue;
            if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
            size_t len = strlen(de->d_name) + 1;
            DirEnt *e = g_malloc(sizeof(DirEnt) + len);
            e->st = st;
            memcpy(e->name, de->d_name, len);
            g_ptr_array_add(ents, e);
        }
        closedir(dir);
    }
    return ents;
}

/* Returns the entries (caller unrefs; NULL = unreadable) or sets
 * *cancelled when the user gave up waiting. */
static GPtrArray *dir_read(FM *fm, const char *path, gboolean *cancelled)
{
    DirLoad *d = g_new0(DirLoad, 1);
    d->path   = g_strdup(path);
    d->hidden = fm->show_hidden;
    return fm_wait_work(fm, "Reading directory", path, dir_load_thread, d,
                        dir_load_free, (GDestroyNotify)g_ptr_array_unref, NULL,
                        cancelled);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Tabs – each tab remembers a local directory and the cursor item;
 *  the panel's widgets are shared, switching simply reloads the panel.
 * ─────────────────────────────────────────────────────────────────── */

static void set_active_panel(FM *fm, int idx);

static void tab_free(gpointer data)
{
    PanelTab *t = data;
    g_free(t->path);
    g_free(t->cursor_name);
    g_free(t->rpath);
    ssh_conn_unref(t->conn);
    g_free(t);
}

static PanelTab *tab_new(const char *path)
{
    PanelTab *t = g_new0(PanelTab, 1);
    t->path = g_strdup(path);
    return t;
}

static PanelTab *tab_cur(Panel *p)
{
    return g_ptr_array_index(p->tabs, p->tab_active);
}

/* The directory the panel is "in": search results count as the
 * directory the search started from (results stay on Alt+Left). */
static const char *tab_dir(Panel *p)
{
    return p->search_mode ? p->search_prev_cwd : p->cwd;
}

static void tabs_rebuild(Panel *p);

static void on_tab_clicked(GtkButton *btn, Panel *p)
{
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "fm-tab-idx"));
    set_active_panel(p->fm, p->idx);
    panel_tab_cycle(p, (int)i - (int)p->tab_active);
    panel_focus(p);
}

static void on_tab_middle(GtkGestureClick *g, int n_press, double x, double y,
                          Panel *p)
{
    (void)n_press; (void)x; (void)y;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "fm-tab-idx"));
    set_active_panel(p->fm, p->idx);
    if (i != p->tab_active) panel_tab_cycle(p, (int)i - (int)p->tab_active);
    if (i == p->tab_active) panel_tab_close(p);
}

/* Tabs are written to settings shortly after every change (not only
 * on exit), so they survive a crash or a killed session too. */
static guint tabs_save_id;

static gboolean tabs_save_cb(gpointer fm)
{
    tabs_save_id = 0;
    settings_save_tabs(fm);
    return G_SOURCE_REMOVE;
}

/* Recreate the tab buttons; the strip is hidden with a single tab */
static void tabs_rebuild(Panel *p)
{
    if (!p->tab_box) return;
    if (tabs_save_id) g_source_remove(tabs_save_id);
    tabs_save_id = g_timeout_add_seconds(1, tabs_save_cb, p->fm);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(p->tab_box)))
        gtk_box_remove(GTK_BOX(p->tab_box), c);

    for (guint i = 0; i < p->tabs->len; i++) {
        PanelTab *t = g_ptr_array_index(p->tabs, i);
        const char *path = t->path ? t->path : "/";
        GtkWidget *b;
        if (t->conn) {
            const char *rp = t->rpath ? t->rpath : "/";
            gchar *base = g_path_get_basename(rp);
            gchar *lbl = g_strdup_printf("%s:%s", t->conn->host, base);
            gchar *tip = g_strdup_printf("sftp://%s@%s%s",
                                         t->conn->user, t->conn->host, rp);
            b = gtk_button_new_with_label(lbl);
            gtk_widget_set_tooltip_text(b, tip);
            g_free(base); g_free(lbl); g_free(tip);
        } else {
            gchar *base = g_path_get_basename(path);
            b = gtk_button_new_with_label(base);
            g_free(base);
            gtk_widget_set_tooltip_text(b, path);
        }
        gtk_widget_set_focusable(b, FALSE);
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_add_css_class(b, "fm-tab");
        if (i == p->tab_active) gtk_widget_add_css_class(b, "fm-tab-active");
        g_object_set_data(G_OBJECT(b), "fm-tab-idx", GUINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(on_tab_clicked), p);

        GtkGesture *mc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mc), GDK_BUTTON_MIDDLE);
        g_signal_connect(mc, "pressed", G_CALLBACK(on_tab_middle), p);
        gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(mc));

        gtk_box_append(GTK_BOX(p->tab_box), b);
    }
    gtk_widget_set_visible(p->tab_scroll, p->tabs->len > 1);
}

/* Called by panel_load on success: the active tab follows navigation */
static void tab_track(Panel *p)
{
    PanelTab *t = tab_cur(p);
    if (p->ssh_conn) {
        if (g_strcmp0(t->rpath, p->ssh_conn->remote_path) == 0) return;
        g_free(t->rpath);
        t->rpath = g_strdup(p->ssh_conn->remote_path);
        g_clear_pointer(&t->cursor_name, g_free);
        tabs_rebuild(p);
        return;
    }
    if (g_strcmp0(t->path, p->cwd) == 0) return;
    g_free(t->path);
    t->path = g_strdup(p->cwd);
    g_clear_pointer(&t->cursor_name, g_free);
    tabs_rebuild(p);
}

/* Remember the cursor item of the active tab before leaving it */
static void tab_remember(Panel *p)
{
    PanelTab *t = tab_cur(p);
    g_free(t->cursor_name);
    t->cursor_name = p->search_mode ? g_strdup(p->search_prev_name)
                   : p->arc_file    ? g_path_get_basename(p->arc_file)
                                    : panel_cursor_name(p);
    if (p->ssh_conn) {
        g_free(t->rpath);
        t->rpath = g_strdup(p->ssh_conn->remote_path);
        return;                 /* t->path keeps the local fallback */
    }
    g_free(t->path);
    t->path = g_strdup(tab_dir(p));
}

void panel_tab_track(Panel *p)
{
    if (p->tabs && p->tabs->len) tab_track(p);
}

/* Make conn (may be NULL) the connection of the active tab; the panel
 * follows.  NULL puts the tab back on its local directory. */
void panel_tab_set_conn(Panel *p, SshConn *conn)
{
    PanelTab *t = tab_cur(p);
    panel_monitor_stop(p);
    SshConn *old = t->conn;
    t->conn = ssh_conn_ref(conn);
    p->ssh_conn = conn;
    ssh_conn_unref(old);
    g_clear_pointer(&t->rpath, g_free);
    gtk_widget_set_visible(p->disconnect_btn, conn != NULL);
    if (!conn) panel_load(p, t->path ? t->path : g_get_home_dir());
    tabs_rebuild(p);
}

/* Disconnect: all tabs using conn become local again */
void panel_tabs_drop_conn(Panel *p, SshConn *conn)
{
    panel_monitor_stop(p);
    gboolean active = (p->ssh_conn == conn);
    if (active) p->ssh_conn = NULL;
    ssh_conn_ref(conn);             /* keep it alive while unhooking */
    for (guint i = 0; i < p->tabs->len; i++) {
        PanelTab *t = g_ptr_array_index(p->tabs, i);
        if (t->conn != conn) continue;
        ssh_conn_unref(t->conn);
        t->conn = NULL;
        g_clear_pointer(&t->rpath, g_free);
        g_clear_pointer(&t->cursor_name, g_free);
    }
    ssh_conn_unref(conn);           /* closes the connection */
    if (active) {
        gtk_widget_set_visible(p->disconnect_btn, FALSE);
        PanelTab *t = tab_cur(p);
        panel_load(p, t->path ? t->path : g_get_home_dir());
    }
    tabs_rebuild(p);
}

/* Show tab i: load its directory and put the cursor back */
static void tab_show(Panel *p, guint i)
{
    p->tab_active = i;
    PanelTab *t = tab_cur(p);
    gchar *name = g_strdup(t->cursor_name);

    /* the panel takes the connection of the tab (or none) */
    ssh_connect_cancel(p);
    panel_monitor_stop(p);
    if (p->ssh_conn != t->conn && p->marks)
        g_hash_table_remove_all(p->marks);
    p->ssh_conn = t->conn;
    gtk_widget_set_visible(p->disconnect_btn, t->conn != NULL);

    if (t->conn) {
        gchar *rpath = g_strdup(t->rpath ? t->rpath : "/");
        g_clear_pointer(&t->conn->remote_path, g_free);
        panel_load_remote(p, rpath);
        if (!t->conn->remote_path) {
            /* connection gone or directory unreadable */
            fm_status(p->fm, "SFTP: %s is not reachable – tab disconnected",
                      t->conn->host);
            g_free(rpath);
            g_free(name);
            panel_tabs_drop_conn(p, t->conn);
            return;
        }
        if (name && strcmp(t->conn->remote_path, rpath) == 0)
            panel_select_name(p, name);
        g_free(rpath);
        g_free(name);
        tabs_rebuild(p);
        return;
    }

    gchar *path = g_strdup(t->path ? t->path : g_get_home_dir());
    panel_load(p, path);
    if (strcmp(p->cwd, path) != 0 || p->search_mode) {
        /* directory gone (or load cancelled): the tab takes what is shown */
        g_free(t->path);
        t->path = g_strdup(tab_dir(p));
    } else if (name) {
        panel_select_name(p, name);
    }
    g_free(path);
    g_free(name);
    tabs_rebuild(p);
}

static gboolean tabs_refused(Panel *p)
{
    return p->busy;
}

/* Ctrl+T – new tab after the active one (path NULL = current directory).
 * On an SFTP panel the new tab shares the connection and path is remote. */
void panel_tab_new(Panel *p, const char *path)
{
    if (tabs_refused(p)) return;
    PanelTab *nt;
    if (p->ssh_conn) {
        nt = tab_new(tab_cur(p)->path ? tab_cur(p)->path : g_get_home_dir());
        nt->conn  = ssh_conn_ref(p->ssh_conn);
        nt->rpath = g_strdup(path ? path : p->ssh_conn->remote_path);
        tab_remember(p);
    } else {
        gchar *dir = g_strdup(path ? path : tab_dir(p));
        tab_remember(p);
        nt = tab_new(dir);
        g_free(dir);
    }
    g_ptr_array_insert(p->tabs, (int)p->tab_active + 1, nt);
    tab_show(p, p->tab_active + 1);
}

/* Ctrl+W – close the active tab (never the last one) */
void panel_tab_close(Panel *p)
{
    if (tabs_refused(p)) return;
    if (p->tabs->len < 2) { fm_status(p->fm, "Cannot close the last tab"); return; }
    /* the panel must not keep using a connection the tab may close */
    panel_monitor_stop(p);
    p->ssh_conn = NULL;
    g_ptr_array_remove_index(p->tabs, p->tab_active);
    tab_show(p, MIN(p->tab_active, p->tabs->len - 1));
}

/* Ctrl+Tab / Ctrl+Shift+Tab – move delta tabs, wrapping around */
void panel_tab_cycle(Panel *p, int delta)
{
    if (p->tabs->len < 2 || delta == 0) return;
    if (tabs_refused(p)) return;
    int n = (int)p->tabs->len;
    int i = (((int)p->tab_active + delta) % n + n) % n;
    tab_remember(p);
    tab_show(p, (guint)i);
}

/* Startup: tabs from settings (missing directories skipped), then the
 * active one is loaded.  Always leaves at least one tab. */
void panel_tabs_restore(Panel *p, gchar **paths, int active)
{
    g_ptr_array_set_size(p->tabs, 0);
    int skipped_before = 0;
    for (int i = 0; paths && paths[i]; i++) {
        if (paths[i][0] == '/' && g_file_test(paths[i], G_FILE_TEST_IS_DIR))
            g_ptr_array_add(p->tabs, tab_new(paths[i]));
        else if (i < active)
            skipped_before++;
    }
    if (p->tabs->len == 0) {
        g_ptr_array_add(p->tabs, tab_new(g_get_home_dir()));
        active = 0;
    }
    active -= skipped_before;
    if (active < 0 || active >= (int)p->tabs->len) active = 0;
    tab_show(p, (guint)active);
}

void panel_tabs_free(Panel *p)
{
    panel_monitor_stop(p);
    p->ssh_conn = NULL;
    g_clear_pointer(&p->tabs, g_ptr_array_unref);
}

void panel_load(Panel *p, const char *path)
{
    if (p->ssh_conn) { panel_load_remote(p, path); return; }

    if (p->busy) return;            /* a search is filling this panel */
    panel_scroll_remember(p);
    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    if (strlen(path) >= sizeof(p->cwd)) {
        fm_status(p->fm, "Path too long: %.60s...", path);
        return;
    }

    /* path may point into p->cwd – keep a copy across the wait */
    gchar *want = g_strdup(path);
    gboolean cancelled;
    panel_monitor_stop(p);          /* no reloads from events while waiting */
    GPtrArray *ents = dir_read(p->fm, want, &cancelled);
    if (cancelled) {
        fm_status(p->fm, "Reading the directory was cancelled");
        panel_monitor_start(p);     /* stay in the old directory */
        g_free(want);
        return;
    }
    path = want;

    /* Leaving an archive (any real directory load ends archive mode) */
    arc_panel_leave(p);

    /* Leaving search results: they stay reachable via Alt+Left */
    gboolean was_search = p->search_mode;
    search_detach(p);

    /* Clear marks when entering a different directory */
    if (strcmp(p->cwd, path) != 0 && p->marks)
        g_hash_table_remove_all(p->marks);

    /* Suppress selection events during entire load */
    p->inhibit_sel = TRUE;

    g_strlcpy(p->cwd, path, sizeof(p->cwd));
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), path);

    /* Collect all items into a GPtrArray first, then splice once.
     * This emits a single items-changed on the store so GtkSortListModel
     * sorts only once (O(n log n)) instead of once per append (O(n² log n)). */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    gchar *vis_dir_color = fm_visible_color(p->fm, p->fm->dir_color);

    /* ".." entry always first */
    g_ptr_array_add(items,
        file_item_new("go-up-symbolic", "..", "", "",
                      TRUE, vis_dir_color, p->fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                      (gint64)-2, (gint64)0));

    for (guint i = 0; ents && i < ents->len; i++) {
        DirEnt *e = g_ptr_array_index(ents, i);
        FileItem *fi = file_item_from_stat(p->fm, e->name, &e->st);
        if (p->marks && g_hash_table_contains(p->marks, e->name))
            fi->marked = TRUE;
        g_ptr_array_add(items, fi);
    }
    if (ents) g_ptr_array_unref(ents);
    g_free(vis_dir_color);

    /* Replace store: avoids expensive remove_all signal on large lists.
     * Create fresh store, splice items in, then swap into sort model. */
    GListStore *new_store = g_list_store_new(FILE_ITEM_TYPE);
    g_list_store_splice(new_store, 0, 0,
                        (gpointer *)items->pdata, items->len);
    g_ptr_array_unref(items);
    /* swapping the store destroys the focused row – remember whether the
     * keyboard cursor was in this panel and put it back afterwards */
    GtkWidget *foc = gtk_window_get_focus(GTK_WINDOW(p->fm->window));
    gboolean had_focus = !foc || foc == p->column_view ||
                         gtk_widget_is_ancestor(foc, p->column_view);
    had_focus = had_focus && p->fm->active == p->idx;
    gtk_sort_list_model_set_model(p->sort_model, G_LIST_MODEL(new_store));
    g_object_unref(p->store);   /* old store freed (items unreffed) */
    p->store = new_store;
    if (was_search)             /* results are unsorted; restore after swap */
        gtk_sort_list_model_set_sorter(p->sort_model, p->default_sorter);
    search_update_buttons(p);

    panel_update_status(p);

    /* move cursor to first row */
    p->cursor_pos = 0;
    if (p->column_view && g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0) {
        /* Only select (show cursor) on active panel */
        gboolean is_active = (p->fm->active == p->idx);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL,
                                  is_active ? GTK_LIST_SCROLL_SELECT
                                            : GTK_LIST_SCROLL_NONE,
                                  NULL);
    }

    p->inhibit_sel = FALSE;

    /* Start directory monitor if auto-refresh is enabled */
    panel_monitor_start(p);
    tab_track(p);
    if (had_focus) panel_focus(p);
    g_free(want);
}

void panel_reload(Panel *p)
{
    if (p->ssh_conn) {
        panel_load_remote(p, p->ssh_conn->remote_path);
        return;
    }
    if (p->arc_file) {          /* inside an archive: re-list, keep the place */
        if (!p->busy) arc_panel_reload(p);
        return;
    }
    /* Results are not a directory: reloading p->cwd would drop them */
    if (p->search_mode || p->busy) return;

    gchar *saved = panel_cursor_name(p);
    panel_load(p, p->cwd);  /* inhibit_sel is handled inside panel_load */

    if (!saved) return;

    /* Only select (show cursor) on active panel */
    gboolean is_active = (p->fm->active == p->idx);

    p->inhibit_sel = TRUE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (item && strcmp(item->name, saved) == 0) {
            g_object_unref(item);
            p->cursor_pos = i;
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                      NULL,
                                      is_active ? GTK_LIST_SCROLL_SELECT
                                                : GTK_LIST_SCROLL_NONE,
                                      NULL);
            break;
        }
        if (item) g_object_unref(item);
    }
    p->inhibit_sel = FALSE;
    g_free(saved);
}

void panel_go_up(Panel *p)
{
    if (p->ssh_conn) {
        if (strcmp(p->ssh_conn->remote_path, "/") == 0) return;
        gchar *parent   = g_path_get_dirname(p->ssh_conn->remote_path);
        gchar *basename = g_path_get_basename(p->ssh_conn->remote_path);
        panel_load_remote(p, parent);
        /* restore cursor */
        guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
        for (guint i = 0; i < n; i++) {
            FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
            if (item && strcmp(item->name, basename) == 0) {
                g_object_unref(item);
                p->cursor_pos = i;
                p->inhibit_sel = TRUE;
                gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                          NULL, GTK_LIST_SCROLL_SELECT, NULL);
                p->inhibit_sel = FALSE;
                break;
            }
            if (item) g_object_unref(item);
        }
        panel_scroll_restore(p);
        g_free(parent); g_free(basename);
        return;
    }

    if (p->arc_file) { arc_panel_go_up(p); return; }

    if (p->search_mode) {
        gchar *name = g_strdup(p->search_prev_name);
        panel_load(p, p->search_prev_cwd);  /* results kept for Alt+Left */
        if (!p->search_mode && name)        /* back on the item from before */
            panel_select_name(p, name);
        g_free(name);
        return;
    }
    if (strcmp(p->cwd, "/") == 0) return;
    gchar *parent   = g_path_get_dirname(p->cwd);
    gchar *basename = g_path_get_basename(p->cwd);
    panel_load(p, parent);

    /* restore cursor to dir we came from */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (item && strcmp(item->name, basename) == 0) {
            g_object_unref(item);
            p->cursor_pos = i;
            p->inhibit_sel = TRUE;
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                      NULL, GTK_LIST_SCROLL_SELECT, NULL);
            p->inhibit_sel = FALSE;
            break;
        }
        if (item) g_object_unref(item);
    }
    panel_scroll_restore(p);
    g_free(parent);
    g_free(basename);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Sort comparators
 * ─────────────────────────────────────────────────────────────────── */

/* Directories-first sorter – used as primary in GtkMultiSorter so it is
 * never affected by the column view's ascending/descending toggle. */
static int dirs_first_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    if (fa->size_raw == -2) return -1;      /* ".." entry */
    if (fb->size_raw == -2) return  1;
    if (fa->is_dir && !fb->is_dir)  return -1;
    if (!fa->is_dir && fb->is_dir)  return  1;
    return 0;
}

static int name_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    return strcmp(file_item_sort_key(fa), file_item_sort_key(fb));
}

static int size_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    return (fa->size_raw < fb->size_raw) ? -1 : (fa->size_raw > fb->size_raw) ? 1 : 0;
}

static int date_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    return (fa->date_raw < fb->date_raw) ? -1 : (fa->date_raw > fb->date_raw) ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Column factories
 * ─────────────────────────────────────────────────────────────────── */

/* Name column: icon + label */
static void name_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *image  = gtk_image_new();
    GtkWidget *label  = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(li, box);
}

static void name_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f;
    Panel     *p     = d;
    GtkWidget *box   = gtk_list_item_get_child(li);
    GtkWidget *image = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(image);
    FileItem  *item  = gtk_list_item_get_item(li);

    /* lets the context-menu gesture find the row under the pointer */
    g_object_set_data(G_OBJECT(box), "fm-item", item);

    int isz = p->fm->icon_size > 0 ? p->fm->icon_size : 16;
    /* ".." is just the name – no icon, but the space stays for alignment */
    if (strcmp(item->name, "..") == 0)
        gtk_image_clear(GTK_IMAGE(image));
    else
        gtk_image_set_from_icon_name(GTK_IMAGE(image), item->icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(image), isz);
    gtk_widget_set_size_request(image, isz, isz);

    const gchar *fg = item->marked ? p->fm->vis_mark_color : item->fg_color;
    gint wt = item->marked ? PANGO_WEIGHT_BOLD : item->weight;
    if (fg) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground='%s' weight='%d'>%s</span>",
            fg, wt, item->name);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), item->name);
    }

    /* content search: ":line  matching text" after the name */
    if (item->snippet) {
        gchar *head = fg ? g_markup_printf_escaped(
                               "<span foreground='%s' weight='%d'>%s</span>",
                               fg, wt, item->name)
                         : g_markup_escape_text(item->name, -1);
        gchar *tail = g_markup_printf_escaped("  <span alpha='55%%'>:%d  %s</span>",
                                              item->line, item->snippet);
        gchar *mk = g_strconcat(head, tail, NULL);
        gtk_label_set_markup(GTK_LABEL(label), mk);
        g_free(mk); g_free(head); g_free(tail);
    }
}

/* Size column */
static void size_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_item_set_child(li, label);
}

static void size_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f;
    Panel     *p     = d;
    GtkWidget *label = gtk_list_item_get_child(li);
    FileItem  *item  = gtk_list_item_get_item(li);

    const gchar *fg = item->marked ? p->fm->vis_mark_color : item->fg_color;
    if (fg) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground='%s'>%s</span>", fg, item->size);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), item->size);
    }
}

/* Date column */
static void date_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_item_set_child(li, label);
}

static void date_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f;
    Panel     *p     = d;
    GtkWidget *label = gtk_list_item_get_child(li);
    FileItem  *item  = gtk_list_item_get_item(li);

    /* In search mode, show directory path instead of date */
    const gchar *text = (p->search_mode && item->dir_path) ? item->dir_path : item->date;

    const gchar *fg = item->marked ? p->fm->vis_mark_color : item->fg_color;
    if (fg) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground='%s'>%s</span>", fg, text);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), text);
    }
}

/* Marking re-renders a bound row in place (FileItem "mark-changed")
 * instead of touching the model, which would re-sort the whole panel. */
typedef void (*BindFn)(GtkSignalListItemFactory *, GtkListItem *, gpointer);

static void on_item_mark_changed(FileItem *item, GtkListItem *li)
{
    (void)item;
    BindFn fn = g_object_get_data(G_OBJECT(li), "fm-bind");
    fn(NULL, li, g_object_get_data(G_OBJECT(li), "fm-panel"));
}

static void watch_unbind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    /* disconnect from the item the handler was attached to – on a rebind
     * without unbind gtk_list_item_get_item() already returns the new one */
    gulong hid = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(li), "fm-hid"));
    GObject *item = g_object_get_data(G_OBJECT(li), "fm-hitem");
    if (hid && item) g_signal_handler_disconnect(item, hid);
    g_object_set_data(G_OBJECT(li), "fm-hid", NULL);
    g_object_set_data(G_OBJECT(li), "fm-hitem", NULL);
}

static void watch_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    g_object_set_data(G_OBJECT(li), "fm-bind", g_object_get_data(G_OBJECT(f), "fm-bind"));
    g_object_set_data(G_OBJECT(li), "fm-panel", d);
    watch_unbind(f, li, d);         /* rebind without unbind: drop the old one */
    gpointer item = gtk_list_item_get_item(li);
    gulong hid = g_signal_connect(item, "mark-changed",
                                  G_CALLBACK(on_item_mark_changed), li);
    g_object_set_data(G_OBJECT(li), "fm-hid", GSIZE_TO_POINTER(hid));
    g_object_set_data(G_OBJECT(li), "fm-hitem", item);
}

static void factory_watch_marks(GtkListItemFactory *f, BindFn fn, Panel *p)
{
    g_object_set_data(G_OBJECT(f), "fm-bind", fn);
    g_signal_connect(f, "bind",   G_CALLBACK(watch_bind),   p);
    g_signal_connect(f, "unbind", G_CALLBACK(watch_unbind), p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Callbacks – column view
 * ─────────────────────────────────────────────────────────────────── */

#ifdef HAVE_LIBSSH2
/* Temp dirs holding downloaded remote files; the external app may still
 * read them, so they go when fm exits (see main). */
static GSList *view_tmpdirs;
#endif

static void view_tmpdirs_cleanup(void)
{
#ifdef HAVE_LIBSSH2
    for (GSList *l = view_tmpdirs; l; l = l->next) {
        GDir *d = g_dir_open(l->data, 0, NULL);
        const char *e;
        while (d && (e = g_dir_read_name(d))) {
            gchar *f = g_build_filename(l->data, e, NULL);
            g_unlink(f);
            g_free(f);
        }
        if (d) g_dir_close(d);
        g_rmdir(l->data);
    }
    g_slist_free_full(view_tmpdirs, g_free);
    view_tmpdirs = NULL;
#endif
}

static void activate_item(Panel *p)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (p->cursor_pos >= n) return;

    FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                           p->cursor_pos);
    if (!item) return;

    gchar    *name   = g_strdup(item->name);
    gboolean  is_dir = item->is_dir;
    g_object_unref(item);

    if (p->ssh_conn) {
        if (strcmp(name, "..") == 0) {
            panel_go_up(p);
        } else if (is_dir) {
            gchar *np = fs_remote_join(p->ssh_conn->remote_path, name);
            panel_load_remote(p, np);
            g_free(np);
        } else {
#ifdef HAVE_LIBSSH2
            /* No main-loop pump here: re-entering would let the panel
             * change under us while the (blocking) download runs. */
            fm_status(p->fm, "Downloading '%s'...", name);
            gchar *remote_path = fs_remote_join(p->ssh_conn->remote_path, name);
            gsize len = 0;
            gchar *data = ssh_read_file(p->ssh_conn, remote_path, &len);
            g_free(remote_path);
            if (data && len > 0) {
                /* One private temp dir per session (prevents symlink
                 * attacks); reopening a file replaces its earlier copy */
                if (!view_tmpdirs) {
                    gchar *d = g_dir_make_tmp("fm-view-XXXXXX", NULL);
                    if (d) view_tmpdirs = g_slist_prepend(NULL, d);
                }
                const gchar *tmpdir = view_tmpdirs ? view_tmpdirs->data : NULL;
                if (tmpdir) {
                    gchar *tmp = g_build_filename(tmpdir, name, NULL);
                    if (g_file_set_contents(tmp, data, (gssize)len, NULL)) {
                        gchar *uri = g_filename_to_uri(tmp, NULL, NULL);
                        if (uri) {
                            gtk_show_uri(GTK_WINDOW(p->fm->window),
                                uri, GDK_CURRENT_TIME);
                            g_free(uri);
                        }
                        fm_status(p->fm, "Opened: %s", name);
                    } else {
                        g_unlink(tmp);
                        fm_status(p->fm, "Error writing to temp directory");
                    }
                    g_free(tmp);
                } else {
                    fm_status(p->fm, "Error creating temp directory");
                }
            } else {
                fm_status(p->fm, "Error downloading '%s'", name);
            }
            g_free(data);
#else
            fm_status(p->fm, "Opening remote files is not supported");
#endif
        }
        g_free(name);
        return;
    }

    if (p->arc_file) {
        arc_panel_activate(p);
        g_free(name);
        return;
    }

    if (p->search_mode) {
        /* Results stay: a file opens in its default application,
         * a directory header jumps there (Alt+Left comes back). */
        FileItem *si = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                             p->cursor_pos);
        gboolean is_file = si && si->dir_path;
        if (si) g_object_unref(si);
        if (is_file) {
            gchar *full = panel_cursor_fullpath(p);
            if (arc_panel_enter(p, full)) {     /* a found archive: browse it */
                g_free(full);
                g_free(name);
                return;
            }
            gchar *uri = full ? g_filename_to_uri(full, NULL, NULL) : NULL;
            if (uri) {
                gtk_show_uri(GTK_WINDOW(p->fm->window), uri, GDK_CURRENT_TIME);
                fm_status(p->fm, "Opened: %s", full);
            }
            g_free(uri);
            g_free(full);
        } else {
            search_goto_file(p);
        }
        g_free(name);
        return;
    }

    if (strcmp(name, "..") == 0) {
        panel_go_up(p);
    } else if (is_dir) {
        gchar *newpath = g_build_filename(p->cwd, name, NULL);
        panel_load(p, newpath);
        g_free(newpath);
    } else {
        gchar *fullpath = g_build_filename(p->cwd, name, NULL);
        /* archives open as directories; unreadable ones as before */
        gchar *uri = arc_panel_enter(p, fullpath) ? NULL
                   : g_filename_to_uri(fullpath, NULL, NULL);
        if (uri) {
            gtk_show_uri(GTK_WINDOW(p->fm->window), uri, GDK_CURRENT_TIME);
            g_free(uri);
        }
        g_free(fullpath);
    }
    g_free(name);
}

static void on_cv_activate(GtkColumnView *cv, guint pos, Panel *p)
{
    (void)cv;
    p->cursor_pos = pos;
    activate_item(p);
}

/* Sync cursor_pos from the actual GTK selection.
 * Mouse clicks update GTK's selection but on_selection_changed may not
 * always fire (e.g. after SFTP panel_load_remote replaces store contents).
 * Call this before any keyboard navigation. */
static void sync_cursor_from_selection(Panel *p)
{
    GtkBitset *bs = gtk_selection_model_get_selection(
                        GTK_SELECTION_MODEL(p->selection));
    if (gtk_bitset_get_size(bs) == 1) {
        GtkBitsetIter iter;
        guint pos;
        if (gtk_bitset_iter_init_first(&iter, bs, &pos))
            p->cursor_pos = pos;
    }
    gtk_bitset_unref(bs);
}

static gboolean on_tree_key_press(GtkEventControllerKey *ctrl,
                                   guint keyval, guint keycode,
                                   GdkModifierType state, Panel *p)
{
    (void)ctrl; (void)keycode;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (n == 0) return FALSE;

    /* search results: Ctrl+PgDn / Ctrl+Enter jump to the file's directory */
    if (p->search_mode && (state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_Return ||
         keyval == GDK_KEY_KP_Enter)) {
        sync_cursor_from_selection(p);
        search_goto_file(p);
        return TRUE;
    }

    /* Ctrl+PgDn on an archive – browse it (Total Commander) */
    if (!p->search_mode && !p->arc_file && (state & GDK_CONTROL_MASK) &&
        keyval == GDK_KEY_Page_Down) {
        sync_cursor_from_selection(p);
        gchar *full = p->ssh_conn ? NULL : panel_cursor_fullpath(p);
        gboolean in = arc_panel_enter(p, full);
        g_free(full);
        if (in) return TRUE;
    }

    switch (keyval) {

    case GDK_KEY_Up:
        sync_cursor_from_selection(p);
        if (state & GDK_SHIFT_MASK) sel_toggle_cursor(p, FALSE);
        if (p->cursor_pos > 0) p->cursor_pos--;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Down:
        sync_cursor_from_selection(p);
        if (state & GDK_SHIFT_MASK) sel_toggle_cursor(p, FALSE);
        if (p->cursor_pos < n - 1) p->cursor_pos++;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Page_Up:
        sync_cursor_from_selection(p);
        p->cursor_pos = (p->cursor_pos > 20) ? p->cursor_pos - 20 : 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Page_Down:
        sync_cursor_from_selection(p);
        p->cursor_pos = MIN(p->cursor_pos + 20, n - 1);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Home:
        p->cursor_pos = 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            0, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_End:
        p->cursor_pos = n > 0 ? n - 1 : 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_space:
    case GDK_KEY_Insert:
        sync_cursor_from_selection(p);
        sel_toggle_cursor(p, TRUE);
        return TRUE;

    case GDK_KEY_BackSpace:
        sync_cursor_from_selection(p);
        panel_go_up(p);
        return TRUE;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        sync_cursor_from_selection(p);
        activate_item(p);
        return TRUE;

    /* TC-style group marking */
    case GDK_KEY_plus:
    case GDK_KEY_KP_Add:
        sel_group_dialog(p->fm, TRUE);
        return TRUE;
    case GDK_KEY_minus:
    case GDK_KEY_KP_Subtract:
        sel_group_dialog(p->fm, FALSE);
        return TRUE;
    case GDK_KEY_asterisk:
    case GDK_KEY_KP_Multiply:
        sel_invert(p);
        return TRUE;

    default:
        break;
    }
    return FALSE;
}

/* Helper: update active-panel CSS on both path entries */
static void set_active_panel(FM *fm, int idx)
{
    fm->active = idx;
    for (int i = 0; i < 2; i++) {
        Panel *pp = &fm->panels[i];
        if (i == idx) {
            gtk_widget_add_css_class(pp->path_entry, "active-panel");
            gtk_widget_remove_css_class(pp->path_entry, "inactive-panel");
        } else {
            gtk_widget_remove_css_class(pp->path_entry, "active-panel");
            gtk_widget_add_css_class(pp->path_entry, "inactive-panel");
        }
    }
}

/* Make p the active panel and put the keyboard cursor on it.
 *
 * Everything that sends the cursor back into a panel (Tab, after a
 * dialog or a job, after a reload) goes through here.  It does not rely
 * on the focus "enter" signal: when the focus is already inside the
 * panel, or was lost because a reload replaced the focused row, that
 * signal never comes – fm->active then disagreed with the real focus
 * and inhibit_sel stayed TRUE, so Tab kept jumping into the same panel
 * and clicks were ignored. */
void panel_focus(Panel *p)
{
    FM *fm = p->fm;
    Panel *o = &fm->panels[p->idx ^ 1];

    set_active_panel(fm, p->idx);

    o->inhibit_sel = TRUE;                      /* hide the other cursor */
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(o->selection));
    o->inhibit_sel = FALSE;

    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (n > 0) {
        if (p->cursor_pos >= n) p->cursor_pos = n - 1;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), p->cursor_pos,
                                  NULL, GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS,
                                  NULL);
    }
    p->inhibit_sel = FALSE;
}

/* After a modal window (job progress, "Reading directory") closes the
 * main window may get no focused widget back.  Checked from an idle
 * callback, because the focus returns asynchronously. */
static gboolean refocus_idle(gpointer data)
{
    FM *fm = data;
    if (!fm->window) return G_SOURCE_REMOVE;
    GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(fm->window));
    gboolean ok = f && (GTK_IS_EDITABLE(f) || GTK_IS_TEXT_VIEW(f));
    for (int i = 0; i < 2 && !ok; i++)
        ok = f && (f == fm->panels[i].column_view ||
                   gtk_widget_is_ancestor(f, fm->panels[i].column_view));
    if (!ok) panel_focus(active_panel(fm));
    return G_SOURCE_REMOVE;
}

void fm_refocus_if_lost(FM *fm)
{
    g_idle_add(refocus_idle, fm);
}

/* Selection changed: fired on mouse click (and our own scroll_to).
 * When exactly ONE item ends up selected, treat that as the new cursor. */
static void on_selection_changed(GtkSelectionModel *model, guint position,
                                  guint n_items, Panel *p)
{
    (void)position; (void)n_items;

    if (p->inhibit_sel) return;

    GtkBitset *bs = gtk_selection_model_get_selection(model);
    guint64 n_sel = gtk_bitset_get_size(bs);

    if (n_sel == 1) {
        GtkBitsetIter iter;
        guint pos;
        if (gtk_bitset_iter_init_first(&iter, bs, &pos)) {
            p->cursor_pos = pos;
            set_active_panel(p->fm, p->idx);
        }
    }
    gtk_bitset_unref(bs);
}

static void on_focus_leave(GtkEventControllerFocus *ctrl, Panel *p)
{
    (void)ctrl;
    p->inhibit_sel = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->selection));
    p->inhibit_sel = FALSE;
}

static void on_focus_enter(GtkEventControllerFocus *ctrl, Panel *p)
{
    (void)ctrl;
    set_active_panel(p->fm, p->idx);

    /* inhibit_sel is pre-set to TRUE only on Tab-switch (keyboard).
     * For mouse clicks it is FALSE — the click's on_selection_changed
     * will set the cursor, so don't restore the old position here. */
    if (!p->inhibit_sel) return;

    /* Keyboard focus: restore saved cursor position. */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (n > 0) {
        if (p->cursor_pos >= n) p->cursor_pos = 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
                                  p->cursor_pos, NULL,
                                  GTK_LIST_SCROLL_SELECT, NULL);
    }
    p->inhibit_sel = FALSE;
}

/* Put the shown location back into the path entry */
static void panel_entry_reset(Panel *p)
{
    gchar *shown = p->arc_file ? g_build_filename(p->arc_file, p->arc_dir, NULL)
                               : g_strdup(p->cwd);
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), shown);
    g_free(shown);
}

static void do_path_activate(Panel *p)
{
    /* IMPORTANT: copy text because gtk_editable_get_text returns internal buffer
     * that becomes invalid when we modify the entry */
    gchar *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->path_entry)));

    if (!text || !text[0]) {
        g_free(text);
        gtk_editable_set_text(GTK_EDITABLE(p->path_entry), p->cwd);
        return;
    }

    if (p->ssh_conn) {
        /* SSH connected – parse path from entry or use as-is */
        const char *remote_path = text;

        /* If user typed sftp://user@host/path, extract just the path */
        if (g_str_has_prefix(text, "sftp://")) {
            const char *host_start = text + 7;
            const char *path_start = strchr(host_start, '/');
            if (path_start)
                remote_path = path_start;
            else
                remote_path = "/";
        }

        /* Handle relative paths for SSH */
        gchar *full_path = NULL;
        if (remote_path[0] != '/') {
            /* Relative path – prepend current remote directory */
            const char *base = p->ssh_conn->remote_path;
            if (base[strlen(base)-1] == '/')
                full_path = g_strdup_printf("%s%s", base, remote_path);
            else
                full_path = g_strdup_printf("%s/%s", base, remote_path);
            remote_path = full_path;
        }

        panel_load_remote(p, remote_path);
        g_free(full_path);
    } else {
        /* Local panel – build full path */
        gchar *full_path = NULL;

        /* Handle ~ expansion */
        if (text[0] == '~') {
            if (text[1] == '\0' || text[1] == '/')
                full_path = g_strdup_printf("%s%s", g_get_home_dir(), text + 1);
            else
                full_path = g_strdup(text);  /* ~user not supported, try as-is */
        }
        /* Handle relative paths */
        else if (text[0] != '/') {
            if (p->cwd[strlen(p->cwd)-1] == '/')
                full_path = g_strdup_printf("%s%s", p->cwd, text);
            else
                full_path = g_strdup_printf("%s/%s", p->cwd, text);
        }

        const char *path_to_check = full_path ? full_path : text;
        struct stat st;
        if (stat(path_to_check, &st) == 0 && S_ISDIR(st.st_mode))
            panel_load(p, path_to_check);
        else if (!arc_panel_enter(p, path_to_check))
            panel_entry_reset(p);

        g_free(full_path);
    }
    g_free(text);
}

static gboolean on_path_key_press(GtkEventControllerKey *ctrl,
                                   guint keyval, guint keycode,
                                   GdkModifierType state, Panel *p)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        do_path_activate(p);
        panel_focus(p);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        panel_entry_reset(p);
        panel_focus(p);
        return TRUE;
    }
    return FALSE;
}

static void on_path_activate(GtkEntry *entry, Panel *p)
{
    (void)entry;
    do_path_activate(p);
}

static void on_up_clicked(GtkButton *btn, Panel *p) { (void)btn; panel_go_up(p); }

static void on_disconnect_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    ssh_panel_close(p);
}

static void on_search_exit_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    if (p->search_mode) {
        panel_load(p, p->search_prev_cwd);
        panel_focus(p);
    }
}

static void on_results_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    set_active_panel(p->fm, p->idx);
    search_restore(p);
}

static void on_terminal_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    const char *term = p->fm->terminal_app;
    if (!term || !term[0]) term = "gnome-terminal";

    if (p->ssh_conn) {
        const char *rpath = p->ssh_conn->remote_path;

        /* Shell-quote remote path to prevent command injection */
        gchar *quoted_path = g_shell_quote(rpath);
        gchar *quoted_user = g_shell_quote(p->ssh_conn->user);
        gchar *quoted_host = g_shell_quote(p->ssh_conn->host);
        gchar *sh_cmd = (p->ssh_conn->port != 22)
            ? g_strdup_printf("ssh -t -p %d %s@%s 'cd %s && exec $SHELL -l'",
                              p->ssh_conn->port,
                              quoted_user, quoted_host, quoted_path)
            : g_strdup_printf("ssh -t %s@%s 'cd %s && exec $SHELL -l'",
                              quoted_user, quoted_host, quoted_path);
        g_free(quoted_path); g_free(quoted_user); g_free(quoted_host);

        /* Use argv array — no shell parsing, no injection */
        const gchar *argv[7];
        int a = 0;
        argv[a++] = term;
        if (strstr(term, "ptyxis")) {
            argv[a++] = "--new-window";
        }
        argv[a++] = "--";
        argv[a++] = "bash";
        argv[a++] = "-c";
        argv[a++] = sh_cmd;
        argv[a] = NULL;

        GError *err = NULL;
        if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                           G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &err)) {
            fm_status(p->fm, "Failed to open terminal: %s",
                      err ? err->message : "?");
            g_clear_error(&err);
        }
        g_free(sh_cmd);
        return;
    }

    /* Local terminal — use g_spawn_async with argv to prevent injection */
    {
        const gchar *argv[5];
        int a = 0;
        argv[a++] = term;
        if (strstr(term, "ptyxis")) {
            argv[a++] = "--new-window";
            argv[a++] = "-d";
            argv[a++] = p->cwd;
        } else if (strstr(term, "gnome-terminal") || strstr(term, "mate-terminal")) {
            argv[a++] = "--working-directory";
            argv[a++] = p->cwd;
        } else if (strstr(term, "konsole")) {
            argv[a++] = "--workdir";
            argv[a++] = p->cwd;
        } else if (strstr(term, "xfce4-terminal")) {
            argv[a++] = "--default-working-directory";
            argv[a++] = p->cwd;
        }
        argv[a] = NULL;

        GError *err = NULL;
        if (!g_spawn_async(p->cwd, (gchar **)argv, NULL,
                           G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &err)) {
            fm_status(p->fm, "Failed to open terminal: %s",
                      err ? err->message : "?");
            g_clear_error(&err);
        }
    }
}

static void on_home_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    panel_load(p, g_get_home_dir());
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel filter (Ctrl+S)
 * ─────────────────────────────────────────────────────────────────── */

static gboolean filter_match_func(gpointer item, gpointer user_data)
{
    Panel *p = user_data;
    FileItem *fi = FILE_ITEM(item);
    if (strcmp(fi->name, "..") == 0) return TRUE;

    /* Mask filter (glob, files only, case sensitive) */
    if (p->mask_pattern && p->mask_pattern[0] && !fi->is_dir) {
        if (fnmatch(p->mask_pattern, fi->name, 0) != 0)
            return FALSE;
    }

    return TRUE;
}

static void on_filter_changed(GtkEditable *editable, Panel *p)
{
    const gchar *text = gtk_editable_get_text(editable);
    if (!text || !text[0]) return;

    /* MC-style incremental search: jump cursor to first matching item */
    gchar *lower = g_utf8_strdown(text, -1);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (!item) continue;
        gchar *name_lower = g_utf8_strdown(item->name, -1);
        gboolean match = (strstr(name_lower, lower) != NULL);
        g_free(name_lower);
        g_object_unref(item);
        if (match) {
            p->cursor_pos = i;
            p->inhibit_sel = TRUE;
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                      NULL, GTK_LIST_SCROLL_SELECT, NULL);
            p->inhibit_sel = FALSE;
            break;
        }
    }
    g_free(lower);
}

static gboolean on_filter_key(GtkEventControllerKey *ctrl,
                               guint keyval, guint keycode,
                               GdkModifierType state, Panel *p)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape) {
        /* Close search bar, consume key */
        gtk_widget_set_visible(p->filter_bar, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(p->filter_entry), "");
        panel_focus(p);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        /* Close search bar and activate the item under cursor */
        gtk_widget_set_visible(p->filter_bar, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(p->filter_entry), "");
        panel_focus(p);
        activate_item(p);
        return TRUE;
    }
    if (keyval == GDK_KEY_Up    || keyval == GDK_KEY_Down  ||
        keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_Page_Down ||
        keyval == GDK_KEY_Home  || keyval == GDK_KEY_End) {
        /* Close search bar, cursor stays */
        gtk_widget_set_visible(p->filter_bar, FALSE);
        gtk_editable_set_text(GTK_EDITABLE(p->filter_entry), "");
        panel_focus(p);
        return TRUE;
    }
    return FALSE;
}

static void panel_show_filter(Panel *p)
{
    gtk_widget_set_visible(p->filter_bar, TRUE);
    gtk_widget_grab_focus(p->filter_entry);
}

static void menu_mask(GSimpleAction *a, GVariant *v, gpointer d);
/* Per-panel hamburger menu callbacks */
/* Every panel menu entry makes its panel active, then runs an FM action */
#define PMENU(fn, call) \
    static void fn(GSimpleAction *a, GVariant *v, gpointer d) \
    { (void)a; (void)v; Panel *p = d; set_active_panel(p->fm, p->idx); call; }

PMENU(pmenu_ssh,      ssh_connect_dialog(p->fm))
PMENU(pmenu_pack,     fo_pack(p->fm))
PMENU(pmenu_extract,  fo_extract(p->fm))
PMENU(pmenu_mask,     menu_mask(a, v, p->fm))
PMENU(pmenu_search,   search_run(p->fm))
PMENU(pmenu_filter,   panel_show_filter(p))
PMENU(pmenu_sresults, search_restore(p))
PMENU(pmenu_shistory, search_history_dialog(p->fm))
PMENU(pmenu_copy,     clip_copy(p->fm, FALSE))
PMENU(pmenu_cut,      clip_copy(p->fm, TRUE))
PMENU(pmenu_paste,    clip_paste(p->fm))
PMENU(pmenu_copypath, clip_copy_paths(p->fm))
PMENU(pmenu_dup,      fo_copy_here(p->fm))
PMENU(pmenu_rename,   fo_rename(p->fm))
PMENU(pmenu_mrename,  fo_multi_rename(p->fm))
PMENU(pmenu_delete,   fo_delete(p->fm))
PMENU(pmenu_mkdir,    fo_mkdir(p->fm))
PMENU(pmenu_newfile,  fo_newfile(p->fm))
PMENU(pmenu_props,    fo_properties(p->fm))
PMENU(pmenu_selall,   sel_mark_all(p, TRUE))
PMENU(pmenu_selnone,  sel_mark_all(p, FALSE))
PMENU(pmenu_selgroup, sel_group_dialog(p->fm, TRUE))
PMENU(pmenu_unselgrp, sel_group_dialog(p->fm, FALSE))
PMENU(pmenu_invert,   sel_invert(p))
PMENU(pmenu_tabnew,   panel_tab_new(p, NULL))
PMENU(pmenu_tabclose, panel_tab_close(p))
#undef PMENU

/* "Open with": target is a desktop app id, "" asks via the system chooser */
static void pmenu_openwith(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a;
    Panel *p = d;
    set_active_panel(p->fm, p->idx);
    gchar *path = panel_cursor_fullpath(p);
    if (!path) return;
    GFile *file = g_file_new_for_path(path);
    const char *id = g_variant_get_string(v, NULL);
    if (id[0]) {
        GAppInfo *app = NULL;
        GList *all = g_app_info_get_all();
        for (GList *l = all; l && !app; l = l->next)
            if (g_strcmp0(g_app_info_get_id(l->data), id) == 0)
                app = g_object_ref(l->data);
        g_list_free_full(all, g_object_unref);
        GError *err = NULL;
        GList *files = g_list_append(NULL, file);
        GdkAppLaunchContext *lc = gdk_display_get_app_launch_context(
            gtk_widget_get_display(p->fm->window));
        if (!app || !g_app_info_launch(G_APP_INFO(app), files,
                                       G_APP_LAUNCH_CONTEXT(lc), &err))
            fm_status(p->fm, "Open with: %s", err ? err->message : "application not found");
        g_clear_error(&err);
        g_list_free(files);
        g_object_unref(lc);
        if (app) g_object_unref(app);
    } else {
        GtkFileLauncher *fl = gtk_file_launcher_new(file);
        gtk_file_launcher_set_always_ask(fl, TRUE);
        gtk_file_launcher_launch(fl, GTK_WINDOW(p->fm->window), NULL, NULL, NULL);
        g_object_unref(fl);
    }
    g_object_unref(file);
    g_free(path);
}

typedef struct { const char *label, *action; } MenuEntry;

/* Section of `menu` built from entries, actions prefixed with the group */
static void menu_section(GMenu *menu, const char *grp, const MenuEntry *e, gsize n)
{
    GMenu *sec = g_menu_new();
    for (gsize i = 0; i < n; i++) {
        gchar *act = g_strdup_printf("%s.%s", grp, e[i].action);
        g_menu_append(sec, e[i].label, act);
        g_free(act);
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
}

static GMenu *build_file_menu(const char *grp)
{
    static const MenuEntry edit[] = {
        { "Copy    Ctrl+C",            "copy"     },
        { "Cut    Ctrl+X",             "cut"      },
        { "Paste    Ctrl+V",           "paste"    },
        { "Copy path    Ctrl+Shift+C", "copypath" },
    };
    static const MenuEntry file[] = {
        { "Duplicate\u2026    Shift+F5",   "dup"      },
        { "Rename\u2026    Shift+F6",      "rename"   },
        { "Multi-rename\u2026    Ctrl+M",  "mrename"  },
        { "Delete\u2026    Del",           "delete"   },
        { "New directory\u2026    F7",     "mkdir"    },
        { "New file\u2026    Shift+F4",    "newfile"  },
        { "Properties\u2026    Alt+Enter", "props"    },
    };
    static const MenuEntry sel[] = {
        { "Select all    Ctrl+A",          "selall"   },
        { "Unselect all    Ctrl+Shift+A",  "selnone"  },
        { "Select group\u2026    +",       "selgroup" },
        { "Unselect group\u2026    -",     "unselgrp" },
        { "Invert selection    *",         "invert"   },
    };
    GMenu *m = g_menu_new();
    menu_section(m, grp, edit, G_N_ELEMENTS(edit));
    menu_section(m, grp, file, G_N_ELEMENTS(file));
    menu_section(m, grp, sel,  G_N_ELEMENTS(sel));
    return m;
}

/* "Open with" submenu for a local regular file, NULL otherwise */
static GMenu *build_openwith_menu(Panel *p, const char *grp)
{
    if (p->ssh_conn || p->arc_file) return NULL;
    gchar *path = panel_cursor_fullpath(p);
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) { g_free(path); return NULL; }

    gchar *ctype = g_content_type_guess(path, NULL, 0, NULL);
    g_free(path);
    GMenu *m = g_menu_new();
    GAppInfo *def = g_app_info_get_default_for_type(ctype, FALSE);
    GList *apps = g_app_info_get_all_for_type(ctype);
    g_free(ctype);
    gchar *act = g_strdup_printf("%s.openwith", grp);
    /* default application first, then the rest */
    if (def) apps = g_list_prepend(apps, def);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = apps; l; l = l->next) {
        const char *id = g_app_info_get_id(l->data);
        if (!id || g_hash_table_contains(seen, id)) continue;
        g_hash_table_add(seen, (gpointer)id);
        GMenuItem *mi = g_menu_item_new(g_app_info_get_display_name(l->data), NULL);
        g_menu_item_set_action_and_target_value(mi, act, g_variant_new_string(id));
        GIcon *ic = g_app_info_get_icon(l->data);
        if (ic) g_menu_item_set_icon(mi, ic);
        g_menu_append_item(m, mi);
        g_object_unref(mi);
    }
    g_hash_table_destroy(seen);
    g_list_free_full(apps, g_object_unref);

    GMenu *other = g_menu_new();
    GMenuItem *mi = g_menu_item_new("Other application\u2026", NULL);
    g_menu_item_set_action_and_target_value(mi, act, g_variant_new_string(""));
    g_menu_append_item(other, mi);
    g_object_unref(mi);
    g_menu_append_section(m, NULL, G_MENU_MODEL(other));
    g_object_unref(other);
    g_free(act);
    return m;
}

/* Right click: move cursor to the clicked row, then show the file menu */
static void on_cv_right_click(GtkGestureClick *g, int n_press, double x, double y, Panel *p)
{
    (void)n_press;
    set_active_panel(p->fm, p->idx);

    GtkWidget *w = gtk_widget_pick(p->column_view, x, y, GTK_PICK_DEFAULT);
    FileItem *hit = NULL;
    for (; w && w != p->column_view; w = gtk_widget_get_parent(w))
        if ((hit = g_object_get_data(G_OBJECT(w), "fm-item"))) break;
    if (hit) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
        for (guint i = 0; i < n; i++) {
            FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
            g_object_unref(it);
            if (it == hit) {
                p->cursor_pos = i;
                p->inhibit_sel = TRUE;
                gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                          NULL, GTK_LIST_SCROLL_SELECT, NULL);
                p->inhibit_sel = FALSE;
                break;
            }
        }
    }

    GtkWidget *pop = g_object_get_data(G_OBJECT(p->column_view), "fm-ctx-menu");
    {
        /* rebuilt each time: the "Open with" list depends on the file type */
        gchar *grp = g_strdup_printf("p%d", p->idx);
        GMenu *cm = build_file_menu(grp);
        GMenu *ow = hit ? build_openwith_menu(p, grp) : NULL;
        if (ow) {
            GMenu *sec = g_menu_new();
            g_menu_append_submenu(sec, "Open with", G_MENU_MODEL(ow));
            g_menu_prepend_section(cm, NULL, G_MENU_MODEL(sec));
            g_object_unref(sec);
            g_object_unref(ow);
        }
        gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(pop), G_MENU_MODEL(cm));
        g_object_unref(cm);
        g_free(grp);
    }
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}

/*─────────────────────────────────────────────────────────────────── *
 *  Panel: widget setup
 * ─────────────────────────────────────────────────────────────────── */

void panel_setup(Panel *p, FM *fm, int idx)
{
    p->fm  = fm;
    p->idx = idx;
    p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    p->tabs  = g_ptr_array_new_with_free_func(tab_free);
    g_ptr_array_add(p->tabs, tab_new(NULL));    /* follows the first load */

    /* outer vertical box */
    p->frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(p->frame, 2);
    gtk_widget_set_margin_end(p->frame, 2);

    /* ── tab strip (hidden until a second tab exists) ── */
    p->tab_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    p->tab_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(p->tab_scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(p->tab_scroll), p->tab_box);
    gtk_widget_set_visible(p->tab_scroll, FALSE);

    /* ── path bar ── */
    GtkWidget *path_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    p->path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->path_entry), "Path...");
    gtk_widget_set_hexpand(p->path_entry, TRUE);

    GtkWidget *up_btn   = gtk_button_new_from_icon_name("go-up-symbolic");
    GtkWidget *home_btn = gtk_button_new_from_icon_name("go-home-symbolic");
    gtk_widget_set_tooltip_text(up_btn,   "Go up (Backspace)");
    gtk_widget_set_tooltip_text(home_btn, "Home directory");

    /* SSH disconnect button – initially hidden */
    p->disconnect_btn = gtk_button_new_from_icon_name("network-offline-symbolic");
    gtk_widget_set_tooltip_text(p->disconnect_btn, "Disconnect SSH");
    gtk_widget_set_visible(p->disconnect_btn, FALSE);

    /* Search exit button – initially hidden */
    p->search_btn = gtk_button_new_from_icon_name("edit-clear-symbolic");
    gtk_widget_set_tooltip_text(p->search_btn, "Exit search results");
    gtk_widget_set_visible(p->search_btn, FALSE);

    /* Back-to-results button – shown after leaving search results */
    p->results_btn = gtk_button_new_from_icon_name("edit-find-symbolic");
    gtk_widget_set_tooltip_text(p->results_btn, "Back to search results (Alt+\u2190)");
    gtk_widget_set_visible(p->results_btn, FALSE);

    /* Terminal button – always visible */
    p->terminal_btn = gtk_button_new_from_icon_name("utilities-terminal-symbolic");
    gtk_widget_set_tooltip_text(p->terminal_btn, "Open terminal");

    /* ── Per-panel actions: hamburger menu + right-click menu ── */
    gchar *grp = g_strdup_printf("p%d", idx);
    GSimpleActionGroup *ag = g_simple_action_group_new();
    {
        struct { const char *name; GCallback cb; } acts[] = {
            { "ssh",      G_CALLBACK(pmenu_ssh)      },
            { "pack",     G_CALLBACK(pmenu_pack)     },
            { "extract",  G_CALLBACK(pmenu_extract)  },
            { "mask",     G_CALLBACK(pmenu_mask)     },
            { "search",   G_CALLBACK(pmenu_search)   },
            { "filter",   G_CALLBACK(pmenu_filter)   },
            { "sresults", G_CALLBACK(pmenu_sresults) },
            { "shistory", G_CALLBACK(pmenu_shistory) },
            { "copy",     G_CALLBACK(pmenu_copy)     },
            { "cut",      G_CALLBACK(pmenu_cut)      },
            { "paste",    G_CALLBACK(pmenu_paste)    },
            { "copypath", G_CALLBACK(pmenu_copypath) },
            { "dup",      G_CALLBACK(pmenu_dup)      },
            { "rename",   G_CALLBACK(pmenu_rename)   },
            { "mrename",  G_CALLBACK(pmenu_mrename)  },
            { "delete",   G_CALLBACK(pmenu_delete)   },
            { "mkdir",    G_CALLBACK(pmenu_mkdir)    },
            { "newfile",  G_CALLBACK(pmenu_newfile)  },
            { "props",    G_CALLBACK(pmenu_props)    },
            { "selall",   G_CALLBACK(pmenu_selall)   },
            { "selnone",  G_CALLBACK(pmenu_selnone)  },
            { "selgroup", G_CALLBACK(pmenu_selgroup) },
            { "unselgrp", G_CALLBACK(pmenu_unselgrp) },
            { "invert",   G_CALLBACK(pmenu_invert)   },
            { "tabnew",   G_CALLBACK(pmenu_tabnew)   },
            { "tabclose", G_CALLBACK(pmenu_tabclose) },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(acts); i++) {
            GSimpleAction *sa = g_simple_action_new(acts[i].name, NULL);
            g_signal_connect(sa, "activate", acts[i].cb, p);
            g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
            g_object_unref(sa);
        }
        {
            GSimpleAction *sa = g_simple_action_new("openwith", G_VARIANT_TYPE_STRING);
            g_signal_connect(sa, "activate", G_CALLBACK(pmenu_openwith), p);
            g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
            g_object_unref(sa);
        }

        static const MenuEntry conn[] = {
            { "SSH / SFTP\u2026", "ssh" },
        };
        static const MenuEntry arc[] = {
            { "Create archive\u2026    Alt+P",  "pack"    },
            { "Extract archive\u2026    Alt+E", "extract" },
        };
        static const MenuEntry find[] = {
            { "File mask\u2026",             "mask"   },
            { "Search files\u2026    F2",     "search" },
            { "Filter files\u2026    Ctrl+S", "filter" },
            { "Back to search results    Alt+\u2190", "sresults" },
            { "Search history\u2026    Shift+F2",  "shistory" },
        };
        static const MenuEntry tabs[] = {
            { "New tab    Ctrl+T",   "tabnew"   },
            { "Close tab    Ctrl+W", "tabclose" },
        };
        GMenu *pmenu = g_menu_new();
        menu_section(pmenu, grp, tabs, G_N_ELEMENTS(tabs));
        menu_section(pmenu, grp, conn, G_N_ELEMENTS(conn));
        GMenu *files = build_file_menu(grp);
        g_menu_append_submenu(pmenu, "Files", G_MENU_MODEL(files));
        g_object_unref(files);
        menu_section(pmenu, grp, arc,  G_N_ELEMENTS(arc));
        menu_section(pmenu, grp, find, G_N_ELEMENTS(find));

        GtkWidget *hbtn = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(hbtn), "open-menu-symbolic");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(hbtn), G_MENU_MODEL(pmenu));
        gtk_widget_set_tooltip_text(hbtn, "Panel menu");
        gtk_widget_set_focusable(hbtn, FALSE);
        g_object_unref(pmenu);

        gtk_box_append(GTK_BOX(path_box), devices_button_new(p));
        gtk_box_append(GTK_BOX(path_box), p->path_entry);
        gtk_box_append(GTK_BOX(path_box), hbtn);
        gtk_box_append(GTK_BOX(path_box), p->terminal_btn);
        gtk_box_append(GTK_BOX(path_box), p->disconnect_btn);
        gtk_box_append(GTK_BOX(path_box), p->search_btn);
        gtk_box_append(GTK_BOX(path_box), p->results_btn);
        gtk_box_append(GTK_BOX(path_box), up_btn);
        gtk_box_append(GTK_BOX(path_box), home_btn);
    }
    /* group on the frame: reachable from the hamburger and the popover */
    gtk_widget_insert_action_group(p->frame, grp, G_ACTION_GROUP(ag));
    g_object_unref(ag);

    /* ── GListStore ── */
    p->store = g_list_store_new(FILE_ITEM_TYPE);

    /* ── Column view ── */
    p->column_view = gtk_column_view_new(NULL);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(p->column_view), TRUE);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(p->column_view), FALSE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(p->column_view), FALSE);

    /* Name column */
    GtkListItemFactory *name_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(name_factory, "setup", G_CALLBACK(name_setup), NULL);
    g_signal_connect(name_factory, "bind",  G_CALLBACK(name_bind),  p);
    factory_watch_marks(name_factory, name_bind, p);
    GtkColumnViewColumn *col_name = gtk_column_view_column_new("Name", name_factory);
    gtk_column_view_column_set_resizable(col_name, TRUE);
    gtk_column_view_column_set_expand(col_name, TRUE);
    gtk_column_view_column_set_fixed_width(col_name,
        fm->col_name_width > 0 ? fm->col_name_width : 250);
    GtkSorter *ns = GTK_SORTER(gtk_custom_sorter_new(name_sort_func, NULL, NULL));
    gtk_column_view_column_set_sorter(col_name, ns);
    g_object_unref(ns);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(p->column_view), col_name);
    g_object_unref(col_name);

    /* Size column */
    GtkListItemFactory *size_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(size_factory, "setup", G_CALLBACK(size_setup), NULL);
    g_signal_connect(size_factory, "bind",  G_CALLBACK(size_bind),  p);
    factory_watch_marks(size_factory, size_bind, p);
    GtkColumnViewColumn *col_size = gtk_column_view_column_new("Size", size_factory);
    gtk_column_view_column_set_resizable(col_size, TRUE);
    gtk_column_view_column_set_fixed_width(col_size,
        fm->col_size_width > 0 ? fm->col_size_width : 100);
    GtkSorter *ss = GTK_SORTER(gtk_custom_sorter_new(size_sort_func, NULL, NULL));
    gtk_column_view_column_set_sorter(col_size, ss);
    g_object_unref(ss);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(p->column_view), col_size);
    g_object_unref(col_size);

    /* Date column */
    GtkListItemFactory *date_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(date_factory, "setup", G_CALLBACK(date_setup), NULL);
    g_signal_connect(date_factory, "bind",  G_CALLBACK(date_bind),  p);
    factory_watch_marks(date_factory, date_bind, p);
    GtkColumnViewColumn *col_date = gtk_column_view_column_new("Modified", date_factory);
    gtk_column_view_column_set_resizable(col_date, TRUE);
    gtk_column_view_column_set_fixed_width(col_date,
        fm->col_date_width > 0 ? fm->col_date_width : 145);
    GtkSorter *ds = GTK_SORTER(gtk_custom_sorter_new(date_sort_func, NULL, NULL));
    gtk_column_view_column_set_sorter(col_date, ds);
    g_object_unref(ds);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(p->column_view), col_date);
    g_object_unref(col_date);

    /* Sort model: dirs-first (always) + column sort (respects asc/desc) */
    GtkSorter *cv_sorter = gtk_column_view_get_sorter(
                               GTK_COLUMN_VIEW(p->column_view));
    GtkMultiSorter *multi = gtk_multi_sorter_new();
    gtk_multi_sorter_append(multi,
        GTK_SORTER(gtk_custom_sorter_new(dirs_first_func, NULL, NULL)));
    gtk_multi_sorter_append(multi, g_object_ref(cv_sorter));
    p->default_sorter = GTK_SORTER(g_object_ref(multi));
    p->sort_model = gtk_sort_list_model_new(
        g_object_ref(G_LIST_MODEL(p->store)),
        GTK_SORTER(multi));
    gtk_sort_list_model_set_incremental(p->sort_model, FALSE);

    /* Filter model (Ctrl+S) — between sort and selection */
    p->custom_filter = gtk_custom_filter_new(filter_match_func, p, NULL);
    p->filter_model = gtk_filter_list_model_new(
        g_object_ref(G_LIST_MODEL(p->sort_model)),
        GTK_FILTER(g_object_ref(p->custom_filter)));
    p->filter_text = NULL;

    p->selection = gtk_multi_selection_new(
        g_object_ref(G_LIST_MODEL(p->filter_model)));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(p->column_view),
                              GTK_SELECTION_MODEL(p->selection));

    /* Default sort by name ascending */
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(p->column_view),
                                   col_name, GTK_SORT_ASCENDING);

    /* Scrolled window */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->column_view);
    gtk_widget_set_vexpand(scroll, TRUE);

    /* status label */
    p->status_label = gtk_label_new("");
    gtk_widget_set_halign(p->status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(p->status_label, 4);

    /* filter bar (Ctrl+S) — initially hidden */
    p->filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(p->filter_bar, 4);
    gtk_widget_set_margin_end(p->filter_bar, 4);
    gtk_widget_set_margin_top(p->filter_bar, 2);
    gtk_widget_set_visible(p->filter_bar, FALSE);
    GtkWidget *filter_lbl = gtk_label_new("Filter:");
    p->filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->filter_entry), "type to filter…");
    gtk_widget_set_hexpand(p->filter_entry, TRUE);
    gtk_box_append(GTK_BOX(p->filter_bar), filter_lbl);
    gtk_box_append(GTK_BOX(p->filter_bar), p->filter_entry);

    g_signal_connect(p->filter_entry, "changed", G_CALLBACK(on_filter_changed), p);
    GtkEventController *filt_key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(filt_key, GTK_PHASE_CAPTURE);
    g_signal_connect(filt_key, "key-pressed", G_CALLBACK(on_filter_key), p);
    gtk_widget_add_controller(p->filter_entry, filt_key);

    /* pack */
    gtk_box_append(GTK_BOX(p->frame), p->tab_scroll);
    gtk_box_append(GTK_BOX(p->frame), path_box);
    gtk_box_append(GTK_BOX(p->frame), scroll);
    gtk_box_append(GTK_BOX(p->frame), p->filter_bar);
    gtk_box_append(GTK_BOX(p->frame), p->status_label);

    /* signals */
    g_signal_connect(p->selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), p);
    g_signal_connect(p->column_view, "activate", G_CALLBACK(on_cv_activate), p);
    g_signal_connect(p->path_entry, "activate", G_CALLBACK(on_path_activate), p);
    g_signal_connect(up_btn,            "clicked", G_CALLBACK(on_up_clicked), p);
    g_signal_connect(home_btn,          "clicked", G_CALLBACK(on_home_clicked), p);
    g_signal_connect(p->disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), p);
    g_signal_connect(p->search_btn,     "clicked", G_CALLBACK(on_search_exit_clicked), p);
    g_signal_connect(p->results_btn,    "clicked", G_CALLBACK(on_results_clicked), p);
    g_signal_connect(p->terminal_btn, "clicked", G_CALLBACK(on_terminal_clicked), p);

    /* Right-click context menu */
    {
        GMenu *cm = build_file_menu(grp);
        GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(cm));
        g_object_unref(cm);
        gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
        gtk_widget_set_halign(pop, GTK_ALIGN_START);
        gtk_widget_set_parent(pop, p->column_view);
        g_object_set_data(G_OBJECT(p->column_view), "fm-ctx-menu", pop);

        GtkGesture *rc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
        g_signal_connect(rc, "pressed", G_CALLBACK(on_cv_right_click), p);
        gtk_widget_add_controller(p->column_view, GTK_EVENT_CONTROLLER(rc));
    }
    g_free(grp);

    /* Key controller on column view (CAPTURE to intercept before default) */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_tree_key_press), p);
    gtk_widget_add_controller(p->column_view, key_ctrl);

    /* Key controller on path entry (for Enter/Escape) */
    GtkEventController *path_key = gtk_event_controller_key_new();
    g_signal_connect(path_key, "key-pressed", G_CALLBACK(on_path_key_press), p);
    gtk_widget_add_controller(p->path_entry, path_key);

    /* Focus controller */
    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "enter", G_CALLBACK(on_focus_enter), p);
    g_signal_connect(focus_ctrl, "leave", G_CALLBACK(on_focus_leave), p);
    gtk_widget_add_controller(p->column_view, focus_ctrl);

    gtk_widget_set_focusable(p->column_view, TRUE);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Bottom toolbar – F-key buttons
 * ─────────────────────────────────────────────────────────────────── */

static void btn_copy(GtkButton *b, gpointer d)    { (void)b; fo_copy(d);   }
static void btn_move(GtkButton *b, gpointer d)    { (void)b; fo_move(d);   }
static void btn_mkdir(GtkButton *b, gpointer d)   { (void)b; fo_mkdir(d);  }
static void btn_delete(GtkButton *b, gpointer d)  { (void)b; fo_delete(d); }
static void btn_help(GtkButton *b, gpointer d)    { (void)b; help_show(d); }
static void btn_find(GtkButton *b, gpointer d)    { (void)b; search_run(d); }
static void btn_view(GtkButton *b, gpointer d)    { (void)b; viewer_show(d); }
static void btn_editor(GtkButton *b, gpointer d)  { (void)b; editor_show(d); }
static void btn_quit(GtkButton *b, gpointer d)    { (void)b; g_application_quit(G_APPLICATION(((FM*)d)->app)); }

static GtkWidget *create_toolbar(FM *fm)
{
    static const struct {
        const char *label;
        const char *tooltip;
        void (*cb)(GtkButton *, gpointer);
    } btns[] = {
        { "F1 Help",    "Keyboard shortcuts",        btn_help    },
        { "F2 Search",  "Search files",              btn_find    },
        { "F3 View",    "View file contents",        btn_view    },
        { "F4 Editor",  "Open file in editor",       btn_editor  },
        { "F5 Copy",    "Copy to other panel",       btn_copy    },
        { "F6 Move",    "Move to other panel",       btn_move    },
        { "F7 MkDir",   "Create new directory",      btn_mkdir   },
        { "F8 Delete",  "Delete selected files",     btn_delete  },
        { "F10 Quit",   "Quit application",          btn_quit    },
    };

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_top(bar, 2);
    gtk_widget_set_margin_bottom(bar, 2);

    for (gsize i = 0; i < G_N_ELEMENTS(btns); i++) {
        GtkWidget *btn = gtk_button_new_with_label(btns[i].label);
        gtk_widget_set_tooltip_text(btn, btns[i].tooltip);
        gtk_widget_set_focusable(btn, FALSE);
        gtk_widget_add_css_class(btn, "fm-fkey");
        gtk_widget_set_hexpand(btn, TRUE);
        g_signal_connect(btn, "clicked", G_CALLBACK(btns[i].cb), fm);
        gtk_box_append(GTK_BOX(bar), btn);
    }
    return bar;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Global key handler
 * ─────────────────────────────────────────────────────────────────── */

static gboolean on_window_key_press(GtkEventControllerKey *ctrl,
                                     guint keyval, guint keycode,
                                     GdkModifierType state, FM *fm)
{
    (void)ctrl;

    /* Ctrl+Tab / Ctrl+Shift+Tab – next / previous tab (before plain Tab) */
    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab)) {
        gboolean back = keyval == GDK_KEY_ISO_Left_Tab || (state & GDK_SHIFT_MASK);
        panel_tab_cycle(active_panel(fm), back ? -1 : 1);
        return TRUE;
    }

    /* Tab – switch active panel */
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        /* the panel that really has the focus, not just fm->active */
        GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(fm->window));
        int cur = fm->active;
        for (int i = 0; i < 2; i++)
            if (f && (f == fm->panels[i].column_view ||
                      gtk_widget_is_ancestor(f, fm->panels[i].column_view)))
                cur = i;
        panel_focus(&fm->panels[cur ^ 1]);
        return TRUE;
    }

    /* Navigation keys with the keyboard focus nowhere in a panel (e.g. a
     * modal job window just closed and the focus did not come back):
     * GTK would move the focus to the next widget – typically the other
     * panel's path entry.  Put the cursor back into the active panel and
     * handle the key there instead. */
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_Down: case GDK_KEY_Page_Up: case GDK_KEY_Page_Down:
    case GDK_KEY_Home: case GDK_KEY_End: case GDK_KEY_Return: case GDK_KEY_KP_Enter:
    case GDK_KEY_space: case GDK_KEY_Insert: case GDK_KEY_BackSpace: {
        GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(fm->window));
        gboolean in_text  = f && (GTK_IS_EDITABLE(f) || GTK_IS_TEXT_VIEW(f));
        gboolean in_panel = FALSE;
        for (int i = 0; i < 2; i++)
            if (f && (f == fm->panels[i].column_view ||
                      gtk_widget_is_ancestor(f, fm->panels[i].column_view)))
                in_panel = TRUE;
        if (!in_text && !in_panel) {
            Panel *a = active_panel(fm);
            panel_focus(a);
            on_tree_key_press(NULL, keyval, keycode, state, a);
            return TRUE;
        }
        break;
    }
    default:
        break;
    }

    /* F-keys – only when not typing in path entry */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(fm->window));
    if (focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus))) return FALSE;

    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    gboolean ctl   = (state & GDK_CONTROL_MASK) != 0;
    gboolean alt   = (state & GDK_ALT_MASK) != 0;

    switch (keyval) {
    case GDK_KEY_F5:
        if (shift) fo_copy_here(fm); else fo_copy(fm);
        return TRUE;
    case GDK_KEY_F6:
        if (shift) fo_rename(fm); else fo_move(fm);
        return TRUE;
    case GDK_KEY_F7:  fo_mkdir(fm);           return TRUE;
    case GDK_KEY_F8:
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        if (shift) fo_delete_permanent(fm); else fo_delete(fm);
        return TRUE;
    case GDK_KEY_F9:  fo_rename(fm);          return TRUE;
    case GDK_KEY_F2:
        if (alt) { devices_popup(&fm->panels[1]); return TRUE; }
        if (shift) search_history_dialog(fm); else search_run(fm);
        return TRUE;

    /* Alt+Left – back to the last search results */
    case GDK_KEY_Left:
        if (alt) {
            if (!search_restore(active_panel(fm)))
                fm_status(fm, "No search results to return to (Shift+F2 = history)");
            return TRUE;
        }
        break;
    case GDK_KEY_F3:  viewer_show(fm);        return TRUE;
    case GDK_KEY_F4:
        if (shift) fo_newfile(fm); else editor_show(fm);
        return TRUE;
    case GDK_KEY_F1:
        if (alt) { devices_popup(&fm->panels[0]); return TRUE; }
        help_show(fm);
        return TRUE;
    case GDK_KEY_F10: g_application_quit(G_APPLICATION(fm->app)); return TRUE;

    /* Alt+Enter – properties */
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        if (alt) { fo_properties(fm); return TRUE; }
        break;

    /* Clipboard / marking */
    case GDK_KEY_c:
    case GDK_KEY_C:
        if (ctl && shift) { clip_copy_paths(fm);  return TRUE; }
        if (ctl)          { clip_copy(fm, FALSE); return TRUE; }
        break;
    case GDK_KEY_x:
    case GDK_KEY_X:
        if (ctl) { clip_copy(fm, TRUE); return TRUE; }
        break;
    case GDK_KEY_v:
    case GDK_KEY_V:
        if (ctl) { clip_paste(fm); return TRUE; }
        break;
    case GDK_KEY_a:
    case GDK_KEY_A:
        if (ctl) { sel_mark_all(active_panel(fm), !shift); return TRUE; }
        break;

    /* Alt+E – extract archive */
    case GDK_KEY_e:
    case GDK_KEY_E:
        if (state & GDK_ALT_MASK) { fo_extract(fm); return TRUE; }
        break;

    /* Alt+P – pack (create archive) */
    case GDK_KEY_p:
    case GDK_KEY_P:
        if (state & GDK_ALT_MASK) { fo_pack(fm); return TRUE; }
        break;

    /* Ctrl+M – multi-rename */
    case GDK_KEY_m:
    case GDK_KEY_M:
        if (ctl) { fo_multi_rename(fm); return TRUE; }
        break;

    /* Tabs: Ctrl+T new, Ctrl+W close, Ctrl+Up directory under cursor */
    case GDK_KEY_t:
    case GDK_KEY_T:
        if (ctl) { panel_tab_new(active_panel(fm), NULL); return TRUE; }
        break;
    case GDK_KEY_w:
    case GDK_KEY_W:
        if (ctl) { panel_tab_close(active_panel(fm)); return TRUE; }
        break;
    case GDK_KEY_Up:
        if (ctl) {
            Panel *a = active_panel(fm);
            if (a->ssh_conn) {
                gchar *rdir = NULL;
                guint n = g_list_model_get_n_items(G_LIST_MODEL(a->filter_model));
                FileItem *it = a->cursor_pos < n
                    ? g_list_model_get_item(G_LIST_MODEL(a->filter_model), a->cursor_pos)
                    : NULL;
                if (it && it->is_dir) {
                    const char *rp = a->ssh_conn->remote_path;
                    rdir = strcmp(it->name, "..") == 0 ? g_path_get_dirname(rp)
                         : g_build_filename(rp, it->name, NULL);
                }
                if (it) g_object_unref(it);
                panel_tab_new(a, rdir);
                g_free(rdir);
                return TRUE;
            }
            gchar *name = panel_cursor_name(a);
            gchar *full = panel_cursor_fullpath(a);
            if (name && strcmp(name, "..") == 0 && !a->search_mode) {
                g_free(full);
                full = g_path_get_dirname(a->cwd);
            }
            if (full && g_file_test(full, G_FILE_TEST_IS_DIR))
                panel_tab_new(a, full);
            else
                panel_tab_new(a, NULL);
            g_free(name);
            g_free(full);
            return TRUE;
        }
        break;

    /* Ctrl+S – filter panel */
    case GDK_KEY_s:
    case GDK_KEY_S:
        if (state & GDK_CONTROL_MASK) {
            panel_show_filter(active_panel(fm));
            return TRUE;
        }
        break;

    /* Ctrl+R – reload both panels */
    case GDK_KEY_r:
    case GDK_KEY_R:
        if (state & GDK_CONTROL_MASK) {
            panel_reload(&fm->panels[0]);
            panel_reload(&fm->panels[1]);
            fm_status(fm, "Panels refreshed");
            return TRUE;
        }
        break;

    /* Ctrl+H – toggle hidden files */
    case GDK_KEY_h:
    case GDK_KEY_H:
        if (state & GDK_CONTROL_MASK) {
            fm->show_hidden = !fm->show_hidden;
            settings_save(fm);
            panel_reload(&fm->panels[0]);
            panel_reload(&fm->panels[1]);
            fm_status(fm, fm->show_hidden ? "Hidden files: shown" : "Hidden files: hidden");
            return TRUE;
        }
        break;

    /* Ctrl+= – synchronize panels */
    case GDK_KEY_equal:
        if (state & GDK_CONTROL_MASK) {
            Panel *a = active_panel(fm), *o = other_panel(fm);
            if (a->ssh_conn || o->ssh_conn) {
                /* remote → remote only within the same server */
                SshConn *ac = a->ssh_conn, *oc = o->ssh_conn;
                if (ac && oc && ac->port == oc->port &&
                    g_strcmp0(ac->host, oc->host) == 0 &&
                    g_strcmp0(ac->user, oc->user) == 0) {
                    panel_load(o, ac->remote_path);
                    fm_status(fm, "Panels synchronized");
                } else {
                    fm_status(fm, "Cannot synchronize a local and an SFTP panel "
                                  "(or two different servers)");
                }
                return TRUE;
            }
            /* search results: the directory of the found file under the
             * cursor, otherwise the directory the search started from */
            const char *dir = a->cwd;
            FileItem *it = NULL;
            if (a->search_mode) {
                dir = a->search_prev_cwd;
                guint n = g_list_model_get_n_items(G_LIST_MODEL(a->filter_model));
                if (a->cursor_pos < n)
                    it = g_list_model_get_item(G_LIST_MODEL(a->filter_model), a->cursor_pos);
                if (it && it->dir_path) dir = it->dir_path;
                else if (it && it->size_raw == -1) dir = it->name;  /* group header */
            }
            gchar *d = g_strdup(dir);
            if (it) g_object_unref(it);
            panel_load(o, d);
            g_free(d);
            fm_status(fm, "Panels synchronized");
            return TRUE;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Application activate
 * ─────────────────────────────────────────────────────────────────── */

/* ── Menu action callbacks ────────────────────────────────────────── */

static void menu_mask(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)v;
    FM *fm = d;
    Panel *p = active_panel(fm);

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "File mask");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 350, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *lbl = gtk_label_new("Show only (glob, e.g. *.c):");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *entry = gtk_entry_new();
    /* Pre-fill with current mask or * */
    gtk_editable_set_text(GTK_EDITABLE(entry),
        (p->mask_pattern && p->mask_pattern[0]) ? p->mask_pattern : "*");

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *btn_ok     = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(btn_box), btn_ok);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(btn_ok,     "clicked",  G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(btn_cancel, "clicked",  G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate", G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg, "close-request",   G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));
    int resp = dlg_ctx_run(&ctx);

    const gchar *pat = gtk_editable_get_text(GTK_EDITABLE(entry));
    gchar *pattern = g_strdup(pat);
    gtk_window_destroy(GTK_WINDOW(dlg));

    if (resp != 1) { g_free(pattern); return; }

    /* Set mask: "*" or empty means no mask */
    g_free(p->mask_pattern);
    if (!pattern[0] || strcmp(pattern, "*") == 0 || strcmp(pattern, "*.*") == 0)
        p->mask_pattern = NULL;
    else
        p->mask_pattern = pattern;
    if (!p->mask_pattern) g_free(pattern);

    /* Trigger re-filter */
    gtk_filter_changed(GTK_FILTER(p->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
    p->cursor_pos = 0;
    p->inhibit_sel = TRUE;
    if (g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0) {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL, GTK_LIST_SCROLL_SELECT, NULL);
    }
    p->inhibit_sel = FALSE;
    panel_update_status(p);
    panel_focus(p);
}
static void restore_panel_cursor(FM *fm)
{
    Panel *p = active_panel(fm);
    panel_focus(p);
    if (g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0) {
        p->inhibit_sel = TRUE;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
                                  p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        p->inhibit_sel = FALSE;
    }
}

static void on_window_focus_in(GtkEventControllerFocus *ctrl, FM *fm)
{
    (void)ctrl;
    restore_panel_cursor(fm);
}

/* ─────────────────────────────────────────────────────────────────── */


static void on_activate(GtkApplication *app, gpointer data)
{
    FM *fm = data;
    fm->app = app;

    /* CSS – app styles */
    fm->css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* CSS – font (managed by settings, higher priority) */
    fm->font_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->font_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    /* CSS – editor font (highest priority, targets textview.fm-editor) */
    fm->editor_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->editor_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

    /* CSS – viewer font (targets textview.fm-viewer) */
    fm->viewer_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->viewer_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

    /* load settings (applies font CSS) */
    settings_load(fm);

    /* window */
    fm->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(fm->window), APP_NAME " – File Manager");
    gtk_window_set_default_size(GTK_WINDOW(fm->window), 1200, 700);

    /* header bar */
    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hbar), TRUE);

    /* reload button in header */
    GtkWidget *reload_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reload_btn, "Refresh both panels (Ctrl+R)");
    g_signal_connect_swapped(reload_btn, "clicked", G_CALLBACK(panel_reload), &fm->panels[0]);
    g_signal_connect_swapped(reload_btn, "clicked", G_CALLBACK(panel_reload), &fm->panels[1]);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), reload_btn);

    /* settings gear button in header */
    GtkWidget *settings_btn = gtk_button_new_from_icon_name("preferences-system-symbolic");
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    g_signal_connect_swapped(settings_btn, "clicked",
        G_CALLBACK(settings_dialog), fm);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), settings_btn);



    gtk_window_set_titlebar(GTK_WINDOW(fm->window), hbar);

    /* main layout */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(fm->window), vbox);

    /* paned */
    fm->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(fm->paned), TRUE);

    panel_setup(&fm->panels[0], fm, 0);
    panel_setup(&fm->panels[1], fm, 1);

    gtk_paned_set_start_child(GTK_PANED(fm->paned), fm->panels[0].frame);
    gtk_paned_set_end_child(GTK_PANED(fm->paned), fm->panels[1].frame);
    gtk_paned_set_resize_start_child(GTK_PANED(fm->paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(fm->paned), TRUE);

    /* toolbar */
    GtkWidget *toolbar = create_toolbar(fm);

    /* statusbar (GtkLabel) */
    fm->statusbar = gtk_label_new("");
    gtk_widget_set_halign(fm->statusbar, GTK_ALIGN_START);
    gtk_widget_set_margin_start(fm->statusbar, 4);
    gtk_widget_set_margin_top(fm->statusbar, 2);
    gtk_widget_set_margin_bottom(fm->statusbar, 2);

    gtk_widget_set_vexpand(fm->paned, TRUE);
    gtk_box_append(GTK_BOX(vbox), fm->paned);
    gtk_box_append(GTK_BOX(vbox), toolbar);
    gtk_box_append(GTK_BOX(vbox), fm->statusbar);

    /* global key handler (CAPTURE phase for Tab + F-keys) */
    GtkEventController *win_key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(win_key, GTK_PHASE_CAPTURE);
    g_signal_connect(win_key, "key-pressed", G_CALLBACK(on_window_key_press), fm);
    gtk_widget_add_controller(fm->window, win_key);

    /* Restore cursor when window regains focus (e.g. after Alt-Tab, taskbar click) */
    GtkEventController *win_focus = gtk_event_controller_focus_new();
    g_signal_connect(win_focus, "enter", G_CALLBACK(on_window_focus_in), fm);
    gtk_widget_add_controller(fm->window, win_focus);

    devices_init(fm);

    /* load directories: saved tabs, or the home directory */
    for (int i = 0; i < 2; i++) {
        int active = 0;
        gchar **paths = settings_load_tabs(i, &active);
        panel_tabs_restore(&fm->panels[i], paths, active);
        g_strfreev(paths);
    }

    /* set initial active panel – deselect the inactive one */
    set_active_panel(fm, 0);
    fm->panels[1].inhibit_sel = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(fm->panels[1].selection));
    fm->panels[1].inhibit_sel = FALSE;

    gtk_window_present(GTK_WINDOW(fm->window));
    panel_focus(&fm->panels[0]);

    fm_status(fm, "Welcome to " APP_NAME " – File Manager");
}

/* Exit: remember the tabs of both panels, then free them */
static void on_shutdown(GApplication *app, FM *fm)
{
    (void)app;
    if (!fm->window) return;
    /* widgets may be gone by now – the tab paths are current anyway
     * (panel_load keeps the active one in sync) */
    if (tabs_save_id) { g_source_remove(tabs_save_id); tabs_save_id = 0; }
    settings_save_tabs(fm);
    arc_panel_leave(&fm->panels[0]);
    arc_panel_leave(&fm->panels[1]);
    panel_tabs_free(&fm->panels[0]);
    panel_tabs_free(&fm->panels[1]);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  main
 * ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    memset(&g_fm, 0, sizeof(g_fm));

#ifdef HAVE_LIBADWAITA
    adw_init();
    GtkApplication *app = GTK_APPLICATION(
        adw_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS));
#else
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &g_fm);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &g_fm);
    /* lets worker-thread prompts (SSH host key) find the main window */
    g_object_set_data(G_OBJECT(app), "fm", &g_fm);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    view_tmpdirs_cleanup();
    arc_panel_cleanup();
    g_object_unref(app);
    return status;
}
