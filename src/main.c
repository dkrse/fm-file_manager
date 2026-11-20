#include "fm.h"
#include "dialogs.h"
#include "job.h"
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

static gboolean on_main_close_request(GtkWindow *win, FM *fm);

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

static void item_index_drop(GListModel *m, guint pos, guint rem, guint add, Panel *p)
{
    (void)m; (void)pos; (void)rem; (void)add;
    g_clear_pointer(&p->item_index, g_hash_table_destroy);
}

static GHashTable *item_index(Panel *p)
{
    if (p->item_index && p->item_index_store == p->store) return p->item_index;
    g_clear_pointer(&p->item_index, g_hash_table_destroy);
    if (p->item_index_store) {                 /* old store still alive */
        g_signal_handler_disconnect(p->item_index_store, p->item_index_hid);
        g_object_remove_weak_pointer(G_OBJECT(p->item_index_store),
                                     (gpointer *)&p->item_index_store);
        p->item_index_store = NULL;
    }
    GListModel *m = G_LIST_MODEL(p->store);
    guint n = g_list_model_get_n_items(m);
    p->item_index = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(m, i);
        g_hash_table_insert(p->item_index, sel_mark_key(p, it), it);
        g_object_unref(it);                    /* the store keeps it */
    }
    p->item_index_store = p->store;
    g_object_add_weak_pointer(G_OBJECT(p->store), (gpointer *)&p->item_index_store);
    p->item_index_hid = g_signal_connect(p->store, "items-changed",
                                         G_CALLBACK(item_index_drop), p);
    return p->item_index;
}

void panel_update_status(Panel *p)
{
    if (p->idx == p->fm->active) cmdline_update_prompt(p->fm);
    guint n_marks = p->marks ? g_hash_table_size(p->marks) : 0;
    guint total = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));

    gchar *msg;
    if (n_marks > 0) {
        /* total size of marked files (directories are not walked) –
         * O(marks) through the index instead of a scan per keypress */
        gint64 bytes = 0;
        GHashTable *idx = item_index(p);
        GHashTableIter hi;
        gpointer key;
        g_hash_table_iter_init(&hi, p->marks);
        while (g_hash_table_iter_next(&hi, &key, NULL)) {
            FileItem *it = g_hash_table_lookup(idx, key);
            if (it && it->marked && !it->is_dir && it->size_raw > 0) bytes += it->size_raw;
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

/* ── reading a whole file in a worker (viewer, editor, open, verify) ── */
typedef struct {
    SshConn *conn;              /* ref held, NULL = local */
    gchar   *path;
    gsize    max_len;
    gint     cancelled;         /* atomic */
} ReadReq;

typedef struct {
    gchar *data;
    gsize  len;
    gchar *err;
} ReadRes;

static void read_req_free(gpointer d)
{
    ReadReq *q = d;
    if (q->conn) ssh_conn_unref(q->conn);
    g_free(q->path);
    g_free(q);
}

static void read_res_free(gpointer d)
{
    ReadRes *r = d;
    if (!r) return;
    g_free(r->data);
    g_free(r->err);
    g_free(r);
}

static void read_req_cancel(gpointer d)
{
    g_atomic_int_set(&((ReadReq *)d)->cancelled, 1);
}

static gpointer read_file_thread(gpointer d)
{
    ReadReq *q = d;
    ReadRes *r = g_new0(ReadRes, 1);
    if (q->conn) {
#ifdef HAVE_LIBSSH2
        r->data = ssh_read_file(q->conn, q->path, q->max_len, &r->len);
        if (!r->data) r->err = g_strdup("SFTP read error");
#else
        r->err = g_strdup("SFTP is not supported");
#endif
        return r;
    }
    int fd = open(q->path, O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        r->err = g_strdup(g_strerror(errno));
        if (fd >= 0) close(fd);
        return r;
    }
    if (S_ISDIR(st.st_mode)) {
        r->err = g_strdup(g_strerror(EISDIR));
        close(fd);
        return r;
    }
    gsize cap = (S_ISREG(st.st_mode) && st.st_size > 0) ? (gsize)st.st_size + 1 : 65536;
    if (q->max_len && cap > q->max_len) cap = q->max_len;
    GByteArray *ba = g_byte_array_sized_new((guint)MIN(cap, (gsize)G_MAXUINT - 1));
    guint8 buf[65536];
    for (;;) {
        if (g_atomic_int_get(&q->cancelled)) break;
        gsize want = sizeof buf;
        if (q->max_len) {
            if (ba->len >= q->max_len) break;
            want = MIN(want, q->max_len - ba->len);
        }
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { r->err = g_strdup(g_strerror(errno)); break; }
        if (n == 0) break;
        if (ba->len > G_MAXUINT - sizeof buf) { r->err = g_strdup("File too large"); break; }
        g_byte_array_append(ba, buf, (guint)n);
    }
    close(fd);
    if (r->err) { g_byte_array_free(ba, TRUE); return r; }
    r->len = ba->len;
    g_byte_array_append(ba, (const guint8 *)"", 1);    /* NUL-terminate */
    r->data = (gchar *)g_byte_array_free(ba, FALSE);
    return r;
}

gchar *fm_read_file(FM *fm, SshConn *conn, const char *path, gsize max_len,
                    gsize *out_len, gchar **err, gboolean *cancelled)
{
    *out_len = 0;
    if (err) *err = NULL;
    gboolean dummy;
    if (!cancelled) cancelled = &dummy;
    *cancelled = FALSE;
    if (!path) { if (err) *err = g_strdup("No file"); return NULL; }
    ReadReq *q = g_new0(ReadReq, 1);
    q->conn    = conn ? ssh_conn_ref(conn) : NULL;
    q->path    = g_strdup(path);
    q->max_len = max_len;
    gchar *base = g_path_get_basename(path);
    ReadRes *r = fm_wait_work(fm, "Reading file", base, read_file_thread, q,
                              read_req_free, read_res_free, read_req_cancel,
                              cancelled);
    g_free(base);
    if (!r) return NULL;                  /* cancelled */
    gchar *data = r->data;
    *out_len = r->len;
    if (err) { *err = r->err; r->err = NULL; }
    r->data = NULL;
    read_res_free(r);
    return data;
}

/* ── remote save in a worker (editor F2): a slow link must not freeze
 * every window for the whole upload ── */
typedef struct { SshConn *conn; gchar *path; gchar *data; gsize len; } WriteReq;

static void write_req_free(gpointer d)
{
    WriteReq *q = d;
    ssh_conn_unref(q->conn);
    g_free(q->path);
    g_free(q->data);
    g_free(q);
}

static gpointer write_file_thread(gpointer d)
{
    WriteReq *q = d;
    /* never NULL: NULL from fm_wait_work means "cancelled" */
    return GINT_TO_POINTER(ssh_write_file(q->conn, q->path, q->data, q->len) ? 2 : 1);
}

int fm_write_remote(FM *fm, SshConn *conn, const char *path,
                    const gchar *data, gsize len)
{
    WriteReq *q = g_new0(WriteReq, 1);
    q->conn = ssh_conn_ref(conn);
    q->path = g_strdup(path);
    q->data = g_memdup2(data, len);
    q->len  = len;
    gchar *base = g_path_get_basename(path);
    gboolean cancelled;
    gpointer r = fm_wait_work(fm, "Saving file", base, write_file_thread, q,
                              write_req_free, NULL, NULL, &cancelled);
    g_free(base);
    if (cancelled) return -1;
    return GPOINTER_TO_INT(r) == 2 ? 1 : 0;
}

/* Directory-specific part */
typedef struct {
    struct stat st;
    gchar      *key;      /* collation key (owned until taken by the item) */
    gboolean    ok;       /* fstatat succeeded                             */
    char        name[];
} DirEnt;

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

static void dirent_free(gpointer data)
{
    DirEnt *e = data;
    g_free(e->key);
    g_free(e);
}

/* fstatat + sort key for a slice of the entries.  Both are per-entry
 * independent: on a 50k directory they took ~70 ms in one thread. */
typedef struct { GPtrArray *ents; guint from, to; int dfd; } DirSlice;

static gpointer dir_slice_thread(gpointer data)
{
    DirSlice *s = data;
    for (guint i = s->from; i < s->to; i++) {
        DirEnt *e = g_ptr_array_index(s->ents, i);
        e->ok = fstatat(s->dfd, e->name, &e->st, AT_SYMLINK_NOFOLLOW) == 0;
        if (e->ok) e->key = g_utf8_collate_key_for_filename(e->name, -1);
    }
    return NULL;
}

#define DIR_SLICE_MIN 2048    /* below this a single thread is faster */
#define DIR_THREADS_MAX 8

/* → GPtrArray of DirEnt*, NULL = opendir failed */
static gpointer dir_load_thread(gpointer data)
{
    DirLoad *d = data;
    /* the stat runs here too: on a hung network mount it blocks */
    struct stat st;
    if (stat(d->path, &st) != 0 || !S_ISDIR(st.st_mode)) return NULL;
    DIR *dir = opendir(d->path);
    if (!dir) return NULL;
    GPtrArray *ents = g_ptr_array_new_with_free_func(dirent_free);
    int dfd = dirfd(dir);
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.' &&
            (de->d_name[1] == '\0' ||
             (de->d_name[1] == '.' && de->d_name[2] == '\0')))
            continue;
        if (!d->hidden && de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name) + 1;
        DirEnt *e = g_malloc(sizeof(DirEnt) + len);
        e->key = NULL;
        e->ok  = FALSE;
        memcpy(e->name, de->d_name, len);
        g_ptr_array_add(ents, e);
    }

    guint n = ents->len;
    guint nt = MIN(DIR_THREADS_MAX, MAX(1u, n / DIR_SLICE_MIN));
    nt = MIN(nt, MAX(1u, g_get_num_processors()));
    DirSlice sl[DIR_THREADS_MAX];
    GThread *th[DIR_THREADS_MAX] = { NULL };
    for (guint t = 0; t < nt; t++) {
        sl[t] = (DirSlice){ ents, n * t / nt, n * (t + 1) / nt, dfd };
        if (t > 0) th[t] = g_thread_new("fm-stat", dir_slice_thread, &sl[t]);
    }
    dir_slice_thread(&sl[0]);
    for (guint t = 1; t < nt; t++) g_thread_join(th[t]);
    closedir(dir);

    /* drop entries that vanished between readdir and stat */
    GPtrArray *out = g_ptr_array_new_full(n, dirent_free);
    g_ptr_array_set_free_func(ents, NULL);
    for (guint i = 0; i < n; i++) {
        DirEnt *e = g_ptr_array_index(ents, i);
        if (e->ok) g_ptr_array_add(out, e);
        else       dirent_free(e);
    }
    g_ptr_array_unref(ents);
    return out;
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

void panel_load(Panel *p, const char *path)
{
    if (p->ssh_conn) { panel_load_remote(p, path); return; }

    if (p->busy) return;            /* a search is filling this panel */
    panel_scroll_remember(p);
    if (!path || !path[0]) return;
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
    if (!ents) {                    /* not a directory or unreadable */
        if (strcmp(p->cwd, want) != 0)
            fm_status(p->fm, "Cannot open directory %s", want);
        panel_monitor_start(p);
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
        if (!fi->sort_key) { fi->sort_key = e->key; e->key = NULL; }
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
    panel_tab_track(p);
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
    /* panel_load remembered the offset: keep the view where it was
     * instead of jumping to put the cursor row at an edge */
    panel_scroll_restore(p);
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

/* Row text with colour/weight as shared attribute lists instead of Pango
 * markup: no escaping and no markup parsing per bind (3 labels × every
 * row scrolled into view).  A handful of colour/weight pairs exist. */
static PangoAttrList *row_attrs(const gchar *fg, gint wt)
{
    static GHashTable *cache;
    if (!fg && wt == PANGO_WEIGHT_NORMAL) return NULL;
    if (!cache)
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)pango_attr_list_unref);
    gchar key[80];
    g_snprintf(key, sizeof key, "%s|%d", fg ? fg : "", wt);
    PangoAttrList *l = g_hash_table_lookup(cache, key);
    if (l) return l;
    l = pango_attr_list_new();
    PangoColor c;
    if (fg && pango_color_parse(&c, fg))
        pango_attr_list_insert(l, pango_attr_foreground_new(c.red, c.green, c.blue));
    if (wt != PANGO_WEIGHT_NORMAL)
        pango_attr_list_insert(l, pango_attr_weight_new((PangoWeight)wt));
    g_hash_table_insert(cache, g_strdup(key), l);
    return l;
}

static void label_set_styled(GtkWidget *label, const char *text,
                             const gchar *fg, gint wt)
{
    PangoAttrList *a = row_attrs(fg, wt);
    if (gtk_label_get_attributes(GTK_LABEL(label)) != a)
        gtk_label_set_attributes(GTK_LABEL(label), a);
    gtk_label_set_text(GTK_LABEL(label), text ? text : "");
}

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
    if (gtk_image_get_pixel_size(GTK_IMAGE(image)) != isz) {  /* rows are reused */
        gtk_image_set_pixel_size(GTK_IMAGE(image), isz);
        gtk_widget_set_size_request(image, isz, isz);
    }

    const gchar *fg = item->marked ? p->fm->vis_mark_color : item->fg_color;
    gint wt = item->marked ? PANGO_WEIGHT_BOLD : item->weight;
    label_set_styled(label, item->name, fg, wt);

    /* content search: ":line  matching text" after the name */
    if (item->snippet) {
        gchar *head = fg ? g_markup_printf_escaped(
                               "<span foreground='%s' weight='%d'>%s</span>",
                               fg, wt, item->name)
                         : g_markup_escape_text(item->name, -1);
        gchar *tail = g_markup_printf_escaped("  <span alpha='55%%'>:%d  %s</span>",
                                              item->line, item->snippet);
        gchar *mk = g_strconcat(head, tail, NULL);
        gtk_label_set_attributes(GTK_LABEL(label), NULL);
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
    label_set_styled(label, file_item_size_str(item), fg, PANGO_WEIGHT_NORMAL);
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
    const gchar *text = (p->search_mode && item->dir_path) ? item->dir_path : file_item_date_str(item);

    const gchar *fg = item->marked ? p->fm->vis_mark_color : item->fg_color;
    label_set_styled(label, text, fg, PANGO_WEIGHT_NORMAL);
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
            gchar *remote_path = fs_remote_join(p->ssh_conn->remote_path, name);
            gsize len = 0;
            gboolean canc = FALSE;
            gchar *data = fm_read_file(p->fm, p->ssh_conn, remote_path, 0, &len,
                                       NULL, &canc);
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
            } else if (canc) {
                fm_status(p->fm, "Download cancelled");
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

gboolean on_tree_key_press(GtkEventControllerKey *ctrl,
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
void set_active_panel(FM *fm, int idx)
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
    cmdline_update_prompt(fm);
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
/* Is row `pos` fully inside the visible part of the list?  Rows have one
 * height, so it follows from the adjustment (upper = n rows). */
static gboolean panel_row_visible(Panel *p, guint pos, guint n)
{
    GtkAdjustment *va = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->column_view));
    if (!va || n == 0) return FALSE;
    double upper = gtk_adjustment_get_upper(va), page = gtk_adjustment_get_page_size(va);
    double v = gtk_adjustment_get_value(va);
    if (upper <= 0 || page <= 0) return FALSE;      /* not laid out yet */
    if (upper <= page) return TRUE;                 /* everything fits */
    double h = upper / n;
    return pos * h >= v && (pos + 1) * h <= v + page;
}

/* Select + focus the cursor row.  When it is already on screen (back
 * from F3/F4 or any dialog) the list must not move: a plain scroll_to
 * shifted it, and the focused row widget could be recycled so the
 * cursor vanished.  Off screen → scrolled into view as usual. */
static void panel_cursor_show(Panel *p, guint n)
{
    GtkScrollInfo *si = NULL;
    if (panel_row_visible(p, p->cursor_pos, n)) {
        si = gtk_scroll_info_new();
        gtk_scroll_info_set_enable_vertical(si, FALSE);
        gtk_scroll_info_set_enable_horizontal(si, FALSE);
    }
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), p->cursor_pos,
                              NULL, GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS,
                              si);   /* takes ownership */
}

/* ── exact view around F3/F4: the list must look the same afterwards ── */
double panel_view_save(Panel *p)
{
    GtkAdjustment *va = p->column_view
        ? gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->column_view)) : NULL;
    return va ? gtk_adjustment_get_value(va) : -1;
}

typedef struct { Panel *p; double value; guint cursor; gchar *dir; } ViewRestore;

static void view_apply(Panel *p, double value)
{
    GtkAdjustment *va = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->column_view));
    if (va && value >= 0) gtk_adjustment_set_value(va, value);
    panel_focus(p);              /* row now visible → selected, no scroll */
}

static gboolean view_restore_idle(gpointer d)
{
    ViewRestore *r = d;
    const char *dir = panel_dir(r->p);
    /* only if nothing else changed the panel meanwhile */
    if (dir && g_strcmp0(dir, r->dir) == 0 && r->p->cursor_pos == r->cursor)
        view_apply(r->p, r->value);
    g_free(r->dir);
    g_free(r);
    return G_SOURCE_REMOVE;
}

void panel_view_restore(Panel *p, double value)
{
    view_apply(p, value);
    /* the main window gets focus back asynchronously and its handlers
     * (plus the list's own focus scroll) run after this: apply again
     * once they are done */
    ViewRestore *r = g_new0(ViewRestore, 1);
    r->p = p; r->value = value; r->cursor = p->cursor_pos;
    r->dir = g_strdup(panel_dir(p));
    g_timeout_add(60, view_restore_idle, r);
}

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
        panel_cursor_show(p, n);
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
    /* only the window lost focus (F3/F4, a dialog, another app): keep the
     * cursor – clearing it left the panel with no visible cursor after
     * the viewer/editor closed.  Focus moving to another widget of this
     * window still hides it. */
    GtkRoot *root = gtk_widget_get_root(p->column_view);
    if (GTK_IS_WINDOW(root) && !gtk_window_is_active(GTK_WINDOW(root))) return;
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
        panel_cursor_show(p, n);
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

        /* The remote shell parses this one string: quote the path once.
         * Locally there is no shell – ssh gets plain argv entries, and
         * "--" keeps a host/user starting with '-' from becoming an option. */
        gchar *remote_cmd;
        if (rpath && rpath[0]) {
            gchar *quoted_path = g_shell_quote(rpath);
            remote_cmd = g_strdup_printf("cd -- %s && exec \"$SHELL\" -l",
                                         quoted_path);
            g_free(quoted_path);
        } else {
            remote_cmd = g_strdup("exec \"$SHELL\" -l");
        }
        gchar *target = g_strdup_printf("%s@%s", p->ssh_conn->user,
                                        p->ssh_conn->host);
        gchar port[16];
        g_snprintf(port, sizeof port, "%d", p->ssh_conn->port);

        const gchar *argv[12];
        int a = 0;
        argv[a++] = term;
        if (strstr(term, "ptyxis")) {
            argv[a++] = "--new-window";
        }
        argv[a++] = "--";
        argv[a++] = "ssh";
        argv[a++] = "-t";
        if (p->ssh_conn->port != 22) {
            argv[a++] = "-p";
            argv[a++] = port;
        }
        argv[a++] = "--";
        argv[a++] = target;
        argv[a++] = remote_cmd;
        argv[a] = NULL;

        GError *err = NULL;
        if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                           G_SPAWN_SEARCH_PATH,
                           NULL, NULL, NULL, &err)) {
            fm_status(p->fm, "Failed to open terminal: %s",
                      err ? err->message : "?");
            g_clear_error(&err);
        }
        g_free(remote_cmd);
        g_free(target);
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

/* File mask changed: attach the filter only while a mask is set */
void panel_apply_mask(Panel *p)
{
    gboolean on = p->mask_pattern && p->mask_pattern[0];
    GtkFilter *cur = gtk_filter_list_model_get_filter(p->filter_model);
    if (on && !cur)
        gtk_filter_list_model_set_filter(p->filter_model, GTK_FILTER(p->custom_filter));
    else if (on)
        gtk_filter_changed(GTK_FILTER(p->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
    else if (cur)
        gtk_filter_list_model_set_filter(p->filter_model, NULL);
}

static void on_filter_changed(GtkEditable *editable, Panel *p)
{
    const gchar *text = gtk_editable_get_text(editable);
    if (!text || !text[0]) return;

    /* MC-style incremental search: jump cursor to first matching item */
    gchar *lower = g_utf8_strdown(text, -1);
    gboolean ascii = TRUE;
    for (const char *c = lower; *c; c++) if ((guchar)*c >= 0x80) { ascii = FALSE; break; }
    size_t nlen = strlen(lower);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (!item) continue;
        gboolean match = FALSE;
        if (ascii) {        /* common case: no allocation per row */
            for (const char *h = item->name; *h && !match; h++) {
                size_t k = 0;
                while (k < nlen && h[k] && g_ascii_tolower(h[k]) == lower[k]) k++;
                match = k == nlen;
            }
        } else {
            gchar *name_lower = g_utf8_strdown(item->name, -1);
            match = strstr(name_lower, lower) != NULL;
            g_free(name_lower);
        }
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

void panel_show_filter(Panel *p)
{
    gtk_widget_set_visible(p->filter_bar, TRUE);
    gtk_widget_grab_focus(p->filter_entry);
}

/*─────────────────────────────────────────────────────────────────── *
 *  Panel: widget setup
 * ─────────────────────────────────────────────────────────────────── */

void panel_setup(Panel *p, FM *fm, int idx)
{
    p->fm  = fm;
    p->idx = idx;
    p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    p->tabs  = g_ptr_array_new_with_free_func(panel_tab_free);
    g_ptr_array_add(p->tabs, panel_tab_item_new(NULL));    /* follows the first load */

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
    GSimpleActionGroup *ag = panel_menu_actions(p);
    {
        GtkWidget *hbtn = panel_menu_button(grp);

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
    /* no filter while no mask is set: a custom filter is called for every
     * item of every load (~75 ms for 50k files) – see panel_apply_mask */
    p->filter_model = gtk_filter_list_model_new(
        g_object_ref(G_LIST_MODEL(p->sort_model)), NULL);
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
    p->list_scroll = scroll;
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

    panel_context_menu_attach(p, grp);
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
 *  Application activate
 * ─────────────────────────────────────────────────────────────────── */

static void restore_panel_cursor(FM *fm)
{
    /* panel_focus already selects + focuses the cursor row (without
     * moving a visible one); a second plain scroll_to here jumped */
    panel_focus(active_panel(fm));
}

static void on_window_focus_in(GtkEventControllerFocus *ctrl, FM *fm)
{
    (void)ctrl;
    restore_panel_cursor(fm);
}

/* ─────────────────────────────────────────────────────────────────── */


/* Dialogs without their own titlebar get server-side decorations drawn
 * by the compositor in the *system* theme – a light title bar on a dark
 * fm.  Give every new toplevel a GTK header bar so it follows fm's theme
 * (a window that sets its own titlebar later simply replaces it). */
static void on_toplevels_changed(GListModel *list, guint pos, guint removed,
                                 guint added, gpointer d)
{
    (void)removed; (void)d;
    for (guint i = pos; i < pos + added; i++) {
        GtkWindow *w = g_list_model_get_item(list, i);
        if (GTK_IS_WINDOW(w) && !gtk_window_get_titlebar(w) &&
            !gtk_widget_get_realized(GTK_WIDGET(w)) &&
            !GTK_IS_DIALOG(w) && gtk_window_get_decorated(w))
            gtk_window_set_titlebar(w, gtk_header_bar_new());
        g_object_unref(w);
    }
}

static void on_activate(GtkApplication *app, gpointer data)
{
    static gboolean hooked;
    if (!hooked) {
        hooked = TRUE;
        g_signal_connect(gtk_window_get_toplevels(), "items-changed",
                         G_CALLBACK(on_toplevels_changed), NULL);
    }
    FM *fm = data;
    /* second launch: the running instance just comes to the front
     * (building everything again would leak the first UI) */
    if (fm->window) { gtk_window_present(GTK_WINDOW(fm->window)); return; }
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
    {
        int w = 1200, h = 700;
        gboolean max = FALSE;
        settings_load_window(&w, &h, &max);
        gtk_window_set_default_size(GTK_WINDOW(fm->window), w, h);
        if (max) gtk_window_maximize(GTK_WINDOW(fm->window));
    }
    g_signal_connect(fm->window, "close-request",
                     G_CALLBACK(on_main_close_request), fm);

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
    gtk_box_append(GTK_BOX(vbox), create_cmdline(fm));
    gtk_box_append(GTK_BOX(vbox), toolbar);

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

/* Main window closing: remember its size (the default size tracks the
 * current unmaximized size in GTK 4) */
static gboolean on_main_close_request(GtkWindow *win, FM *fm)
{
    if (job_running_count() > 0) {
        fm_status(fm, "File operations are still running – wait for them "
                      "or cancel them (Ctrl+J shows background jobs)");
        job_show_background(fm);
        return TRUE;                    /* keep the window */
    }
    int w = 0, h = 0;
    gtk_window_get_default_size(win, &w, &h);
    settings_save_window(w, h, gtk_window_is_maximized(win));
    return FALSE;                       /* let the window close */
}

/* Exit: remember the tabs of both panels, then free them */
static void on_shutdown(GApplication *app, FM *fm)
{
    (void)app;
    if (!fm->window) return;
    /* widgets may be gone by now – the tab paths are current anyway
     * (panel_load keeps the active one in sync) */
    panel_tabs_cancel_save();
    qv_close(fm);
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
