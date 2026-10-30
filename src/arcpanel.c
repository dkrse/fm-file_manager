#include "fm.h"
#include "job.h"
#include "dialogs.h"
#include "arcfs.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Archives as directories – the panel side of arcfs.c.
 *
 *  Enter on a supported archive reads its table of contents once (in a
 *  worker, see fm_wait_work) and the panel then browses it like a
 *  directory tree.  The archive is read-only: F5 extracts the selection,
 *  Enter / F3 / F4 on a file work on a temporary copy.
 *
 *  While inside, p->cwd stays the directory holding the archive, so
 *  tabs, Ctrl+= and "leave" behave as if the panel were still there;
 *  arc_file / arc_dir / arc_entries say what is shown.  Any panel_load
 *  of a real directory drops the archive state (arc_panel_leave).
 * ─────────────────────────────────────────────────────────────────── */

/* Temp dirs with extracted copies; the external app may still read
 * them, so they go when fm exits (arc_panel_cleanup). */
static GSList *arc_tmpdirs;

/* Identifies the archive version the entries were read from */
static gint64 arc_stamp(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (gint64)st.st_mtim.tv_sec * G_GINT64_CONSTANT(1000000000) +
           st.st_mtim.tv_nsec + (gint64)st.st_size * 7919;
}

static gchar *arc_inner(Panel *p, const char *name)
{
    return p->arc_dir[0] ? g_strconcat(p->arc_dir, "/", name, NULL) : g_strdup(name);
}

gboolean arc_panel_readonly_msg(Panel *p)
{
    if (!p->arc_file) return FALSE;
    fm_status(p->fm, "Archive is read-only");
    return TRUE;
}

void arc_panel_leave(Panel *p)
{
    if (!p->arc_file) return;
    g_clear_pointer(&p->arc_file, g_free);
    g_clear_pointer(&p->arc_dir, g_free);
    g_clear_pointer(&p->arc_entries, g_ptr_array_unref);
    if (p->marks) g_hash_table_remove_all(p->marks);
}

/* ── listing in a worker ──────────────────────────────────────────── */

typedef struct {
    GPtrArray *entries;
    gchar     *error;
} ArcList;

static void arc_list_free(gpointer d)
{
    ArcList *l = d;
    if (l->entries) g_ptr_array_unref(l->entries);
    g_free(l->error);
    g_free(l);
}

static gpointer arc_list_thread(gpointer path)
{
    ArcList *l = g_new0(ArcList, 1);
    GError *err = NULL;
    l->entries = arcfs_list(path, &err);
    if (!l->entries) {
        l->error = g_strdup(err ? err->message : "unknown error");
        g_clear_error(&err);
    }
    return l;
}

/* Table of contents, NULL on error (*error set) or cancel (*cancelled) */
static GPtrArray *arc_read(FM *fm, const char *archive, gchar **error,
                           gboolean *cancelled)
{
    *error = NULL;
    ArcList *l = fm_wait_work(fm, "Reading archive", archive, arc_list_thread,
                              g_strdup(archive), g_free, arc_list_free, cancelled);
    if (!l) return NULL;
    GPtrArray *entries = l->entries;
    l->entries = NULL;
    *error = g_steal_pointer(&l->error);
    arc_list_free(l);
    return entries;
}

/* ── display ──────────────────────────────────────────────────────── */

/* Show p->arc_dir; the cursor goes to `select` (NULL = first row) */
static void arc_render(Panel *p, const char *select)
{
    FM *fm = p->fm;
    p->inhibit_sel = TRUE;

    gchar *shown = p->arc_dir[0] ? g_build_filename(p->arc_file, p->arc_dir, NULL)
                                 : g_strdup(p->arc_file);
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), shown);
    g_free(shown);

    /* same layout as panel_load: ".." first, the sorter does the rest */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    gchar *vis_dir_color = fm_visible_color(fm, fm->dir_color);
    g_ptr_array_add(items,
        file_item_new("go-up-symbolic", "..", "", "",
                      TRUE, vis_dir_color, fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                      (gint64)-2, (gint64)0));
    g_free(vis_dir_color);

    GPtrArray *kids = arcfs_children(p->arc_entries, p->arc_dir);
    for (guint i = 0; i < kids->len; i++) {
        ArcEntry *e = g_ptr_array_index(kids, i);
        const char *slash = strrchr(e->path, '/');
        const char *name = slash ? slash + 1 : e->path;
        if (!fm->show_hidden && name[0] == '.') continue;

        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_mode = e->is_dir ? (S_IFDIR | (e->mode & 07777)) : e->mode;
        if (!(st.st_mode & S_IFMT)) st.st_mode |= S_IFREG;
        st.st_size  = e->is_dir ? 0 : e->size;
        st.st_mtime = (time_t)e->mtime;
        FileItem *fi = file_item_from_stat(fm, name, &st);
        if (p->marks && g_hash_table_contains(p->marks, name))
            fi->marked = TRUE;
        g_ptr_array_add(items, fi);
    }
    g_ptr_array_unref(kids);

    GListStore *store = g_list_store_new(FILE_ITEM_TYPE);
    g_list_store_splice(store, 0, 0, (gpointer *)items->pdata, items->len);
    g_ptr_array_unref(items);
    gtk_sort_list_model_set_model(p->sort_model, G_LIST_MODEL(store));
    g_object_unref(p->store);
    p->store = store;
    panel_update_status(p);

    p->cursor_pos = 0;
    if (g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0)
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0, NULL,
            p->fm->active == p->idx ? GTK_LIST_SCROLL_SELECT : GTK_LIST_SCROLL_NONE,
            NULL);
    p->inhibit_sel = FALSE;
    if (select) panel_select_name(p, select);
}

/* ── navigation ───────────────────────────────────────────────────── */

gboolean arc_panel_enter(Panel *p, const char *archive)
{
#ifdef HAVE_LIBARCHIVE
    if (!archive || p->ssh_conn || p->busy) return FALSE;
    gchar *base = g_path_get_basename(archive);
    gboolean ok = arcfs_supported(base);
    g_free(base);
    if (!ok || !g_file_test(archive, G_FILE_TEST_IS_REGULAR)) return FALSE;
    gchar *dir = g_path_get_dirname(archive);
    if (strlen(dir) >= sizeof(p->cwd)) { g_free(dir); return FALSE; }

    gchar *file = g_strdup(archive);   /* may point into the panel */
    gint64 stamp = arc_stamp(file);
    gchar *error;
    gboolean cancelled;
    GPtrArray *entries = arc_read(p->fm, file, &error, &cancelled);
    if (!entries) {
        if (cancelled) fm_status(p->fm, "Reading the archive was cancelled");
        else           fm_status(p->fm, "Cannot browse the archive: %s", error);
        g_free(error); g_free(file); g_free(dir);
        return cancelled;            /* error → caller opens it as a file */
    }

    /* found by a search: the results stay reachable via Alt+Left */
    gboolean was_search = p->search_mode;
    search_detach(p);
    if (was_search)
        gtk_sort_list_model_set_sorter(p->sort_model, p->default_sorter);
    search_update_buttons(p);

    arc_panel_leave(p);
    panel_monitor_stop(p);
    if (p->marks) g_hash_table_remove_all(p->marks);
    g_strlcpy(p->cwd, dir, sizeof(p->cwd));
    p->arc_file    = file;
    p->arc_dir     = g_strdup("");
    p->arc_entries = entries;
    p->arc_stamp   = stamp;
    arc_render(p, NULL);
    fm_status(p->fm, "Archive (read-only): F5 extracts, Backspace at the top leaves");
    g_free(dir);
    return TRUE;
#else
    (void)p; (void)archive;
    return FALSE;
#endif
}

/* Back to the directory holding the archive, cursor on the archive */
static void arc_exit(Panel *p)
{
    gchar *dir  = g_strdup(p->cwd);
    gchar *name = g_path_get_basename(p->arc_file);
    panel_load(p, dir);
    if (!p->arc_file) panel_select_name(p, name);
    g_free(dir);
    g_free(name);
}

void arc_panel_go_up(Panel *p)
{
    if (!p->arc_dir[0]) { arc_exit(p); return; }
    const char *slash = strrchr(p->arc_dir, '/');
    gchar *came_from = g_strdup(slash ? slash + 1 : p->arc_dir);
    gchar *parent = slash ? g_strndup(p->arc_dir, (gsize)(slash - p->arc_dir))
                          : g_strdup("");
    g_free(p->arc_dir);
    p->arc_dir = parent;
    if (p->marks) g_hash_table_remove_all(p->marks);
    arc_render(p, came_from);
    g_free(came_from);
}

static void arc_open(Panel *p, int how);

void arc_panel_activate(Panel *p)
{
    FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), p->cursor_pos);
    if (!it) return;
    gchar *name = g_strdup(it->name);
    gboolean is_dir = it->is_dir;
    g_object_unref(it);

    if (strcmp(name, "..") == 0) {
        arc_panel_go_up(p);
    } else if (is_dir) {
        gchar *inner = arc_inner(p, name);
        g_free(p->arc_dir);
        p->arc_dir = inner;
        if (p->marks) g_hash_table_remove_all(p->marks);
        arc_render(p, NULL);
    } else {
        arc_open(p, 0);
    }
    g_free(name);
}

/* Ctrl+R, after operations, hidden files toggled: re-read the archive
 * only when the file changed, otherwise just re-render */
void arc_panel_reload(Panel *p)
{
    gchar *cur = panel_cursor_name(p);
    gint64 stamp = arc_stamp(p->arc_file);
    if (stamp < 0) {                    /* archive gone: back to its directory */
        arc_exit(p);
        g_free(cur);
        return;
    }
    if (stamp != p->arc_stamp) {
        gchar *error;
        gboolean cancelled;
        GPtrArray *entries = arc_read(p->fm, p->arc_file, &error, &cancelled);
        if (!entries && !cancelled) {
            fm_status(p->fm, "Cannot read the archive: %s", error);
            g_free(error);
            g_free(cur);
            arc_exit(p);
            return;
        }
        if (entries) {
            g_ptr_array_unref(p->arc_entries);
            p->arc_entries = entries;
            p->arc_stamp   = stamp;
        }
    }
    arc_render(p, cur);
    g_free(cur);
}

/* ── temporary copies (Enter / F3 / F4) ───────────────────────────── */

typedef struct {
    gchar *archive, *base, *inner, *dest;
} ArcPick;

static void arc_pick_free(gpointer d)
{
    ArcPick *a = d;
    g_free(a->archive); g_free(a->base); g_free(a->inner); g_free(a->dest);
    g_free(a);
}

/* → error message, or NULL on success */
static gpointer arc_pick_thread(gpointer d)
{
    ArcPick *a = d;
    FsCtx ctx = { .policy = FS_OVERWRITE_ALL };
    const char *inner[] = { a->inner };
    int r = arcfs_extract(&ctx, a->archive, a->base, inner, 1, a->dest);
    gchar *err = r == FS_OK ? NULL
               : g_strdup(ctx.error ? ctx.error : "extraction failed");
    fs_ctx_clear(&ctx);
    return err;
}

/* how: 0 = default application, 1 = viewer, 2 = editor */
static void arc_open(Panel *p, int how)
{
    FM *fm = p->fm;
    FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), p->cursor_pos);
    if (!it || strcmp(it->name, "..") == 0) {
        if (it) g_object_unref(it);
        fm_status(fm, "No file is selected");
        return;
    }
    gchar *name = g_strdup(it->name);
    gboolean is_dir = it->is_dir;
    g_object_unref(it);
    if (is_dir) {
        fm_status(fm, "'%s' is a directory", name);
        g_free(name);
        return;
    }

    gchar *tmpdir = g_dir_make_tmp("fm-arc-XXXXXX", NULL);
    if (!tmpdir) {
        fm_status(fm, "Error creating temp directory");
        g_free(name);
        return;
    }
    arc_tmpdirs = g_slist_prepend(arc_tmpdirs, tmpdir);

    ArcPick *a = g_new0(ArcPick, 1);
    a->archive = g_strdup(p->arc_file);
    a->base    = g_strdup(p->arc_dir);
    a->inner   = arc_inner(p, name);
    a->dest    = g_strdup(tmpdir);
    gboolean cancelled;
    gchar *err = fm_wait_work(fm, "Extracting", name, arc_pick_thread, a,
                              arc_pick_free, g_free, &cancelled);
    gchar *path = g_build_filename(tmpdir, name, NULL);
    if (cancelled) {
        fm_status(fm, "Extraction cancelled");
    } else if (err || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        fm_status(fm, "Cannot extract '%s': %s", name, err ? err : "not found");
    } else if (how == 1) {
        viewer_show_path(fm, path, name);
    } else if (how == 2) {
        editor_show_path(fm, path);
        fm_status(fm, "Editing a temporary copy – changes are not written back into the archive");
    } else {
        gchar *uri = g_filename_to_uri(path, NULL, NULL);
        if (uri) gtk_show_uri(GTK_WINDOW(fm->window), uri, GDK_CURRENT_TIME);
        g_free(uri);
        fm_status(fm, "Opened a temporary copy: %s", name);
    }
    g_free(err);
    g_free(path);
    g_free(name);
}

void arc_panel_open(FM *fm, int how)
{
    arc_open(active_panel(fm), how);
}

void arc_panel_cleanup(void)
{
    for (GSList *l = arc_tmpdirs; l; l = l->next)
        fs_delete(NULL, l->data);
    g_slist_free_full(arc_tmpdirs, g_free);
    arc_tmpdirs = NULL;
}

/* ── F5: extract the selection ────────────────────────────────────── */

typedef struct {
    const char *archive, *base, *dest;
    const char *const *inner;
    guint  n;
    gint64 total;
    int    result;
} ArcCopy;

static void arc_copy_worker(Job *job, gpointer d)
{
    ArcCopy *c = d;
    job_set_phase(job, "Extracting…");
    job_set_total(job, c->total);
    c->result = arcfs_extract(&job->ctx, c->archive, c->base, c->inner, c->n, c->dest);
}

void arc_panel_copy_out(FM *fm)
{
    Panel *src = active_panel(fm);
    Panel *dst = other_panel(fm);
    if (dst->ssh_conn) {
        fm_status(fm, "Copy from an archive to an SFTP panel is not supported – extract locally first");
        return;
    }
    if (arc_panel_readonly_msg(dst)) return;

    GList *names = panel_selection(src);
    if (!names) { fm_status(fm, "Nothing is selected"); return; }

    gchar *prompt = dlg_count_prompt("Copy", names, "to:");
    gchar *dest = ask_destination(fm, "Copy", prompt, panel_dir(dst));
    g_free(prompt);
    if (dest && dest[0] != '/') {       /* relative → the archive's directory */
        gchar *abs = g_build_filename(src->cwd, dest, NULL);
        g_free(dest);
        dest = abs;
    }
    if (dest && !g_file_test(dest, G_FILE_TEST_IS_DIR)) {
        fm_status(fm, "Destination directory does not exist: %s", dest);
        g_clear_pointer(&dest, g_free);
    }
    if (!dest) {
        g_list_free_full(names, g_free);
        panel_focus(src);
        return;
    }

    GPtrArray *inner = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = names; l; l = l->next)
        g_ptr_array_add(inner, arc_inner(src, l->data));
    ArcCopy c = {
        .archive = src->arc_file,
        .base    = src->arc_dir,
        .dest    = dest,
        .inner   = (const char *const *)inner->pdata,
        .n       = inner->len,
    };
    c.total = arcfs_size(src->arc_entries, c.inner, c.n);

    Job *job = job_new(fm, "Extracting");
    job_run(job, arc_copy_worker, &c);
    int files = g_atomic_int_get(&job->ctx.files_done);
    if (job_cancelled(job) || c.result == FS_CANCELLED)
        fm_status(fm, "Extraction cancelled – %d file%s written", files, files == 1 ? "" : "s");
    else if (c.result == FS_ERR)
        fm_status(fm, "Extract error: %s", job->ctx.error ? job->ctx.error : "unknown");
    else
        fm_status(fm, "Extracted %d file%s to %s%s", files, files == 1 ? "" : "s",
                  dest, c.result == FS_SKIPPED ? " (some skipped)" : "");
    job_free(job);

    /* refresh panels showing the destination */
    for (int i = 0; i < 2; i++) {
        Panel *q = &fm->panels[i];
        if (!q->ssh_conn && !q->arc_file && !q->search_mode && strcmp(q->cwd, dest) == 0)
            panel_reload(q);
    }
    if (src->marks) g_hash_table_remove_all(src->marks);
    arc_panel_reload(src);

    g_ptr_array_unref(inner);
    g_list_free_full(names, g_free);
    g_free(dest);
    panel_focus(src);
}
