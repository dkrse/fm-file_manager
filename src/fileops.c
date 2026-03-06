#include "fm.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>

#define COPY_BUF (256 * 1024)

/* ─────────────────────────────────────────────────────────────────── *
 *  Progress dialog (GTK4 – custom window + GMainLoop)
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *window;
    GtkWidget *label;
    GtkWidget *bar;
} ProgressDlg;

static ProgressDlg *progress_new(GtkWindow *parent, const char *title)
{
    ProgressDlg *pd = g_new0(ProgressDlg, 1);
    pd->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(pd->window), title);
    gtk_window_set_modal(GTK_WINDOW(pd->window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(pd->window), parent);
    gtk_window_set_resizable(GTK_WINDOW(pd->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(pd->window), 400, -1);
    gtk_window_set_deletable(GTK_WINDOW(pd->window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);

    pd->label = gtk_label_new("");
    gtk_widget_set_halign(pd->label, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(pd->label), PANGO_ELLIPSIZE_MIDDLE);
    pd->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pd->bar), TRUE);

    gtk_box_append(GTK_BOX(vbox), pd->label);
    gtk_box_append(GTK_BOX(vbox), pd->bar);
    gtk_window_set_child(GTK_WINDOW(pd->window), vbox);
    gtk_window_present(GTK_WINDOW(pd->window));
    /* pump event loop so the window actually renders before first update */
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    return pd;
}

static void progress_update(ProgressDlg *pd, const char *text, double frac)
{
    gtk_label_set_text(GTK_LABEL(pd->label), text);
    double f = CLAMP(frac, 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pd->bar), f);
    gchar *pct = g_strdup_printf("%.0f %%", f * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(pd->bar), pct);
    g_free(pct);
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void progress_free(ProgressDlg *pd)
{
    gtk_window_destroy(GTK_WINDOW(pd->window));
    g_free(pd);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Low-level helpers with byte-level progress
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    ProgressDlg *pd;
    gint64       total_bytes;
    gint64       done_bytes;
    const char  *current_name;
} CopyProgress;

static void cp_tick(CopyProgress *cp, gint64 chunk)
{
    if (!cp || !cp->pd) return;
    cp->done_bytes += chunk;
    double frac = cp->total_bytes > 0
                  ? (double)cp->done_bytes / cp->total_bytes : 0.0;
    gchar *s_done = fmt_size(cp->done_bytes);
    gchar *s_total = fmt_size(cp->total_bytes);
    gchar *msg = g_strdup_printf("%s  (%s / %s)",
        cp->current_name ? cp->current_name : "", s_done, s_total);
    progress_update(cp->pd, msg, frac);
    g_free(msg); g_free(s_done); g_free(s_total);
}

static gint64 calc_size_r(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return st.st_size;

    gint64 total = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *de;
    char sub[4096];
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        g_snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        total += calc_size_r(sub);
    }
    closedir(dir);
    return total;
}

static int copy_file(const char *src, const char *dst, CopyProgress *cp)
{
    int fin = open(src, O_RDONLY);
    if (fin < 0) return -1;

    struct stat st;
    fstat(fin, &st);

    int fout = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777);
    if (fout < 0) { close(fin); return -1; }

    char *buf = g_malloc(COPY_BUF);
    ssize_t n;
    int err = 0;
    while ((n = read(fin, buf, COPY_BUF)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = write(fout, buf + w, (size_t)(n - w));
            if (r < 0) { err = -1; break; }
            w += r;
        }
        if (err) break;
        cp_tick(cp, n);
    }
    if (n < 0) err = -1;
    g_free(buf);
    close(fin);
    close(fout);
    return err;
}

static int copy_dir_r(const char *src, const char *dst, CopyProgress *cp)
{
    if (mkdir(dst, 0755) != 0 && errno != EEXIST) return -1;

    DIR *dir = opendir(src);
    if (!dir) return -1;

    struct dirent *de;
    char sp[4096], dp[4096];
    struct stat st;
    int err = 0;

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        g_snprintf(sp, sizeof(sp), "%s/%s", src, de->d_name);
        g_snprintf(dp, sizeof(dp), "%s/%s", dst, de->d_name);
        if (lstat(sp, &st) != 0) continue;
        err |= S_ISDIR(st.st_mode) ? copy_dir_r(sp, dp, cp) : copy_file(sp, dp, cp);
    }
    closedir(dir);
    return err;
}

static int delete_r(const char *path, ProgressDlg *pd, int *del_count)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        int r = unlink(path);
        if (pd && del_count) {
            (*del_count)++;
            if (*del_count % 50 == 0) {
                gchar *msg = g_strdup_printf("Deleted: %d files", *del_count);
                gtk_label_set_text(GTK_LABEL(pd->label), msg);
                g_free(msg);
                gtk_progress_bar_pulse(GTK_PROGRESS_BAR(pd->bar));
                while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
            }
        }
        return r;
    }

    DIR *dir = opendir(path);
    if (!dir) return -1;

    struct dirent *de;
    char sub[4096];
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        g_snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        delete_r(sub, pd, del_count);
    }
    closedir(dir);
    return rmdir(path);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  SFTP transfer helpers (only compiled with libssh2)
 * ─────────────────────────────────────────────────────────────────── */

#ifdef HAVE_LIBSSH2

static int sftp_upload(SshConn *conn, const char *local_path,
                        const char *remote_path, CopyProgress *cp)
{
    if (!conn || !conn->sftp) return -1;

    int fin = open(local_path, O_RDONLY);
    if (fin < 0) return -1;

    LIBSSH2_SFTP_HANDLE *fout = libssh2_sftp_open(conn->sftp, remote_path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!fout) { close(fin); return -1; }

    char *buf = g_malloc(COPY_BUF);
    ssize_t n;
    int err = 0;
    while ((n = read(fin, buf, COPY_BUF)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = libssh2_sftp_write(fout, buf + w, (size_t)(n - w));
            if (r < 0) { err = -1; break; }
            w += r;
        }
        if (err) break;
        cp_tick(cp, n);
    }
    if (n < 0) err = -1;
    g_free(buf);
    close(fin);
    libssh2_sftp_close(fout);
    return err;
}

static int sftp_download(SshConn *conn, const char *remote_path,
                          const char *local_path, CopyProgress *cp)
{
    if (!conn || !conn->sftp) return -1;

    LIBSSH2_SFTP_HANDLE *fin = libssh2_sftp_open(conn->sftp, remote_path,
        LIBSSH2_FXF_READ, 0);
    if (!fin) return -1;

    int fout = open(local_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fout < 0) { libssh2_sftp_close(fin); return -1; }

    char *buf = g_malloc(COPY_BUF);
    ssize_t n;
    int err = 0;
    while ((n = libssh2_sftp_read(fin, buf, COPY_BUF)) > 0) {
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = write(fout, buf + w, (size_t)(n - w));
            if (r < 0) { err = -1; break; }
            w += r;
        }
        if (err) break;
        cp_tick(cp, n);
    }
    if (n < 0) err = -1;
    g_free(buf);
    libssh2_sftp_close(fin);
    close(fout);
    return err;
}

#endif /* HAVE_LIBSSH2 */

/* ─────────────────────────────────────────────────────────────────── *
 *  Browse-button callback (GtkFileDialog async)
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
    gtk_file_dialog_set_title(fd, "Select target directory");

    const char *cur = gtk_editable_get_text(GTK_EDITABLE(entry));
    if (cur && cur[0]) {
        GFile *f = g_file_new_for_path(cur);
        gtk_file_dialog_set_initial_folder(fd, f);
        g_object_unref(f);
    }

    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_file_dialog_select_folder(fd, GTK_WINDOW(toplevel), NULL,
                                   on_browse_done, entry);
    g_object_unref(fd);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Destination dialog (copy / move)
 * ─────────────────────────────────────────────────────────────────── */

static gchar *ask_destination(FM *fm, const char *title, const char *default_dst)
{
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *lbl = gtk_label_new("Target directory:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    GtkWidget *entry = gtk_entry_new();
    if (default_dst) gtk_editable_set_text(GTK_EDITABLE(entry), default_dst);
    gtk_widget_set_size_request(entry, 380, -1);
    gtk_widget_set_hexpand(entry, TRUE);

    GtkWidget *browse = gtk_button_new_with_label("...");
    gtk_widget_set_tooltip_text(browse, "Select directory");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), entry);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(hbox), entry);
    gtk_box_append(GTK_BOX(hbox), browse);

    /* Buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn     = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), hbox);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    gchar *result = NULL;
    if (dlg_ctx_run(&ctx) == 1) {
        const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (txt && txt[0]) result = g_strdup(txt);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    return result;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Copy
 * ─────────────────────────────────────────────────────────────────── */

void fo_copy(FM *fm)
{
    Panel *src = active_panel(fm);
    Panel *dst = other_panel(fm);

#ifdef HAVE_LIBSSH2
    if (src->ssh_conn && dst->ssh_conn) {
        fm_status(fm, "Remote→Remote copy is not supported");
        return;
    }
#endif

    GList *targets = panel_selection(src);
    if (!targets) { fm_status(fm, "Nothing selected"); return; }

    const char *default_dst =
#ifdef HAVE_LIBSSH2
        (dst->ssh_conn ? dst->ssh_conn->remote_path : dst->cwd);
#else
        dst->cwd;
#endif
    gchar *dest = ask_destination(fm, "Copy to", default_dst);
    if (!dest) { g_list_free_full(targets, g_free); return; }

    int ok = 0, fail = 0;
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), "Copying...");

    /* Pre-calculate total bytes for progress */
    gint64 total_bytes = 0;
#ifdef HAVE_LIBSSH2
    if (src->ssh_conn) {
        /* SSH download: get sizes via SFTP stat */
        for (GList *l = targets; l; l = l->next) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path,
                                        (const char *)l->data);
            LIBSSH2_SFTP_ATTRIBUTES a;
            if (libssh2_sftp_stat(src->ssh_conn->sftp, rp, &a) == 0)
                total_bytes += (gint64)a.filesize;
            g_free(rp);
        }
    } else if (dst->ssh_conn) {
        /* SSH upload: local sizes */
        for (GList *l = targets; l; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            total_bytes += calc_size_r(sp);
            g_free(sp);
        }
    } else
#endif
    {
        for (GList *l = targets; l; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            total_bytes += calc_size_r(sp);
            g_free(sp);
        }
    }

    CopyProgress cp = { .pd = pd, .total_bytes = total_bytes, .done_bytes = 0 };

    for (GList *l = targets; l; l = l->next) {
        const char *name = l->data;
        cp.current_name = name;

        int r = -1;

#ifdef HAVE_LIBSSH2
        if (src->ssh_conn) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path, name);
            gchar *lp = g_build_filename(dest, name, NULL);
            r = sftp_download(src->ssh_conn, rp, lp, &cp);
            g_free(rp); g_free(lp);
        } else if (dst->ssh_conn) {
            gchar *lp = g_build_filename(src->cwd, name, NULL);
            gchar *rp = g_strdup_printf("%s/%s", dest, name);
            r = sftp_upload(dst->ssh_conn, lp, rp, &cp);
            g_free(lp); g_free(rp);
        } else
#endif
        {
            gchar *sp = g_build_filename(src->cwd, name, NULL);
            gchar *dp = g_build_filename(dest,     name, NULL);
            struct stat st;
            r = (lstat(sp, &st) == 0 && S_ISDIR(st.st_mode))
                ? copy_dir_r(sp, dp, &cp) : copy_file(sp, dp, &cp);
            g_free(sp); g_free(dp);
        }

        if (r == 0) ok++; else fail++;
    }

    progress_update(pd, "Done", 1.0);
    progress_free(pd);
    g_list_free_full(targets, g_free);
    g_free(dest);

    fm_status(fm, "Copied: %d  errors: %d", ok, fail);

#ifdef HAVE_LIBSSH2
    if (src->ssh_conn && fail > 0) {
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (libssh2_sftp_stat(src->ssh_conn->sftp, src->ssh_conn->remote_path, &a) != 0) {
            ssh_panel_close(src);
            panel_reload(dst);
            return;
        }
    }
    if (dst->ssh_conn && fail > 0) {
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (libssh2_sftp_stat(dst->ssh_conn->sftp, dst->ssh_conn->remote_path, &a) != 0) {
            ssh_panel_close(dst);
            panel_reload(src);
            return;
        }
    }
#endif

    panel_reload(src);
    panel_reload(dst);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Move
 * ─────────────────────────────────────────────────────────────────── */

void fo_move(FM *fm)
{
    Panel *src = active_panel(fm);
    Panel *dst = other_panel(fm);

#ifdef HAVE_LIBSSH2
    if (src->ssh_conn && dst->ssh_conn) {
        fm_status(fm, "Remote→Remote move is not supported");
        return;
    }
#endif

    GList *targets = panel_selection(src);
    if (!targets) { fm_status(fm, "Nothing selected"); return; }

    const char *default_dst =
#ifdef HAVE_LIBSSH2
        (dst->ssh_conn ? dst->ssh_conn->remote_path : dst->cwd);
#else
        dst->cwd;
#endif
    gchar *dest = ask_destination(fm, "Move to", default_dst);
    if (!dest) { g_list_free_full(targets, g_free); return; }

    int ok = 0, fail = 0;
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), "Moving...");

    /* Pre-calculate total bytes for progress */
    gint64 total_bytes = 0;
#ifdef HAVE_LIBSSH2
    if (src->ssh_conn) {
        for (GList *l = targets; l; l = l->next) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path,
                                        (const char *)l->data);
            LIBSSH2_SFTP_ATTRIBUTES a;
            if (libssh2_sftp_stat(src->ssh_conn->sftp, rp, &a) == 0)
                total_bytes += (gint64)a.filesize;
            g_free(rp);
        }
    } else if (dst->ssh_conn) {
        for (GList *l = targets; l; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            total_bytes += calc_size_r(sp);
            g_free(sp);
        }
    } else
#endif
    {
        for (GList *l = targets; l; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            total_bytes += calc_size_r(sp);
            g_free(sp);
        }
    }

    CopyProgress cp = { .pd = pd, .total_bytes = total_bytes, .done_bytes = 0 };

    for (GList *l = targets; l; l = l->next) {
        const char *name = l->data;
        cp.current_name = name;

        int r = -1;

#ifdef HAVE_LIBSSH2
        if (src->ssh_conn) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path, name);
            gchar *lp = g_build_filename(dest, name, NULL);
            r = sftp_download(src->ssh_conn, rp, lp, &cp);
            if (r == 0)
                libssh2_sftp_unlink(src->ssh_conn->sftp, rp);
            g_free(rp); g_free(lp);
        } else if (dst->ssh_conn) {
            gchar *lp = g_build_filename(src->cwd, name, NULL);
            gchar *rp = g_strdup_printf("%s/%s", dest, name);
            r = sftp_upload(dst->ssh_conn, lp, rp, &cp);
            if (r == 0) delete_r(lp, NULL, NULL);
            g_free(lp); g_free(rp);
        } else
#endif
        {
            gchar *sp = g_build_filename(src->cwd, name, NULL);
            gchar *dp = g_build_filename(dest,     name, NULL);
            r = 0;
            if (rename(sp, dp) != 0) {
                if (errno == EXDEV) {
                    struct stat st;
                    r = (lstat(sp, &st) == 0 && S_ISDIR(st.st_mode))
                        ? copy_dir_r(sp, dp, &cp) : copy_file(sp, dp, &cp);
                    if (r == 0) delete_r(sp, NULL, NULL);
                } else {
                    r = -1;
                }
            }
            g_free(sp); g_free(dp);
        }

        if (r == 0) ok++; else fail++;
    }

    progress_free(pd);
    g_list_free_full(targets, g_free);
    g_free(dest);

    fm_status(fm, "Moved: %d  errors: %d", ok, fail);

#ifdef HAVE_LIBSSH2
    if (src->ssh_conn && fail > 0) {
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (libssh2_sftp_stat(src->ssh_conn->sftp, src->ssh_conn->remote_path, &a) != 0) {
            ssh_panel_close(src);
            panel_reload(dst);
            return;
        }
    }
    if (dst->ssh_conn && fail > 0) {
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (libssh2_sftp_stat(dst->ssh_conn->sftp, dst->ssh_conn->remote_path, &a) != 0) {
            ssh_panel_close(dst);
            panel_reload(src);
            return;
        }
    }
#endif

    panel_reload(src);
    panel_reload(dst);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Delete
 * ─────────────────────────────────────────────────────────────────── */

void fo_delete(FM *fm)
{
    Panel *p = active_panel(fm);
    GList *targets = panel_selection(p);
    if (!targets) { fm_status(fm, "Nothing selected"); return; }

    int n = g_list_length(targets);
    gchar *question = (n == 1)
        ? g_strdup_printf("Delete '%s'?", (char *)targets->data)
        : g_strdup_printf("Delete %d items?", n);

    /* Confirmation dialog */
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Confirm deletion");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 400, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);

    GtkWidget *q_lbl = gtk_label_new(question);
    gtk_label_set_wrap(GTK_LABEL(q_lbl), TRUE);
    gtk_widget_set_halign(q_lbl, GTK_ALIGN_START);
    g_free(question);

    GtkWidget *warn = gtk_label_new("This action cannot be undone.");
    gtk_widget_set_halign(warn, GTK_ALIGN_START);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn     = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(ok_btn, "destructive-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), q_lbl);
    gtk_box_append(GTK_BOX(vbox), warn);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),     &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),  &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    gboolean go = (dlg_ctx_run(&ctx) == 1);
    gtk_window_destroy(GTK_WINDOW(dlg));

    if (!go) { g_list_free_full(targets, g_free); return; }

    int ok2 = 0, fail = 0, del_count = 0;
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), "Deleting...");

    for (GList *l = targets; l; l = l->next) {
        const char *name = l->data;

        gchar *msg = g_strdup_printf("Deleting: %s", name);
        gtk_label_set_text(GTK_LABEL(pd->label), msg);
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(pd->bar));
        g_free(msg);
        while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);

#ifdef HAVE_LIBSSH2
        if (p->ssh_conn) {
            gchar *rp = g_strdup_printf("%s/%s", p->ssh_conn->remote_path, name);
            int r = libssh2_sftp_unlink(p->ssh_conn->sftp, rp);
            if (r != 0)
                r = libssh2_sftp_rmdir(p->ssh_conn->sftp, rp);
            (r == 0) ? ok2++ : fail++;
            g_free(rp);
        } else
#endif
        {
            gchar *path = g_build_filename(p->cwd, name, NULL);
            (delete_r(path, pd, &del_count) == 0) ? ok2++ : fail++;
            g_free(path);
        }
    }

    progress_free(pd);
    g_list_free_full(targets, g_free);
    fm_status(fm, "Deleted: %d  errors: %d", ok2, fail);
    panel_reload(p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Mkdir
 * ─────────────────────────────────────────────────────────────────── */

void fo_mkdir(FM *fm)
{
    Panel *p = active_panel(fm);

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "New directory");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 380, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *lbl   = gtk_label_new("New directory name:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *entry = gtk_entry_new();
    gtk_widget_set_size_request(entry, 320, -1);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn     = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    if (dlg_ctx_run(&ctx) == 1) {
        const gchar *name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (name && name[0]) {
            gchar *path = g_build_filename(p->cwd, name, NULL);
            if (mkdir(path, 0755) == 0) {
                fm_status(fm, "Directory created: %s", name);
                panel_reload(p);
            } else {
                fm_status(fm, "Error creating directory: %s", g_strerror(errno));
            }
            g_free(path);
        }
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Rename
 * ─────────────────────────────────────────────────────────────────── */

void fo_rename(FM *fm)
{
    Panel *p = active_panel(fm);
    gchar *old_name = panel_cursor_name(p);
    if (!old_name || strcmp(old_name, "..") == 0) {
        g_free(old_name);
        fm_status(fm, "No file selected");
        return;
    }

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Rename");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 420, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    gchar *markup = g_strdup_printf("Rename  <b>%s</b>  to:", old_name);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), markup);
    g_free(markup);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), old_name);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    gtk_widget_set_size_request(entry, 360, -1);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn     = gtk_button_new_with_label("Rename");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    if (dlg_ctx_run(&ctx) == 1) {
        const gchar *new_name = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (new_name && new_name[0] && strcmp(new_name, old_name) != 0) {
            gchar *src_path = g_build_filename(p->cwd, old_name, NULL);
            gchar *dst_path = g_build_filename(p->cwd, new_name, NULL);
            if (rename(src_path, dst_path) == 0) {
                fm_status(fm, "Renamed: %s → %s", old_name, new_name);
                panel_reload(p);
            } else {
                fm_status(fm, "Error renaming: %s", g_strerror(errno));
            }
            g_free(src_path); g_free(dst_path);
        }
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    g_free(old_name);
}
