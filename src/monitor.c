#include "fm.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Directory monitoring (auto-refresh via GFileMonitor / inotify)
 *
 *  Uses incremental updates: on file create/delete we add/remove
 *  a single item in the GListStore.  GtkColumnView only re-renders
 *  visible rows, so even huge directories stay fast.
 * ─────────────────────────────────────────────────────────────────── */

/* Build a panel row from lstat() data – shared by load, monitor, poll */
FileItem *file_item_from_stat(FM *fm, const char *name, const struct stat *st)
{
    gboolean is_dir  = S_ISDIR(st->st_mode);
    gboolean is_link = S_ISLNK(st->st_mode);
    gboolean is_exec = !is_dir && (st->st_mode & (S_IXUSR | S_IXGRP | S_IXOTH));

    /* theme-adjusted directory colour, recomputed only when it changes */
    static gchar *vdc, *vdc_src;
    static gint   vdc_theme = -1;
    const gchar *fg = NULL;
    gint wt = PANGO_WEIGHT_NORMAL;
    if (is_dir) {
        if (vdc_theme != fm_dark(fm) || g_strcmp0(vdc_src, fm->dir_color) != 0) {
            g_free(vdc);
            g_free(vdc_src);
            vdc       = fm_visible_color(fm, fm->dir_color);
            vdc_src   = g_strdup(fm->dir_color);
            vdc_theme = fm_dark(fm);
        }
        fg = vdc;
        wt = fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL;
    } else if (is_link) {
        fg = fm_link_color(fm);
    } else if (is_exec) {
        fg = fm_exec_color(fm);
    }

    /* file_item_new_take takes ownership of size/date strings */
    FileItem *fi = file_item_new_take(icon_for_entry(name, is_dir, is_link, is_exec),
                                      name,
                                      is_dir ? g_strdup("<DIR>") : fmt_size(st->st_size),
                                      fmt_date(st->st_mtime),
                                      is_dir, fg, wt,
                                      (gint64)st->st_size, (gint64)st->st_mtime);
    return fi;
}

static gint store_find(Panel *p, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = 0; i < n; i++) {
        FileItem *fi = g_list_model_get_item(G_LIST_MODEL(p->store), i);
        gboolean hit = fi && strcmp(fi->name, name) == 0;
        if (fi) g_object_unref(fi);
        if (hit) return (gint)i;
    }
    return -1;
}

/* Store changes shift rows: keep the cursor on the same file, or on the
 * row where it was when that file disappeared */
static gchar *cursor_name_save(Panel *p)
{
    FileItem *fi = g_list_model_get_item(G_LIST_MODEL(p->filter_model), p->cursor_pos);
    gchar *name = fi ? g_strdup(fi->name) : NULL;
    if (fi) g_object_unref(fi);
    return name;
}

static void cursor_name_restore(Panel *p, gchar *name, guint old_pos)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    gint found = -1;
    for (guint i = 0; name && i < n && found < 0; i++) {
        FileItem *fi = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (fi && strcmp(fi->name, name) == 0) found = (gint)i;
        if (fi) g_object_unref(fi);
    }
    g_free(name);
    if (n == 0) { p->cursor_pos = 0; return; }
    guint pos = found >= 0 ? (guint)found : MIN(old_pos, n - 1);
    p->cursor_pos = pos;
    p->inhibit_sel = TRUE;
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), pos, NULL,
        p->fm->active == p->idx ? GTK_LIST_SCROLL_SELECT : GTK_LIST_SCROLL_NONE, NULL);
    p->inhibit_sel = FALSE;
}

static void monitor_remove(Panel *p, const char *name)
{
    gint i = store_find(p, name);
    if (i < 0) return;
    p->inhibit_sel = TRUE;
    g_list_store_remove(p->store, (guint)i);
    p->inhibit_sel = FALSE;
    if (p->marks) g_hash_table_remove(p->marks, name);
    panel_update_status(p);
}

/* Add a new entry or refresh an existing one (size/date/type changed) */
static void monitor_upsert(Panel *p, const char *name)
{
    if (!p->fm->show_hidden && name[0] == '.') return;

    gchar *fullpath = g_build_filename(p->cwd, name, NULL);
    struct stat fst;
    gboolean ok = lstat(fullpath, &fst) == 0;
    g_free(fullpath);
    if (!ok) return;

    FileItem *nfi = file_item_from_stat(p->fm, name, &fst);
    gint i = store_find(p, name);
    p->inhibit_sel = TRUE;
    if (i >= 0) {
        FileItem *old = g_list_model_get_item(G_LIST_MODEL(p->store), (guint)i);
        nfi->marked = old->marked;
        g_object_unref(old);
        g_list_store_splice(p->store, (guint)i, 1, (gpointer *)&nfi, 1);
    } else {
        nfi->marked = p->marks && g_hash_table_contains(p->marks, name);
        g_list_store_append(p->store, nfi);
    }
    p->inhibit_sel = FALSE;
    g_object_unref(nfi);
    panel_update_status(p);
}

/* Events are collected and applied in one batch: unpacking an archive
 * into the visible directory fires thousands of them, and handling each
 * one separately (linear lookup + status walk) froze the panel. */
#define MON_BATCH_MS     150
#define MON_RELOAD_LIMIT 64     /* more changes than this → one reload */

static gboolean monitor_flush(gpointer data)
{
    Panel *p = data;
    p->mon_flush_id = 0;
    GHashTable *pending = p->mon_pending;
    p->mon_pending = NULL;
    if (!pending) return G_SOURCE_REMOVE;

    if (g_hash_table_size(pending) > MON_RELOAD_LIMIT) {
        panel_reload(p);                       /* keeps cursor and marks */
    } else {
        guint old_pos = p->cursor_pos;
        gchar *cur = cursor_name_save(p);
        GHashTableIter it;
        gpointer key;
        g_hash_table_iter_init(&it, pending);
        while (g_hash_table_iter_next(&it, &key, NULL)) {
            gchar *full = g_build_filename(p->cwd, key, NULL);
            struct stat st;
            gboolean exists = lstat(full, &st) == 0;
            g_free(full);
            if (exists) monitor_upsert(p, key);
            else        monitor_remove(p, key);
        }
        cursor_name_restore(p, cur, old_pos);
    }
    g_hash_table_destroy(pending);
    return G_SOURCE_REMOVE;
}

static void monitor_queue(Panel *p, const char *name)
{
    if (!p->mon_pending)
        p->mon_pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(p->mon_pending, g_strdup(name));
    if (!p->mon_flush_id)
        p->mon_flush_id = g_timeout_add(MON_BATCH_MS, monitor_flush, p);
}

static void on_dir_changed(GFileMonitor *monitor, GFile *file,
                           GFile *other, GFileMonitorEvent event,
                           Panel *p)
{
    (void)monitor;

    /* Skip SFTP and search mode panels */
    if (p->ssh_conn || p->search_mode) return;

    gchar *basename = g_file_get_basename(file);
    if (!basename) return;
    if (strcmp(basename, ".") == 0 || strcmp(basename, "..") == 0) {
        g_free(basename);
        return;
    }

    switch (event) {
    case G_FILE_MONITOR_EVENT_DELETED:
    case G_FILE_MONITOR_EVENT_MOVED_OUT:
    case G_FILE_MONITOR_EVENT_CREATED:
    case G_FILE_MONITOR_EVENT_MOVED_IN:
    case G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT:
    case G_FILE_MONITOR_EVENT_ATTRIBUTE_CHANGED:
        monitor_queue(p, basename);
        break;
    case G_FILE_MONITOR_EVENT_RENAMED: {
        /* WATCH_MOVES reports renames inside the directory as one event */
        gchar *nn = other ? g_file_get_basename(other) : NULL;
        if (nn && p->marks && g_hash_table_remove(p->marks, basename))
            g_hash_table_add(p->marks, g_strdup(nn));
        monitor_queue(p, basename);
        if (nn) monitor_queue(p, nn);
        g_free(nn);
        break;
    }
    default:
        break;
    }
    g_free(basename);
}

/* ── SFTP polling: periodic incremental refresh ──────────────────── */

#ifdef HAVE_LIBSSH2
/* The listing runs in a worker so a slow link never stalls the UI;
 * the result is merged into the store on the main thread. */
typedef struct {
    Panel      *p;
    SshConn    *conn;
    gchar      *path;
    gboolean    hidden;
    guint       gen;
    GHashTable *remote;      /* name → LIBSSH2_SFTP_ATTRIBUTES*, NULL = busy/failed */
} SftpPoll;

static gpointer sftp_poll_thread(gpointer data);

static gboolean sftp_poll_apply(gpointer data)
{
    SftpPoll *sp = data;
    Panel *p = sp->p;

    /* stale: panel_monitor_stop() already joined the thread */
    if (sp->gen != p->poll_gen) goto out;
    g_thread_join(p->poll_thread);
    p->poll_thread = NULL;

    GHashTable *remote = sp->remote;
    if (!remote || p->ssh_conn != sp->conn || p->search_mode ||
        strcmp(p->ssh_conn->remote_path, sp->path) != 0)
        goto out;

    gboolean changed = FALSE;
    guint old_pos = p->cursor_pos;
    gchar *cur = cursor_name_save(p);

    /* Pass 1: remove items no longer on remote; note the ones we have */
    GHashTable *present = g_hash_table_new(g_str_hash, g_str_equal);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = n; i > 0; i--) {
        FileItem *fi = g_list_model_get_item(G_LIST_MODEL(p->store), i - 1);
        if (!fi) continue;
        if (strcmp(fi->name, "..") != 0 &&
            !g_hash_table_contains(remote, fi->name)) {
            p->inhibit_sel = TRUE;
            g_list_store_remove(p->store, i - 1);
            p->inhibit_sel = FALSE;
            changed = TRUE;
        } else {
            g_hash_table_add(present, fi->name);   /* name owned by store item */
        }
        g_object_unref(fi);
    }

    /* Pass 2: add new items from remote */
    GHashTableIter iter;
    gpointer key, val;
    g_hash_table_iter_init(&iter, remote);
    while (g_hash_table_iter_next(&iter, &key, &val)) {
        const char *rname = key;
        if (g_hash_table_contains(present, rname)) continue;

        LIBSSH2_SFTP_ATTRIBUTES *a = val;
        struct stat st = { 0 };
        if (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) st.st_mode = a->permissions;
        if (a->flags & LIBSSH2_SFTP_ATTR_SIZE)        st.st_size = (off_t)a->filesize;
        if (a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME)   st.st_mtime = (time_t)a->mtime;
        FileItem *fi = file_item_from_stat(p->fm, rname, &st);
        p->inhibit_sel = TRUE;
        g_list_store_append(p->store, fi);
        g_object_unref(fi);
        p->inhibit_sel = FALSE;
        changed = TRUE;
    }

    g_hash_table_destroy(present);
    if (changed) {
        panel_update_status(p);
        cursor_name_restore(p, cur, old_pos);
    } else {
        g_free(cur);
    }

out:
    if (sp->remote) g_hash_table_destroy(sp->remote);
    g_free(sp->path);
    g_free(sp);
    return G_SOURCE_REMOVE;
}

static gpointer sftp_poll_thread(gpointer data)
{
    SftpPoll *sp = data;
    SshConn *conn = sp->conn;

    /* A file operation owns the session right now – try next time */
    if (ssh_conn_trylock(conn)) {
        LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(conn->sftp, sp->path);
        if (handle) {
            sp->remote = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
            char name_buf[4096], longentry[4096];
            LIBSSH2_SFTP_ATTRIBUTES attrs;
            int rc;
            while ((rc = libssh2_sftp_readdir_ex(handle, name_buf, sizeof(name_buf),
                                                  longentry, sizeof(longentry), &attrs)) > 0) {
                if (strcmp(name_buf, ".") == 0 || strcmp(name_buf, "..") == 0)
                    continue;
                if (!sp->hidden && name_buf[0] == '.')
                    continue;
                LIBSSH2_SFTP_ATTRIBUTES *a = g_new(LIBSSH2_SFTP_ATTRIBUTES, 1);
                *a = attrs;
                g_hash_table_insert(sp->remote, g_strdup(name_buf), a);
            }
            libssh2_sftp_closedir(handle);
            if (rc < 0) {               /* partial listing – don't apply it */
                g_hash_table_destroy(sp->remote);
                sp->remote = NULL;
            }
        }
        ssh_conn_unlock(conn);
    }
    g_idle_add(sftp_poll_apply, sp);
    return NULL;
}

static gboolean sftp_poll_tick(gpointer data)
{
    Panel *p = data;
    if (!p->ssh_conn || !p->ssh_conn->sftp || p->search_mode || p->poll_thread)
        return G_SOURCE_CONTINUE;

    SftpPoll *sp = g_new0(SftpPoll, 1);
    sp->p      = p;
    sp->conn   = p->ssh_conn;
    sp->path   = g_strdup(p->ssh_conn->remote_path);
    sp->hidden = p->fm->show_hidden;
    sp->gen    = p->poll_gen;
    p->poll_thread = g_thread_new("fm-sftp-poll", sftp_poll_thread, sp);
    return G_SOURCE_CONTINUE;
}
#endif /* HAVE_LIBSSH2 */

void panel_monitor_stop(Panel *p)
{
    if (p->mon_flush_id) {
        g_source_remove(p->mon_flush_id);
        p->mon_flush_id = 0;
    }
    g_clear_pointer(&p->mon_pending, g_hash_table_destroy);
    if (p->dir_monitor) {
        g_file_monitor_cancel(p->dir_monitor);
        g_object_unref(p->dir_monitor);
        p->dir_monitor = NULL;
    }
    if (p->poll_id) {
        g_source_remove(p->poll_id);
        p->poll_id = 0;
    }
    /* the connection may be closed right after this – wait for the
     * listing (holds the session lock, so it is short) and drop it */
    p->poll_gen++;
    if (p->poll_thread) {
        g_thread_join(p->poll_thread);
        p->poll_thread = NULL;
    }
}

void panel_monitor_start(Panel *p)
{
    panel_monitor_stop(p);

    if (!p->fm->auto_refresh) return;
    if (p->search_mode) return;
    if (p->arc_file) return;            /* archives are not watched */

#ifdef HAVE_LIBSSH2
    if (p->ssh_conn) {
        /* SFTP: poll every 5 seconds */
        p->poll_id = g_timeout_add_seconds(5, sftp_poll_tick, p);
        return;
    }
#endif

    if (!p->cwd[0]) return;

    GFile *dir = g_file_new_for_path(p->cwd);
    p->dir_monitor = g_file_monitor_directory(dir,
        G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    g_object_unref(dir);

    if (p->dir_monitor) {
        g_file_monitor_set_rate_limit(p->dir_monitor, 500); /* ms */
        g_signal_connect(p->dir_monitor, "changed",
                         G_CALLBACK(on_dir_changed), p);
    }
}
