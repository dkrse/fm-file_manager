#include "fm.h"
#include "job.h"
#include "dialogs.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  File operations – dialogs on the main thread, the actual work in a
 *  Job worker thread using the fsops.c engine.
 * ─────────────────────────────────────────────────────────────────── */

/* Restore keyboard focus to the panel after a dialog / job */
static void refocus(Panel *p)
{
    panel_focus(p);
}

static void reload_both(FM *fm)
{
    panel_reload(&fm->panels[0]);
    panel_reload(&fm->panels[1]);
}

/* After a failed SFTP job: drop the panel if the connection is gone */
static gboolean conn_alive(Panel *p)
{
#ifdef HAVE_LIBSSH2
    if (!p->ssh_conn) return TRUE;
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (!ssh_conn_trylock(p->ssh_conn)) return TRUE;
    gboolean ok = libssh2_sftp_stat(p->ssh_conn->sftp,
                                    p->ssh_conn->remote_path, &a) == 0;
    ssh_conn_unlock(p->ssh_conn);
    if (!ok) {
        fm_status(p->fm, "SFTP connection lost – panel closed");
        ssh_panel_close(p);
    }
    return ok;
#else
    (void)p;
    return TRUE;
#endif
}

/* Status line summary: "Copied: 3  skipped: 1  errors: 1 – first error" */
static void report_result(FM *fm, const char *verb, Job *job,
                          int ok, int skipped, int fail)
{
    GString *s = g_string_new(NULL);
    if (job_cancelled(job)) g_string_append_printf(s, "%s cancelled – ", verb);
    g_string_append_printf(s, "%s: %d", verb, ok);
    if (skipped) g_string_append_printf(s, "  skipped: %d", skipped);
    if (fail)    g_string_append_printf(s, "  errors: %d", fail);
    if (job->ctx.error) g_string_append_printf(s, "  –  %s", job->ctx.error);
    fm_status(fm, "%s", s->str);
    g_string_free(s, TRUE);
}

static void tally(int r, int *ok, int *skipped, int *fail)
{
    switch (r) {
    case FS_OK:        (*ok)++;      break;
    case FS_SKIPPED:   (*skipped)++; break;
    case FS_CANCELLED:               break;
    default:           (*fail)++;    break;
    }
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Transfer (copy / move / paste) – local ↔ local / SFTP ↔ anything
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GList   *srcs;        /* full source paths                     */
    gchar   *dest;        /* destination directory                 */
    gchar   *rename_to;   /* single item: explicit target name     */
    SshConn *sconn;
    SshConn *dconn;
    gboolean move;
    int      ok, skipped, fail;
    int      worst;       /* worst FS_* result of all items        */
} Xfer;

#ifdef HAVE_LIBSSH2
static gboolean remote_exists(const char *dir, const char *name, gpointer user)
{
    SshConn *c = user;
    gchar *p = fs_remote_join(dir, name);
    gboolean r = fs_sftp_exists(c->sftp, p, NULL);
    g_free(p);
    return r;
}
#endif

static gint64 item_size(Job *job, SshConn *conn, const char *path)
{
#ifdef HAVE_LIBSSH2
    if (conn) return fs_sftp_calc_size(&job->ctx, conn->sftp, path);
#else
    (void)conn;
#endif
    return fs_calc_size(&job->ctx, path);
}

static int transfer_one(Job *job, Xfer *x, const char *src, const char *dst)
{
    FsCtx *c = &job->ctx;
    /* conflict dialog: no local stat() for remote paths */
    job->src_remote = x->sconn != NULL;
    job->dst_remote = x->dconn != NULL;

    if (!x->sconn && !x->dconn)
        return x->move ? fs_move(c, src, dst) : fs_copy(c, src, dst);

#ifdef HAVE_LIBSSH2
    int r;
    if (!x->sconn) {                                    /* upload */
        r = fs_sftp_upload(c, x->dconn->sftp, src, dst);
        if (x->move && r == FS_OK) r = fs_delete(c, src);
        return r;
    }
    if (!x->dconn) {                                    /* download */
        r = fs_sftp_download(c, x->sconn->sftp, src, dst);
        if (x->move && r == FS_OK) r = fs_sftp_delete(c, x->sconn->sftp, src);
        return r;
    }
    /* remote → remote through a private temp directory */
    if (x->sconn == x->dconn) {
        /* replacing an ancestor of the source would delete the source */
        gchar *ns = g_canonicalize_filename(src, "/");
        gchar *nd = g_canonicalize_filename(dst, "/");
        gboolean inside = fs_path_is_inside(ns, nd) || fs_path_is_inside(nd, ns);
        g_free(ns); g_free(nd);
        if (inside) {
            if (!c->error)
                c->error = g_strdup_printf("Cannot copy '%s' onto '%s'", src, dst);
            return FS_ERR;
        }
    }
    job->src_remote = FALSE;           /* upload reads the local temp copy */
    gchar *tmpdir = g_dir_make_tmp("fm-r2r-XXXXXX", NULL);
    if (!tmpdir) return FS_ERR;
    gchar *base = g_path_get_basename(src);
    gchar *tmp  = g_build_filename(tmpdir, base, NULL);
    r = fs_sftp_download(c, x->sconn->sftp, src, tmp);
    if (r == FS_OK) r = fs_sftp_upload(c, x->dconn->sftp, tmp, dst);
    if (x->move && r == FS_OK) r = fs_sftp_delete(c, x->sconn->sftp, src);
    fs_delete(NULL, tmpdir);
    g_free(base); g_free(tmp); g_free(tmpdir);
    return r;
#else
    (void)job; (void)src; (void)dst;
    return FS_ERR;
#endif
}

static void xfer_worker(Job *job, gpointer d)
{
    Xfer *x = d;
    if (x->sconn) ssh_conn_lock(x->sconn);
    if (x->dconn && x->dconn != x->sconn) ssh_conn_lock(x->dconn);

    job_set_phase(job, "Calculating size…");
    gint64 total = 0;
    for (GList *l = x->srcs; l && !job_cancelled(job); l = l->next) {
        gchar *base = g_path_get_basename(l->data);
        job_set_file(job, base);
        g_free(base);
        total += item_size(job, x->sconn, l->data);
    }
    if (x->sconn && x->dconn) total *= 2;     /* download + upload */

    if (!job_cancelled(job)) {
        job_set_phase(job, x->move ? "Moving…" : "Copying…");
        job_set_total(job, total);
    }

    for (GList *l = x->srcs; l && !job_cancelled(job); l = l->next) {
        const char *src = l->data;
        gchar *base = (x->rename_to && !x->srcs->next)
                      ? g_strdup(x->rename_to) : g_path_get_basename(src);
        if (!fs_name_is_safe(base)) {
            x->fail++;
            x->worst = fs_worst(x->worst, FS_ERR);
            g_free(base);
            continue;
        }

        gchar *dst = x->dconn ? fs_remote_join(x->dest, base)
                              : g_build_filename(x->dest, base, NULL);

        /* Same place: copy → duplicate under a new name, move → nothing */
        if (x->sconn == x->dconn && strcmp(src, dst) == 0) {
            if (x->move) {
                x->skipped++;
                x->worst = fs_worst(x->worst, FS_SKIPPED);
                g_free(base); g_free(dst);
                continue;
            }
            gchar *u;
#ifdef HAVE_LIBSSH2
            if (x->dconn) u = fs_unique_name(x->dest, base, remote_exists, x->dconn);
            else
#endif
                u = fs_unique_name(x->dest, base, fs_local_exists, NULL);
            g_free(dst);
            dst = x->dconn ? fs_remote_join(x->dest, u) : g_build_filename(x->dest, u, NULL);
            g_free(u);
        }

        int r = transfer_one(job, x, src, dst);
        x->worst = fs_worst(x->worst, r);
        tally(r, &x->ok, &x->skipped, &x->fail);
        g_free(base);
        g_free(dst);
    }

    if (x->dconn && x->dconn != x->sconn) ssh_conn_unlock(x->dconn);
    if (x->sconn) ssh_conn_unlock(x->sconn);
}

static Panel *panel_of_conn(FM *fm, SshConn *c)
{
    for (int i = 0; i < 2; i++)
        if (c && fm->panels[i].ssh_conn == c) return &fm->panels[i];
    return NULL;
}

static void run_transfer(FM *fm, Xfer *x)
{
    Job *job = job_new(fm, x->move ? "Moving" : "Copying");
    job_run(job, xfer_worker, x);
    if (job_cancelled(job)) x->worst = fs_worst(x->worst, FS_CANCELLED);

    report_result(fm, x->move ? "Moved" : "Copied", job, x->ok, x->skipped, x->fail);
    job_free(job);

    Panel *sp = panel_of_conn(fm, x->sconn);
    Panel *dp = panel_of_conn(fm, x->dconn);
    if (x->fail) {
        if (sp) conn_alive(sp);
        if (dp && dp != sp) conn_alive(dp);
    }
    reload_both(fm);
}

int fo_transfer(FM *fm, GList *paths, SshConn *sconn,
                const char *dest_dir, SshConn *dconn, gboolean move)
{
    if (!paths || !dest_dir) return FS_ERR;
    Xfer x = { 0 };
    for (GList *l = paths; l; l = l->next)
        x.srcs = g_list_prepend(x.srcs, g_strdup(l->data));
    x.srcs  = g_list_reverse(x.srcs);
    x.dest  = g_strdup(dest_dir);
    x.sconn = sconn;
    x.dconn = dconn;
    x.move  = move;
    run_transfer(fm, &x);
    g_list_free_full(x.srcs, g_free);
    g_free(x.dest);
    return x.worst;
}

/* Is `path` an existing directory on the given side? */
static gboolean is_dir_on(SshConn *conn, const char *path)
{
#ifdef HAVE_LIBSSH2
    if (conn) {
        gboolean d = FALSE, ok = FALSE;
        if (ssh_conn_trylock(conn)) {
            ok = fs_sftp_exists(conn->sftp, path, &d);
            ssh_conn_unlock(conn);
        }
        return ok && d;
    }
#else
    (void)conn;
#endif
    return g_file_test(path, G_FILE_TEST_IS_DIR);
}

/* F5 / F6 – Total Commander behaviour:
 *   dialog shows "<other panel>/" selected, Enter copies there;
 *   a single item may be given a new name ("/dst/newname"). */
static void copy_or_move(FM *fm, gboolean move)
{
    Panel *src = active_panel(fm);
    Panel *dst = other_panel(fm);

    /* archives: F5 extracts (arcpanel.c), nothing goes into one */
    if (src->arc_file) {
        if (move) fm_status(fm, "Archive is read-only – use F5 to copy out");
        else      arc_panel_copy_out(fm);
        return;
    }
    if (arc_panel_readonly_msg(dst)) return;

    GList *paths = panel_selection_paths(src);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }

    gchar *prompt = dlg_count_prompt(move ? "Rename/move" : "Copy", paths, "to:");
    gchar *dest = ask_destination(fm, move ? "Move" : "Copy", prompt, panel_dir(dst));
    g_free(prompt);
    if (!dest) { g_list_free_full(paths, g_free); refocus(src); return; }

    /* With an SFTP target panel the typed path is remote, unless it
     * names an existing local directory (user typed a local path). */
    SshConn *dconn = NULL;
    if (dst->ssh_conn && (strcmp(dest, panel_dir(dst)) == 0 ||
                          !g_file_test(dest, G_FILE_TEST_IS_DIR)))
        dconn = dst->ssh_conn;
    /* relative path → relative to the source panel (TC semantics) */
    if (!dconn && dest[0] != '/') {
        gchar *abs = g_build_filename(panel_dir(src), dest, NULL);
        g_free(dest);
        dest = abs;
    }

    Xfer x = { 0 };
    x.srcs  = paths;
    x.sconn = src->ssh_conn;
    x.dconn = dconn;
    x.move  = move;

    if (is_dir_on(dconn, dest)) {
        x.dest = dest;
        dest = NULL;
    } else {
        /* not a directory: "dir/newname" for a single item */
        gchar *parent = g_path_get_dirname(dest);
        gchar *name   = g_path_get_basename(dest);
        if (paths->next || !is_dir_on(dconn, parent) || !fs_name_is_safe(name)) {
            fm_status(fm, "Destination directory does not exist: %s",
                      paths->next ? dest : parent);
            g_free(parent); g_free(name); g_free(dest);
            g_list_free_full(paths, g_free);
            refocus(src);
            return;
        }
        x.dest = parent;
        x.rename_to = name;
    }

    run_transfer(fm, &x);
    if (src->marks) g_hash_table_remove_all(src->marks);
    panel_reload(src);
    g_free(x.dest);
    g_free(x.rename_to);
    g_free(dest);
    g_list_free_full(paths, g_free);
    refocus(src);
}

void fo_copy(FM *fm) { copy_or_move(fm, FALSE); }
void fo_move(FM *fm) { copy_or_move(fm, TRUE);  }

/* Shift+F5: duplicate in the same directory, asking for the new name */
void fo_copy_here(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    GList *paths = panel_selection_paths(p);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }

    Xfer x = { 0 };
    x.srcs  = paths;
    x.sconn = x.dconn = p->ssh_conn;

    if (!paths->next) {
        gchar *base = g_path_get_basename(paths->data);
        gchar *dir  = g_path_get_dirname(paths->data);
        gchar *suggest;
#ifdef HAVE_LIBSSH2
        if (p->ssh_conn) {
            ssh_conn_lock(p->ssh_conn);
            suggest = fs_unique_name(dir, base, remote_exists, p->ssh_conn);
            ssh_conn_unlock(p->ssh_conn);
        } else
#endif
            suggest = fs_unique_name(dir, base, fs_local_exists, NULL);
        gchar *mk = g_markup_printf_escaped("Copy <b>%s</b> as:", base);
        x.rename_to = ask_text(fm, "Copy", mk, suggest, "Copy", TRUE);
        g_free(mk); g_free(suggest); g_free(base);
        if (!x.rename_to) { g_free(dir); g_list_free_full(paths, g_free); refocus(p); return; }
        if (!fs_name_is_safe(x.rename_to)) {
            fm_status(fm, "Invalid name: %s", x.rename_to);
            g_free(x.rename_to); g_free(dir);
            g_list_free_full(paths, g_free);
            refocus(p);
            return;
        }
        x.dest = dir;
    } else {
        x.dest = g_path_get_dirname(paths->data);
    }

    run_transfer(fm, &x);
    if (x.rename_to) panel_select_name(p, x.rename_to);
    g_free(x.rename_to);
    g_free(x.dest);
    g_list_free_full(paths, g_free);
    refocus(p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Delete (trash or permanent)
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GList   *paths;
    SshConn *conn;
    gboolean trash;
    int      ok, skipped, fail;
} DelJob;

static void delete_worker(Job *job, gpointer d)
{
    DelJob *dj = d;
    job_set_phase(job, dj->trash ? "Moving to trash…" : "Deleting…");
    job_set_total_items(job, g_list_length(dj->paths));
    if (dj->conn) ssh_conn_lock(dj->conn);

    for (GList *l = dj->paths; l && !job_cancelled(job); l = l->next) {
        int r;
#ifdef HAVE_LIBSSH2
        if (dj->conn) r = fs_sftp_delete(&job->ctx, dj->conn->sftp, l->data);
        else
#endif
        r = dj->trash ? fs_trash(&job->ctx, l->data) : fs_delete(&job->ctx, l->data);
        tally(r, &dj->ok, &dj->skipped, &dj->fail);
        job_add_done(job, 1);
    }
    if (dj->conn) ssh_conn_unlock(dj->conn);
}

static void delete_impl(FM *fm, gboolean permanent)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    GList *paths = panel_selection_paths(p);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }

    /* F8/Del: local → trash, Shift = permanent; SFTP is always permanent */
    gboolean trash = !p->ssh_conn && !permanent;
    gchar *what = dlg_count_prompt(trash ? "Do you really want to move" : "Do you really want to delete",
                                   paths, trash ? "to the trash?" : "?");
    if (!trash) {
        gchar *t = g_strconcat(what, "\n<small>This cannot be undone.</small>", NULL);
        g_free(what);
        what = t;
    }
    gboolean go = ask_yes_no(fm, trash ? "Move to trash" : "Delete", what);
    g_free(what);
    if (!go) { g_list_free_full(paths, g_free); refocus(p); return; }

    DelJob dj = { .paths = paths, .conn = p->ssh_conn, .trash = trash };
    Job *job = job_new(fm, dj.trash ? "Moving to trash" : "Deleting");
    job_run(job, delete_worker, &dj);
    report_result(fm, dj.trash ? "Trashed" : "Deleted", job, dj.ok, dj.skipped, dj.fail);
    job_free(job);

    if (dj.fail) conn_alive(p);
    if (p->marks) g_hash_table_remove_all(p->marks);
    g_list_free_full(paths, g_free);
    reload_both(fm);
    refocus(p);
}

void fo_delete(FM *fm)           { delete_impl(fm, FALSE); }
void fo_delete_permanent(FM *fm) { delete_impl(fm, TRUE);  }

/* ─────────────────────────────────────────────────────────────────── *
 *  Mkdir (supports nested "a/b/c")
 * ─────────────────────────────────────────────────────────────────── */

void fo_mkdir(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    if (p->search_mode) { fm_status(fm, "Leave search results first"); return; }

    gchar *name = ask_text(fm, "New directory",
                           "New directory name (a/b/c creates nested):",
                           NULL, "Create", FALSE);
    if (!name) { refocus(p); return; }

    if (!fs_relpath_is_safe(name)) {
        fm_status(fm, "Invalid directory name: %s", name);
        g_free(name);
        refocus(p);
        return;
    }

    gboolean ok = FALSE;
#ifdef HAVE_LIBSSH2
    if (p->ssh_conn) {
        if (!ssh_conn_trylock(p->ssh_conn)) {
            fm_status(fm, "SFTP connection is busy");
        } else {
            gchar **parts = g_strsplit(name, "/", -1);
            gchar *cur = g_strdup(p->ssh_conn->remote_path);
            ok = TRUE;
            for (int i = 0; parts[i] && ok; i++) {
                if (!parts[i][0]) continue;
                gchar *next = fs_remote_join(cur, parts[i]);
                g_free(cur);
                cur = next;
                gboolean is_dir = FALSE;
                if (fs_sftp_exists(p->ssh_conn->sftp, cur, &is_dir) && is_dir
                    && parts[i + 1]) continue;
                if (libssh2_sftp_mkdir(p->ssh_conn->sftp, cur, 0755) != 0) ok = FALSE;
            }
            g_free(cur);
            g_strfreev(parts);
            ssh_conn_unlock(p->ssh_conn);
            if (!ok) fm_status(fm, "Error creating remote directory: %s", name);
        }
    } else
#endif
    {
        gchar *path = g_build_filename(p->cwd, name, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            fm_status(fm, "Already exists: %s", name);
        else if (g_mkdir_with_parents(path, 0755) == 0)
            ok = TRUE;
        else
            fm_status(fm, "Error creating directory: %s", g_strerror(errno));
        g_free(path);
    }

    if (ok) {
        fm_status(fm, "Directory created: %s", name);
        reload_both(fm);
        gchar **parts = g_strsplit(name, "/", 2);
        panel_select_name(p, parts[0]);
        g_strfreev(parts);
    }
    g_free(name);
    refocus(p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  New file (Shift+F4) – create empty file and open it in the editor
 * ─────────────────────────────────────────────────────────────────── */

void fo_newfile(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    if (p->search_mode) { fm_status(fm, "Leave search results first"); return; }

    gchar *name = ask_text(fm, "New file", "New file name:", "new.txt", "Create", TRUE);
    if (!name) { refocus(p); return; }
    if (!fs_name_is_safe(name)) {
        fm_status(fm, "Invalid file name: %s", name);
        g_free(name);
        refocus(p);
        return;
    }

    gboolean ok = FALSE;
#ifdef HAVE_LIBSSH2
    if (p->ssh_conn) {
        if (ssh_conn_trylock(p->ssh_conn)) {
            gchar *rp = fs_remote_join(p->ssh_conn->remote_path, name);
            LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_open(p->ssh_conn->sftp, rp,
                LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL, 0644);
            if (h) { libssh2_sftp_close(h); ok = TRUE; }
            ssh_conn_unlock(p->ssh_conn);
            g_free(rp);
        }
        if (!ok) fm_status(fm, "Cannot create remote file (exists or busy): %s", name);
    } else
#endif
    {
        gchar *path = g_build_filename(p->cwd, name, NULL);
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (fd >= 0) { close(fd); ok = TRUE; }
        else fm_status(fm, "Cannot create %s: %s", name, g_strerror(errno));
        g_free(path);
    }

    if (ok) {
        panel_reload(p);
        panel_select_name(p, name);
        fm_status(fm, "Created: %s", name);
        editor_show(fm);
    } else {
        refocus(p);
    }
    g_free(name);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Rename (Shift+F6)
 * ─────────────────────────────────────────────────────────────────── */

void fo_rename(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    gchar *old_name = panel_cursor_name(p);
    if (!old_name || strcmp(old_name, "..") == 0) {
        g_free(old_name);
        fm_status(fm, "No file is selected");
        return;
    }

    /* Directory that contains the item (search results live elsewhere) */
    gchar *dir;
    if (p->ssh_conn) {
        dir = g_strdup(p->ssh_conn->remote_path);
    } else {
        gchar *full = panel_cursor_fullpath(p);
        dir = g_path_get_dirname(full);
        g_free(full);
    }

    gchar *mk = g_markup_printf_escaped("Rename <b>%s</b> to:", old_name);
    gchar *new_name = ask_text(fm, "Rename", mk, old_name, "Rename", TRUE);
    g_free(mk);

    gboolean done = FALSE;
    if (new_name && strcmp(new_name, old_name) != 0) {
        if (!fs_name_is_safe(new_name)) {
            fm_status(fm, "Invalid name: %s", new_name);
            goto out;
        }
#ifdef HAVE_LIBSSH2
        if (p->ssh_conn) {
            if (!ssh_conn_trylock(p->ssh_conn)) {
                fm_status(fm, "SFTP connection is busy");
                goto out;
            }
            gchar *op = fs_remote_join(dir, old_name);
            gchar *np = fs_remote_join(dir, new_name);
            LIBSSH2_SFTP *sftp = p->ssh_conn->sftp;
            gboolean exists_dir = FALSE, go = TRUE, replace = FALSE;
            /* case-only rename: on a case-insensitive server np "exists"
             * but is the source itself – never replace/unlink it */
            if (g_ascii_strcasecmp(old_name, new_name) != 0 &&
                fs_sftp_exists(sftp, np, &exists_dir)) {
                gchar *q = g_markup_printf_escaped("<b>%s</b> already exists. Replace it?", new_name);
                static const char *const b[] = { "Cancel", "Replace", NULL };
                go = replace = !exists_dir && ask_choice(fm, "Rename", q, NULL, b, 1) == 1;
                g_free(q);
                if (exists_dir) fm_status(fm, "A directory with that name exists");
            }
            if (go) {
                /* the old target is only removed if an overwriting
                 * rename is refused (SFTP v3) */
                done = replace ? fs_sftp_replace(sftp, op, np)
                               : libssh2_sftp_rename(sftp, op, np) == 0;
                if (!done) fm_status(fm, "Error renaming remote file");
            }
            ssh_conn_unlock(p->ssh_conn);
            g_free(op); g_free(np);
        } else
#endif
        {
            gchar *op = g_build_filename(dir, old_name, NULL);
            gchar *np = g_build_filename(dir, new_name, NULL);
            struct stat st;
            gboolean go = TRUE;
            /* case-only rename on case-insensitive FS is the same inode – fine */
            if (lstat(np, &st) == 0 && g_ascii_strcasecmp(old_name, new_name) != 0) {
                gchar *q = g_markup_printf_escaped("<b>%s</b> already exists. Replace it?", new_name);
                static const char *const b[] = { "Cancel", "Replace", NULL };
                go = ask_choice(fm, "Rename", q, NULL, b, 1) == 1;
                g_free(q);
            }
            if (go) {
                if (rename(op, np) == 0) done = TRUE;
                else fm_status(fm, "Error renaming: %s", g_strerror(errno));
            }
            g_free(op); g_free(np);
        }
    }

    if (done) {
        fm_status(fm, "Renamed: %s → %s", old_name, new_name);
        if (p->marks) g_hash_table_remove(p->marks, old_name);
        reload_both(fm);
        panel_select_name(p, new_name);
    }
out:
    g_free(new_name);
    g_free(old_name);
    g_free(dir);
    refocus(p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Properties (Alt+Enter) – info + permissions editor
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *checks[9];
    GtkWidget *octal;
} PermUi;

static guint perm_bits(PermUi *u)
{
    guint m = 0;
    for (int i = 0; i < 9; i++)
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(u->checks[i])))
            m |= 1u << (8 - i);
    return m;
}

static void on_perm_toggled(GtkCheckButton *b, PermUi *u)
{
    (void)b;
    gchar *t = g_strdup_printf("%03o", perm_bits(u));
    gtk_label_set_text(GTK_LABEL(u->octal), t);
    g_free(t);
}

/* Directory size in a thread; label is ref'd so it survives the dialog */
typedef struct { gchar *path; FsCtx ctx; GtkWidget *label; } SizeTask;

static void size_task_free(gpointer d)
{
    SizeTask *t = d;
    g_free(t->path);
    g_object_unref(t->label);
    g_free(t);
}

static void size_thread(GTask *task, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    SizeTask *t = data;
    gint64 *r = g_new(gint64, 1);
    *r = fs_calc_size(&t->ctx, t->path);
    g_task_return_pointer(task, r, g_free);
}

static void size_done(GObject *src, GAsyncResult *res, gpointer d)
{
    (void)src; (void)d;
    SizeTask *t = g_task_get_task_data(G_TASK(res));
    gint64 *sz = g_task_propagate_pointer(G_TASK(res), NULL);
    if (sz && gtk_widget_get_root(t->label)) {
        gchar *h = fmt_size(*sz);
        gchar *s = g_strdup_printf("%s (%'" G_GINT64_FORMAT " bytes)", h, *sz);
        gtk_label_set_text(GTK_LABEL(t->label), s);
        g_free(s); g_free(h);
    }
    g_free(sz);
}

static void grid_row(GtkWidget *grid, int row, const char *key, GtkWidget *val)
{
    GtkWidget *k = gtk_label_new(key);
    gtk_widget_set_halign(k, GTK_ALIGN_END);
    gtk_widget_add_css_class(k, "dim-label");
    gtk_widget_set_halign(val, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), k,   0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), val, 1, row, 1, 1);
}

static GtkWidget *sel_label(const char *text)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 50);
    return l;
}

void fo_properties(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    gchar *name = panel_cursor_name(p);
    if (!name || strcmp(name, "..") == 0) { g_free(name); return; }

    gchar *path;
    guint  mode = 0;
    gint64 size = 0, mtime = 0;
    gboolean is_dir = FALSE, is_link = FALSE, have = FALSE;
    gchar *owner = NULL, *link_target = NULL;

#ifdef HAVE_LIBSSH2
    if (p->ssh_conn) {
        path = fs_remote_join(p->ssh_conn->remote_path, name);
        if (ssh_conn_trylock(p->ssh_conn)) {
            LIBSSH2_SFTP_ATTRIBUTES a;
            if (libssh2_sftp_lstat(p->ssh_conn->sftp, path, &a) == 0) {
                have = TRUE;
                mode    = a.permissions;
                size    = (gint64)a.filesize;
                mtime   = (gint64)a.mtime;
                is_dir  = LIBSSH2_SFTP_S_ISDIR(a.permissions);
                is_link = LIBSSH2_SFTP_S_ISLNK(a.permissions);
                owner   = g_strdup_printf("%lu:%lu", a.uid, a.gid);
                if (is_link) {
                    char buf[4096];
                    int n = libssh2_sftp_readlink(p->ssh_conn->sftp, path, buf, sizeof(buf) - 1);
                    if (n >= 0) { buf[n] = '\0'; link_target = g_strdup(buf); }
                }
            }
            ssh_conn_unlock(p->ssh_conn);
        }
    } else
#endif
    {
        path = panel_cursor_fullpath(p);
        struct stat st;
        if (path && lstat(path, &st) == 0) {
            have = TRUE;
            mode    = st.st_mode;
            size    = st.st_size;
            mtime   = st.st_mtime;
            is_dir  = S_ISDIR(st.st_mode);
            is_link = S_ISLNK(st.st_mode);
            struct passwd *pw = getpwuid(st.st_uid);
            struct group  *gr = getgrgid(st.st_gid);
            owner = g_strdup_printf("%s : %s",
                                    pw ? pw->pw_name : "?", gr ? gr->gr_name : "?");
            if (is_link) link_target = g_file_read_link(path, NULL);
        }
    }
    if (!have) {
        fm_status(fm, "Cannot read properties of %s", name);
        g_free(name); g_free(path);
        return;
    }

    DlgCtx ctx;
    dlg_ctx_init(&ctx);
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Properties");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 420, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    int row = 0;

    grid_row(grid, row++, "Name:", sel_label(name));
    gchar *loc = g_path_get_dirname(path);
    grid_row(grid, row++, "Location:", sel_label(loc));
    g_free(loc);
    grid_row(grid, row++, "Type:", gtk_label_new(is_link ? "Symbolic link"
                                               : is_dir ? "Directory" : "File"));
    if (link_target) grid_row(grid, row++, "Target:", sel_label(link_target));

    GtkWidget *size_lbl;
    SizeTask *st_task = NULL;
    GTask    *size_task = NULL;      /* kept alive so st_task stays valid */
    if (is_dir && !p->ssh_conn) {
        size_lbl = gtk_label_new("calculating…");
        st_task = g_new0(SizeTask, 1);
        st_task->path  = g_strdup(path);
        st_task->label = g_object_ref(size_lbl);
        size_task = g_task_new(NULL, NULL, size_done, NULL);
        g_task_set_task_data(size_task, st_task, size_task_free);
        g_task_run_in_thread(size_task, size_thread);
    } else if (is_dir) {
        size_lbl = gtk_label_new("–");
    } else {
        gchar *h = fmt_size(size);
        gchar *s = g_strdup_printf("%s (%'" G_GINT64_FORMAT " bytes)", h, size);
        size_lbl = gtk_label_new(s);
        g_free(s); g_free(h);
    }
    grid_row(grid, row++, "Size:", size_lbl);

    gchar *ds = fmt_date(mtime);
    grid_row(grid, row++, "Modified:", gtk_label_new(ds));
    g_free(ds);
    grid_row(grid, row++, "Owner:", gtk_label_new(owner ? owner : "?"));
    gtk_box_append(GTK_BOX(vbox), grid);

    /* permissions grid: rows owner/group/others, cols r/w/x */
    PermUi pu;
    GtkWidget *pgrid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(pgrid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(pgrid), 2);
    static const char *who[] = { "Owner", "Group", "Others" };
    static const char *what[] = { "Read", "Write", "Execute" };
    for (int c = 0; c < 3; c++)
        gtk_grid_attach(GTK_GRID(pgrid), gtk_label_new(what[c]), c + 1, 0, 1, 1);
    for (int r = 0; r < 3; r++) {
        GtkWidget *wl = gtk_label_new(who[r]);
        gtk_widget_set_halign(wl, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(pgrid), wl, 0, r + 1, 1, 1);
        for (int c = 0; c < 3; c++) {
            int i = r * 3 + c;
            pu.checks[i] = gtk_check_button_new();
            gtk_widget_set_halign(pu.checks[i], GTK_ALIGN_CENTER);
            gtk_check_button_set_active(GTK_CHECK_BUTTON(pu.checks[i]),
                                        (mode >> (8 - i)) & 1);
            gtk_widget_set_sensitive(pu.checks[i], !is_link);
            gtk_grid_attach(GTK_GRID(pgrid), pu.checks[i], c + 1, r + 1, 1, 1);
        }
    }
    pu.octal = gtk_label_new(NULL);
    on_perm_toggled(NULL, &pu);
    for (int i = 0; i < 9; i++)
        g_signal_connect(pu.checks[i], "toggled", G_CALLBACK(on_perm_toggled), &pu);
    gtk_grid_attach(GTK_GRID(pgrid), pu.octal, 4, 1, 1, 1);

    GtkWidget *pframe = gtk_frame_new("Permissions");
    gtk_widget_set_margin_top(pgrid, 6);
    gtk_widget_set_margin_bottom(pgrid, 6);
    gtk_widget_set_margin_start(pgrid, 8);
    gtk_frame_set_child(GTK_FRAME(pframe), pgrid);
    gtk_box_append(GTK_BOX(vbox), pframe);

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bb, GTK_ALIGN_END);
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    GtkWidget *apply_btn = gtk_button_new_with_label("Apply");
    gtk_widget_add_css_class(apply_btn, "suggested-action");
    gtk_widget_set_sensitive(apply_btn, !is_link);
    gtk_box_append(GTK_BOX(bb), close_btn);
    gtk_box_append(GTK_BOX(bb), apply_btn);
    gtk_box_append(GTK_BOX(vbox), bb);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(apply_btn, "clicked",       G_CALLBACK(dlg_ctx_ok),     &ctx);
    g_signal_connect(close_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel), &ctx);
    g_signal_connect(dlg,       "close-request", G_CALLBACK(dlg_ctx_close),  &ctx);
    gtk_window_present(GTK_WINDOW(dlg));

    if (dlg_ctx_run(&ctx) == 1) {
        guint nm = (mode & ~0777u) | perm_bits(&pu);
        gboolean ok = FALSE;
#ifdef HAVE_LIBSSH2
        if (p->ssh_conn) {
            if (ssh_conn_trylock(p->ssh_conn)) {
                LIBSSH2_SFTP_ATTRIBUTES a = { 0 };
                a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
                a.permissions = nm;
                ok = libssh2_sftp_setstat(p->ssh_conn->sftp, path, &a) == 0;
                ssh_conn_unlock(p->ssh_conn);
            }
        } else
#endif
        ok = chmod(path, nm & 07777) == 0;
        if (ok) { fm_status(fm, "Permissions of %s set to %03o", name, nm & 0777); panel_reload(p); }
        else      fm_status(fm, "Cannot change permissions of %s", name);
    }
    if (size_task) {
        fs_cancel(&st_task->ctx);                /* stop the size walk */
        g_object_unref(size_task);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));

    g_free(owner); g_free(link_target);
    g_free(name); g_free(path);
    refocus(p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Browse-button callback (GtkFileDialog async) – shared
 * ─────────────────────────────────────────────────────────────────── */

static void on_browse_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkFileDialog *fd = GTK_FILE_DIALOG(source);
    GtkWidget *entry = GTK_WIDGET(user_data);
    GFile *file = gtk_file_dialog_select_folder_finish(fd, res, NULL);
    if (file) {
        gchar *path = g_file_get_path(file);
        if (path) {
            gtk_editable_set_text(GTK_EDITABLE(entry), path);
            g_free(path);
        }
        g_object_unref(file);
    }
}

void on_browse_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *entry = GTK_WIDGET(user_data);
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Select destination directory");

    const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (cur && cur[0] == '/' && g_file_test(cur, G_FILE_TEST_IS_DIR)) {
        GFile *f = g_file_new_for_path(cur);
        gtk_file_dialog_set_initial_folder(fd, f);
        g_object_unref(f);
    }

    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_file_dialog_select_folder(fd, GTK_WINDOW(toplevel), NULL,
                                   on_browse_done, entry);
    g_object_unref(fd);
}
