#include "fm.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <dirent.h>

#define COPY_BUF (256 * 1024)

/* ── Path traversal guard ─────────────────────────────────────────── */

/* Reject filenames that could escape the target directory */
static gboolean name_is_safe(const char *name)
{
    if (!name || !name[0]) return FALSE;
    if (strcmp(name, "..") == 0) return FALSE;
    if (strchr(name, '/') != NULL) return FALSE;
    if (strstr(name, "..") != NULL) return FALSE;
    return TRUE;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Progress dialog (GTK4 – custom window + auto-pulse timer)
 *
 *  Two modes:
 *   - Indeterminate: auto-pulsing bar (for scanning / calculating)
 *   - Determinate:   fraction 0..1 with percentage text
 *
 *  progress_new()        → creates in indeterminate mode
 *  progress_set_phase()  → change the phase label (e.g. "Calculating…")
 *  progress_set_total()  → switch to determinate mode
 *  progress_tick()       → add bytes, update bar + speed display
 *  progress_set_file()   → set current filename being processed
 *  progress_pulse()      → manual pulse (for delete counters etc.)
 *  progress_free()       → destroy window + stop timer
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget *window;
    GtkWidget *phase_lbl;    /* "Calculating…" / "Copying…" */
    GtkWidget *file_lbl;     /* current filename */
    GtkWidget *bar;
    GtkWidget *detail_lbl;   /* "1.2 MB / 5.0 MB  —  2.3 MB/s" */
    guint      pulse_id;     /* auto-pulse timer source id */
    gint64     total_bytes;
    gint64     done_bytes;
    gint64     start_time;   /* g_get_monotonic_time at start */
    gboolean   determinate;
    gboolean   cancelled;    /* TRUE after user clicks Cancel */
} ProgressDlg;

static gboolean pulse_tick(gpointer data)
{
    ProgressDlg *pd = data;
    if (!pd->determinate) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(pd->bar));
        while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    }
    return G_SOURCE_CONTINUE;
}

static void on_progress_cancel(GtkButton *btn, gpointer data)
{
    (void)btn;
    ProgressDlg *pd = data;
    pd->cancelled = TRUE;
}

static ProgressDlg *progress_new(GtkWindow *parent, const char *title)
{
    ProgressDlg *pd = g_new0(ProgressDlg, 1);
    pd->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(pd->window), title);
    gtk_window_set_modal(GTK_WINDOW(pd->window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(pd->window), parent);
    gtk_window_set_resizable(GTK_WINDOW(pd->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(pd->window), 420, -1);
    gtk_window_set_deletable(GTK_WINDOW(pd->window), FALSE);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    pd->phase_lbl = gtk_label_new(title);
    gtk_widget_set_halign(pd->phase_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(pd->phase_lbl), PANGO_ELLIPSIZE_MIDDLE);

    pd->file_lbl = gtk_label_new("");
    gtk_widget_set_halign(pd->file_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(pd->file_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(pd->file_lbl, "dim-label");

    pd->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pd->bar), FALSE);
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(pd->bar), 0.03);

    pd->detail_lbl = gtk_label_new("");
    gtk_widget_set_halign(pd->detail_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(pd->detail_lbl), PANGO_ELLIPSIZE_END);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_halign(cancel_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(cancel_btn, 4);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_progress_cancel), pd);

    gtk_box_append(GTK_BOX(vbox), pd->phase_lbl);
    gtk_box_append(GTK_BOX(vbox), pd->file_lbl);
    gtk_box_append(GTK_BOX(vbox), pd->bar);
    gtk_box_append(GTK_BOX(vbox), pd->detail_lbl);
    gtk_box_append(GTK_BOX(vbox), cancel_btn);
    gtk_window_set_child(GTK_WINDOW(pd->window), vbox);

    pd->determinate = FALSE;
    pd->cancelled   = FALSE;
    pd->start_time  = g_get_monotonic_time();

    /* Auto-pulse every 80ms — keeps the bar animated during blocking work */
    pd->pulse_id = g_timeout_add(80, pulse_tick, pd);

    gtk_window_present(GTK_WINDOW(pd->window));
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    return pd;
}

static void progress_set_phase(ProgressDlg *pd, const char *phase)
{
    gtk_label_set_text(GTK_LABEL(pd->phase_lbl), phase);
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void progress_set_file(ProgressDlg *pd, const char *name)
{
    gtk_label_set_text(GTK_LABEL(pd->file_lbl), name ? name : "");
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void progress_set_total(ProgressDlg *pd, gint64 total)
{
    pd->total_bytes = total;
    pd->done_bytes  = 0;
    pd->determinate = TRUE;
    pd->start_time  = g_get_monotonic_time();
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pd->bar), 0.0);
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(pd->bar), TRUE);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(pd->bar), "0 %");
}

static void progress_tick(ProgressDlg *pd, gint64 chunk)
{
    pd->done_bytes += chunk;
    double frac = pd->total_bytes > 0
                  ? (double)pd->done_bytes / pd->total_bytes : 0.0;
    frac = CLAMP(frac, 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pd->bar), frac);

    gchar *pct = g_strdup_printf("%.0f %%", frac * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(pd->bar), pct);
    g_free(pct);

    /* Speed + transferred / total */
    gchar *s_done  = fmt_size(pd->done_bytes);
    gchar *s_total = fmt_size(pd->total_bytes);
    gint64 elapsed = g_get_monotonic_time() - pd->start_time;
    if (elapsed > 500000) {  /* show speed after 0.5s */
        double speed = (double)pd->done_bytes / (elapsed / 1e6);
        gchar *s_speed = fmt_size((goffset)speed);
        gchar *detail = g_strdup_printf("%s / %s  —  %s/s", s_done, s_total, s_speed);
        gtk_label_set_text(GTK_LABEL(pd->detail_lbl), detail);
        g_free(detail); g_free(s_speed);
    } else {
        gchar *detail = g_strdup_printf("%s / %s", s_done, s_total);
        gtk_label_set_text(GTK_LABEL(pd->detail_lbl), detail);
        g_free(detail);
    }
    g_free(s_done); g_free(s_total);

    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void progress_pulse(ProgressDlg *pd, const char *detail)
{
    if (pd->determinate) return;
    gtk_progress_bar_pulse(GTK_PROGRESS_BAR(pd->bar));
    if (detail)
        gtk_label_set_text(GTK_LABEL(pd->detail_lbl), detail);
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
}

static void progress_finish(ProgressDlg *pd)
{
    pd->determinate = TRUE;   /* stop auto-pulse from overwriting */
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(pd->bar), 1.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(pd->bar), "100 %");
    progress_set_phase(pd, "Done");
}

static void progress_free(ProgressDlg *pd)
{
    if (pd->pulse_id) g_source_remove(pd->pulse_id);
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
    progress_set_file(cp->pd, cp->current_name);
    progress_tick(cp->pd, chunk);
}

/* Shared scan counter — pumps GTK events every N files so the pulse
 * timer keeps the progress bar animated during size calculation. */
static int scan_counter;

static gint64 calc_size_r(const char *path, ProgressDlg *pd)
{
    if (pd && pd->cancelled) return 0;

    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) {
        if (pd && (++scan_counter & 127) == 0)
            while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
        return st.st_size;
    }

    gint64 total = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *de;
    char sub[4096];
    while ((de = readdir(dir)) != NULL) {
        if (pd && pd->cancelled) break;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        g_snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        total += calc_size_r(sub, pd);
    }
    closedir(dir);
    return total;
}

/* Cancel return code — distinguishable from I/O errors */
#define ERR_CANCELLED 2

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
        if (cp && cp->pd && cp->pd->cancelled) { err = ERR_CANCELLED; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = write(fout, buf + w, (size_t)(n - w));
            if (r < 0) { err = -1; break; }
            w += r;
        }
        if (err) break;
        cp_tick(cp, n);
    }
    if (n < 0 && !err) err = -1;
    g_free(buf);
    close(fin);
    close(fout);
    if (err == ERR_CANCELLED) unlink(dst);   /* clean up partial file */
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

    while ((de = readdir(dir)) != NULL && !err) {
        if (cp && cp->pd && cp->pd->cancelled) { err = ERR_CANCELLED; break; }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        g_snprintf(sp, sizeof(sp), "%s/%s", src, de->d_name);
        g_snprintf(dp, sizeof(dp), "%s/%s", dst, de->d_name);
        if (lstat(sp, &st) != 0) continue;
        err = S_ISDIR(st.st_mode) ? copy_dir_r(sp, dp, cp) : copy_file(sp, dp, cp);
    }
    closedir(dir);
    return err;
}

static int delete_r(const char *path, ProgressDlg *pd, int *del_count)
{
    if (pd && pd->cancelled) return ERR_CANCELLED;

    struct stat st;
    if (lstat(path, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) {
        int r = unlink(path);
        if (pd && del_count) {
            (*del_count)++;
            if (*del_count % 50 == 0) {
                gchar *msg = g_strdup_printf("Deleted: %d files", *del_count);
                progress_pulse(pd, msg);
                g_free(msg);
            }
        }
        return r;
    }

    DIR *dir = opendir(path);
    if (!dir) return -1;

    struct dirent *de;
    char sub[4096];
    int err = 0;
    while ((de = readdir(dir)) != NULL && !err) {
        if (pd && pd->cancelled) { err = ERR_CANCELLED; break; }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        g_snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        err = delete_r(sub, pd, del_count);
    }
    closedir(dir);
    if (err == ERR_CANCELLED) return err;
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
        if (cp && cp->pd && cp->pd->cancelled) { err = ERR_CANCELLED; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = libssh2_sftp_write(fout, buf + w, (size_t)(n - w));
            if (r < 0) { err = -1; break; }
            w += r;
        }
        if (err) break;
        cp_tick(cp, n);
    }
    if (n < 0 && !err) err = -1;
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
        if (cp && cp->pd && cp->pd->cancelled) { err = ERR_CANCELLED; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t r = write(fout, buf + w, (size_t)(n - w));
            if (r < 0) { err = -1; break; }
            w += r;
        }
        if (err) break;
        cp_tick(cp, n);
    }
    if (n < 0 && !err) err = -1;
    g_free(buf);
    libssh2_sftp_close(fin);
    close(fout);
    if (err == ERR_CANCELLED) unlink(local_path);
    return err;
}

/* Recursive SFTP size calculation for progress */
static gint64 sftp_calc_size_r(SshConn *conn, const char *remote_path,
                                ProgressDlg *pd)
{
    if (pd && pd->cancelled) return 0;

    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_stat(conn->sftp, remote_path, &a) != 0) return 0;
    if (!LIBSSH2_SFTP_S_ISDIR(a.permissions)) {
        if (pd && (++scan_counter & 63) == 0)
            while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
        return (gint64)a.filesize;
    }

    gint64 total = 0;
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(conn->sftp, remote_path);
    if (!handle) return 0;

    char name[512];
    char longentry[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (libssh2_sftp_readdir_ex(handle, name, sizeof(name),
                                    longentry, sizeof(longentry), &attrs) > 0) {
        if (pd && pd->cancelled) break;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        gchar *sub = g_strdup_printf("%s/%s", remote_path, name);
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
            LIBSSH2_SFTP_S_ISDIR(attrs.permissions))
            total += sftp_calc_size_r(conn, sub, pd);
        else if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
            total += (gint64)attrs.filesize;
        g_free(sub);
    }
    libssh2_sftp_closedir(handle);
    return total;
}

/* Recursive SFTP upload (local dir → remote dir) */
static int sftp_upload_dir_r(SshConn *conn, const char *local_path,
                              const char *remote_path, CopyProgress *cp)
{
    /* Create remote directory (ignore if exists) */
    libssh2_sftp_mkdir(conn->sftp, remote_path,
        LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP |
        LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH |
        LIBSSH2_SFTP_S_IXOTH);

    DIR *dir = opendir(local_path);
    if (!dir) return -1;

    struct dirent *de;
    struct stat st;
    int err = 0;

    while ((de = readdir(dir)) != NULL && !err) {
        if (cp && cp->pd && cp->pd->cancelled) { err = ERR_CANCELLED; break; }
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        gchar *lp = g_build_filename(local_path, de->d_name, NULL);
        gchar *rp = g_strdup_printf("%s/%s", remote_path, de->d_name);
        if (lstat(lp, &st) == 0) {
            err = S_ISDIR(st.st_mode)
                ? sftp_upload_dir_r(conn, lp, rp, cp)
                : sftp_upload(conn, lp, rp, cp);
        }
        g_free(lp); g_free(rp);
    }
    closedir(dir);
    return err;
}

/* Recursive SFTP download (remote dir → local dir) */
static int sftp_download_dir_r(SshConn *conn, const char *remote_path,
                                const char *local_path, CopyProgress *cp)
{
    if (mkdir(local_path, 0755) != 0 && errno != EEXIST) return -1;

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(conn->sftp, remote_path);
    if (!handle) return -1;

    char name[512];
    char longentry[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int err = 0;

    while (libssh2_sftp_readdir_ex(handle, name, sizeof(name),
                                    longentry, sizeof(longentry), &attrs) > 0 && !err) {
        if (cp && cp->pd && cp->pd->cancelled) { err = ERR_CANCELLED; break; }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        gchar *rp = g_strdup_printf("%s/%s", remote_path, name);
        gchar *lp = g_build_filename(local_path, name, NULL);
        if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
            LIBSSH2_SFTP_S_ISDIR(attrs.permissions))
            err = sftp_download_dir_r(conn, rp, lp, cp);
        else
            err = sftp_download(conn, rp, lp, cp);
        g_free(rp); g_free(lp);
    }
    libssh2_sftp_closedir(handle);
    return err;
}

/* Recursive SFTP delete */
static int sftp_delete_r(SshConn *conn, const char *remote_path,
                          ProgressDlg *pd, int *del_count)
{
    if (pd && pd->cancelled) return ERR_CANCELLED;

    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_stat(conn->sftp, remote_path, &a) != 0) return -1;

    if (!LIBSSH2_SFTP_S_ISDIR(a.permissions)) {
        int r = libssh2_sftp_unlink(conn->sftp, remote_path);
        if (pd && del_count) {
            (*del_count)++;
            if (*del_count % 50 == 0) {
                gchar *msg = g_strdup_printf("Deleted: %d files", *del_count);
                progress_pulse(pd, msg);
                g_free(msg);
            }
        }
        return r;
    }

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(conn->sftp, remote_path);
    if (!handle) return -1;

    char name[512];
    char longentry[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int err = 0;

    while (libssh2_sftp_readdir_ex(handle, name, sizeof(name),
                                    longentry, sizeof(longentry), &attrs) > 0 && !err) {
        if (pd && pd->cancelled) { err = ERR_CANCELLED; break; }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
        gchar *sub = g_strdup_printf("%s/%s", remote_path, name);
        err = sftp_delete_r(conn, sub, pd, del_count);
        g_free(sub);
    }
    libssh2_sftp_closedir(handle);
    if (err == ERR_CANCELLED) return err;
    return libssh2_sftp_rmdir(conn->sftp, remote_path);
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
    gtk_file_dialog_set_title(fd, "Select destination directory");

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

    GtkWidget *lbl = gtk_label_new("Destination directory:");
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

    GList *targets = panel_selection(src);
    if (!targets) { fm_status(fm, "Nothing is selected"); return; }

    const char *default_dst =
#ifdef HAVE_LIBSSH2
        (dst->ssh_conn ? dst->ssh_conn->remote_path : dst->cwd);
#else
        dst->cwd;
#endif
    gchar *dest = ask_destination(fm, "Copy to", default_dst);
    if (!dest) { g_list_free_full(targets, g_free); return; }

    int ok = 0, fail = 0;
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), "Copying");
    progress_set_phase(pd, "Calculating size…");

    /* Pre-calculate total bytes for progress */
    gint64 total_bytes = 0;
    scan_counter = 0;
#ifdef HAVE_LIBSSH2
    if (src->ssh_conn) {
        for (GList *l = targets; l && !pd->cancelled; l = l->next) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path,
                                        (const char *)l->data);
            progress_set_file(pd, (const char *)l->data);
            total_bytes += sftp_calc_size_r(src->ssh_conn, rp, pd);
            g_free(rp);
        }
        /* Remote→Remote: bytes are transferred twice (download + upload) */
        if (dst->ssh_conn) total_bytes *= 2;
    } else if (dst->ssh_conn) {
        for (GList *l = targets; l && !pd->cancelled; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            progress_set_file(pd, (const char *)l->data);
            total_bytes += calc_size_r(sp, pd);
            g_free(sp);
        }
    } else
#endif
    {
        for (GList *l = targets; l && !pd->cancelled; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            progress_set_file(pd, (const char *)l->data);
            total_bytes += calc_size_r(sp, pd);
            g_free(sp);
        }
    }

    if (pd->cancelled) goto copy_done;

    progress_set_phase(pd, "Copying…");
    progress_set_total(pd, total_bytes);

    CopyProgress cp = { .pd = pd, .total_bytes = total_bytes, .done_bytes = 0 };

    for (GList *l = targets; l && !pd->cancelled; l = l->next) {
        const char *name = l->data;
        if (!name_is_safe(name)) { fail++; continue; }
        cp.current_name = name;
        progress_set_file(pd, name);

        int r = -1;

#ifdef HAVE_LIBSSH2
        if (src->ssh_conn && dst->ssh_conn) {
            /* Remote→Remote: download to temp, then upload */
            gchar *tmpdir = g_dir_make_tmp("fm-r2r-XXXXXX", NULL);
            if (!tmpdir) { fail++; continue; }
            gchar *rp_src = g_strdup_printf("%s/%s", src->ssh_conn->remote_path, name);
            gchar *lp_tmp = g_build_filename(tmpdir, name, NULL);
            gchar *rp_dst = g_strdup_printf("%s/%s", dest, name);
            LIBSSH2_SFTP_ATTRIBUTES ra;
            if (libssh2_sftp_stat(src->ssh_conn->sftp, rp_src, &ra) == 0 &&
                LIBSSH2_SFTP_S_ISDIR(ra.permissions))
                r = sftp_download_dir_r(src->ssh_conn, rp_src, lp_tmp, &cp);
            else
                r = sftp_download(src->ssh_conn, rp_src, lp_tmp, &cp);
            if (r == 0) {
                struct stat tst;
                if (lstat(lp_tmp, &tst) == 0 && S_ISDIR(tst.st_mode))
                    r = sftp_upload_dir_r(dst->ssh_conn, lp_tmp, rp_dst, &cp);
                else
                    r = sftp_upload(dst->ssh_conn, lp_tmp, rp_dst, &cp);
            }
            /* clean up temp */
            delete_r(lp_tmp, NULL, NULL);
            rmdir(tmpdir);
            g_free(rp_src); g_free(lp_tmp); g_free(rp_dst); g_free(tmpdir);
        } else if (src->ssh_conn) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path, name);
            gchar *lp = g_build_filename(dest, name, NULL);
            LIBSSH2_SFTP_ATTRIBUTES ra;
            if (libssh2_sftp_stat(src->ssh_conn->sftp, rp, &ra) == 0 &&
                LIBSSH2_SFTP_S_ISDIR(ra.permissions))
                r = sftp_download_dir_r(src->ssh_conn, rp, lp, &cp);
            else
                r = sftp_download(src->ssh_conn, rp, lp, &cp);
            g_free(rp); g_free(lp);
        } else if (dst->ssh_conn) {
            gchar *lp = g_build_filename(src->cwd, name, NULL);
            gchar *rp = g_strdup_printf("%s/%s", dest, name);
            struct stat ust;
            if (lstat(lp, &ust) == 0 && S_ISDIR(ust.st_mode))
                r = sftp_upload_dir_r(dst->ssh_conn, lp, rp, &cp);
            else
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

        if (r == 0) ok++; else if (r != ERR_CANCELLED) fail++;
    }

copy_done:
    progress_finish(pd);
    {   gboolean was_cancelled = pd->cancelled;
        progress_free(pd);
        g_list_free_full(targets, g_free);
        g_free(dest);

        fm_status(fm, was_cancelled ? "Copy cancelled (%d copied, %d errors)"
                                    : "Copied: %d  errors: %d", ok, fail);
    }
    if (src->marks) g_hash_table_remove_all(src->marks);

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

    /* Restore focus to active panel */
    src->inhibit_sel = TRUE;
    gtk_widget_grab_focus(src->column_view);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Move
 * ─────────────────────────────────────────────────────────────────── */

void fo_move(FM *fm)
{
    Panel *src = active_panel(fm);
    Panel *dst = other_panel(fm);

    GList *targets = panel_selection(src);
    if (!targets) { fm_status(fm, "Nothing is selected"); return; }

    const char *default_dst =
#ifdef HAVE_LIBSSH2
        (dst->ssh_conn ? dst->ssh_conn->remote_path : dst->cwd);
#else
        dst->cwd;
#endif
    gchar *dest = ask_destination(fm, "Move to", default_dst);
    if (!dest) { g_list_free_full(targets, g_free); return; }

    int ok = 0, fail = 0;
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), "Moving");
    progress_set_phase(pd, "Calculating size…");

    /* Pre-calculate total bytes for progress */
    gint64 total_bytes = 0;
    scan_counter = 0;
#ifdef HAVE_LIBSSH2
    if (src->ssh_conn) {
        for (GList *l = targets; l && !pd->cancelled; l = l->next) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path,
                                        (const char *)l->data);
            progress_set_file(pd, (const char *)l->data);
            total_bytes += sftp_calc_size_r(src->ssh_conn, rp, pd);
            g_free(rp);
        }
        if (dst->ssh_conn) total_bytes *= 2;
    } else if (dst->ssh_conn) {
        for (GList *l = targets; l && !pd->cancelled; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            progress_set_file(pd, (const char *)l->data);
            total_bytes += calc_size_r(sp, pd);
            g_free(sp);
        }
    } else
#endif
    {
        for (GList *l = targets; l && !pd->cancelled; l = l->next) {
            gchar *sp = g_build_filename(src->cwd, (const char *)l->data, NULL);
            progress_set_file(pd, (const char *)l->data);
            total_bytes += calc_size_r(sp, pd);
            g_free(sp);
        }
    }

    if (pd->cancelled) goto move_done;

    progress_set_phase(pd, "Moving…");
    progress_set_total(pd, total_bytes);

    CopyProgress cp = { .pd = pd, .total_bytes = total_bytes, .done_bytes = 0 };

    for (GList *l = targets; l && !pd->cancelled; l = l->next) {
        const char *name = l->data;
        if (!name_is_safe(name)) { fail++; continue; }
        cp.current_name = name;
        progress_set_file(pd, name);

        int r = -1;

#ifdef HAVE_LIBSSH2
        if (src->ssh_conn && dst->ssh_conn) {
            /* Remote→Remote: download to temp, upload, delete source */
            gchar *tmpdir = g_dir_make_tmp("fm-r2r-XXXXXX", NULL);
            if (!tmpdir) { fail++; continue; }
            gchar *rp_src = g_strdup_printf("%s/%s", src->ssh_conn->remote_path, name);
            gchar *lp_tmp = g_build_filename(tmpdir, name, NULL);
            gchar *rp_dst = g_strdup_printf("%s/%s", dest, name);
            LIBSSH2_SFTP_ATTRIBUTES ra;
            if (libssh2_sftp_stat(src->ssh_conn->sftp, rp_src, &ra) == 0 &&
                LIBSSH2_SFTP_S_ISDIR(ra.permissions))
                r = sftp_download_dir_r(src->ssh_conn, rp_src, lp_tmp, &cp);
            else
                r = sftp_download(src->ssh_conn, rp_src, lp_tmp, &cp);
            if (r == 0) {
                struct stat tst;
                if (lstat(lp_tmp, &tst) == 0 && S_ISDIR(tst.st_mode))
                    r = sftp_upload_dir_r(dst->ssh_conn, lp_tmp, rp_dst, &cp);
                else
                    r = sftp_upload(dst->ssh_conn, lp_tmp, rp_dst, &cp);
            }
            if (r == 0)
                sftp_delete_r(src->ssh_conn, rp_src, NULL, NULL);
            delete_r(lp_tmp, NULL, NULL);
            rmdir(tmpdir);
            g_free(rp_src); g_free(lp_tmp); g_free(rp_dst); g_free(tmpdir);
        } else if (src->ssh_conn) {
            gchar *rp = g_strdup_printf("%s/%s", src->ssh_conn->remote_path, name);
            gchar *lp = g_build_filename(dest, name, NULL);
            LIBSSH2_SFTP_ATTRIBUTES ra;
            if (libssh2_sftp_stat(src->ssh_conn->sftp, rp, &ra) == 0 &&
                LIBSSH2_SFTP_S_ISDIR(ra.permissions))
                r = sftp_download_dir_r(src->ssh_conn, rp, lp, &cp);
            else
                r = sftp_download(src->ssh_conn, rp, lp, &cp);
            if (r == 0)
                sftp_delete_r(src->ssh_conn, rp, NULL, NULL);
            g_free(rp); g_free(lp);
        } else if (dst->ssh_conn) {
            gchar *lp = g_build_filename(src->cwd, name, NULL);
            gchar *rp = g_strdup_printf("%s/%s", dest, name);
            struct stat ust;
            if (lstat(lp, &ust) == 0 && S_ISDIR(ust.st_mode))
                r = sftp_upload_dir_r(dst->ssh_conn, lp, rp, &cp);
            else
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

        if (r == 0) ok++; else if (r != ERR_CANCELLED) fail++;
    }

move_done:
    progress_finish(pd);
    {   gboolean was_cancelled = pd->cancelled;
        progress_free(pd);
        g_list_free_full(targets, g_free);
        g_free(dest);

        fm_status(fm, was_cancelled ? "Move cancelled (%d moved, %d errors)"
                                    : "Moved: %d  errors: %d", ok, fail);
    }
    if (src->marks) g_hash_table_remove_all(src->marks);

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

    /* Restore focus to active panel */
    src->inhibit_sel = TRUE;
    gtk_widget_grab_focus(src->column_view);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Delete
 * ─────────────────────────────────────────────────────────────────── */

void fo_delete(FM *fm)
{
    Panel *p = active_panel(fm);
    GList *targets = panel_selection(p);
    if (!targets) { fm_status(fm, "Nothing is selected"); return; }

    int n = g_list_length(targets);
    gchar *question = (n == 1)
        ? g_strdup_printf("Delete '%s'?", (char *)targets->data)
        : g_strdup_printf("Delete %d items?", n);

    /* Confirmation dialog */
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Delete confirmation");
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

    GtkWidget *warn = gtk_label_new("This action is irreversible.");
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
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), "Deleting");
    progress_set_phase(pd, "Deleting…");

    for (GList *l = targets; l && !pd->cancelled; l = l->next) {
        const char *name = l->data;
        if (!name_is_safe(name)) { fail++; continue; }

        progress_set_file(pd, name);

#ifdef HAVE_LIBSSH2
        if (p->ssh_conn) {
            gchar *rp = g_strdup_printf("%s/%s", p->ssh_conn->remote_path, name);
            (sftp_delete_r(p->ssh_conn, rp, pd, &del_count) == 0) ? ok2++ : fail++;
            g_free(rp);
        } else
#endif
        {
            gchar *path = g_build_filename(p->cwd, name, NULL);
            (delete_r(path, pd, &del_count) == 0) ? ok2++ : fail++;
            g_free(path);
        }
    }

    progress_finish(pd);
    {   gboolean was_cancelled = pd->cancelled;
        progress_free(pd);
        g_list_free_full(targets, g_free);

        fm_status(fm, was_cancelled ? "Delete cancelled (%d deleted, %d errors)"
                                    : "Deleted: %d  errors: %d", ok2, fail);
    }
    if (p->marks) g_hash_table_remove_all(p->marks);
    panel_reload(p);

    /* Restore focus to active panel */
    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
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
        if (name && name[0] && name_is_safe(name)) {
#ifdef HAVE_LIBSSH2
            if (p->ssh_conn) {
                gchar *rp = g_strdup_printf("%s/%s", p->ssh_conn->remote_path, name);
                int r = libssh2_sftp_mkdir(p->ssh_conn->sftp, rp,
                    LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP |
                    LIBSSH2_SFTP_S_IXGRP | LIBSSH2_SFTP_S_IROTH |
                    LIBSSH2_SFTP_S_IXOTH);
                if (r == 0) {
                    fm_status(fm, "Directory created: %s", name);
                    panel_load_remote(p, p->ssh_conn->remote_path);
                } else {
                    fm_status(fm, "Error creating remote directory");
                }
                g_free(rp);
            } else
#endif
            {
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
    }
    gtk_window_destroy(GTK_WINDOW(dlg));

    /* Restore focus to active panel */
    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
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
        fm_status(fm, "No file is selected");
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

    gchar *markup = g_strdup_printf("Rename <b>%s</b> to:", old_name);
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
        if (new_name && new_name[0] && strcmp(new_name, old_name) != 0
            && name_is_safe(new_name)) {
#ifdef HAVE_LIBSSH2
            if (p->ssh_conn) {
                gchar *old_rp = g_strdup_printf("%s/%s", p->ssh_conn->remote_path, old_name);
                gchar *new_rp = g_strdup_printf("%s/%s", p->ssh_conn->remote_path, new_name);
                int r = libssh2_sftp_rename(p->ssh_conn->sftp, old_rp, new_rp);
                if (r == 0) {
                    fm_status(fm, "Renamed: %s → %s", old_name, new_name);
                    panel_load_remote(p, p->ssh_conn->remote_path);
                } else {
                    fm_status(fm, "Error renaming remote file");
                }
                g_free(old_rp); g_free(new_rp);
            } else
#endif
            {
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
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    g_free(old_name);

    /* Restore focus to active panel */
    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Archive helpers
 * ─────────────────────────────────────────────────────────────────── */

typedef enum {
    ARC_UNKNOWN,
    ARC_TAR_GZ,
    ARC_TAR_BZ2,
    ARC_TAR_XZ,
    ARC_TAR_ZST,
    ARC_TAR,
    ARC_ZIP,
    ARC_7Z,
    ARC_RAR,
} ArcType;

static ArcType arc_detect(const char *name)
{
    if (!name) return ARC_UNKNOWN;
    const char *d = strrchr(name, '.');
    if (!d) return ARC_UNKNOWN;

    /* double extensions first */
    if (g_str_has_suffix(name, ".tar.gz")  || g_str_has_suffix(name, ".tar.GZ"))  return ARC_TAR_GZ;
    if (g_str_has_suffix(name, ".tar.bz2") || g_str_has_suffix(name, ".tar.BZ2")) return ARC_TAR_BZ2;
    if (g_str_has_suffix(name, ".tar.xz")  || g_str_has_suffix(name, ".tar.XZ"))  return ARC_TAR_XZ;
    if (g_str_has_suffix(name, ".tar.zst") || g_str_has_suffix(name, ".tar.ZST")) return ARC_TAR_ZST;

    const char *ext = d + 1;
    if (!g_ascii_strcasecmp(ext, "tgz"))   return ARC_TAR_GZ;
    if (!g_ascii_strcasecmp(ext, "tbz2"))  return ARC_TAR_BZ2;
    if (!g_ascii_strcasecmp(ext, "txz"))   return ARC_TAR_XZ;
    if (!g_ascii_strcasecmp(ext, "tar"))   return ARC_TAR;
    if (!g_ascii_strcasecmp(ext, "zip"))   return ARC_ZIP;
    if (!g_ascii_strcasecmp(ext, "7z"))    return ARC_7Z;
    if (!g_ascii_strcasecmp(ext, "rar"))   return ARC_RAR;
    if (!g_ascii_strcasecmp(ext, "gz"))    return ARC_TAR_GZ;  /* might be plain gz */
    if (!g_ascii_strcasecmp(ext, "bz2"))   return ARC_TAR_BZ2;
    if (!g_ascii_strcasecmp(ext, "xz"))    return ARC_TAR_XZ;
    if (!g_ascii_strcasecmp(ext, "zst"))   return ARC_TAR_ZST;
    return ARC_UNKNOWN;
}

static gboolean arc_is_archive(const char *name)
{
    return arc_detect(name) != ARC_UNKNOWN;
}

/* Run a command with a progress dialog, return TRUE on success */
static gboolean run_archive_cmd(FM *fm, const char *title,
                                 const char *phase, const gchar **argv)
{
    ProgressDlg *pd = progress_new(GTK_WINDOW(fm->window), title);
    progress_set_phase(pd, phase);

    /* Show the command target */
    progress_set_file(pd, argv[0]);

    GPid pid;
    GError *err = NULL;
    gboolean spawned = g_spawn_async(NULL, (gchar **)argv, NULL,
        G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
        NULL, NULL, &pid, &err);

    if (!spawned) {
        fm_status(fm, "%s error: %s", title, err ? err->message : "?");
        g_clear_error(&err);
        progress_free(pd);
        return FALSE;
    }

    /* Poll child process while keeping UI alive */
    gboolean done = FALSE;
    int status = 0;
    while (!done && !pd->cancelled) {
        int r = waitpid(pid, &status, WNOHANG);
        if (r > 0) {
            done = TRUE;
        } else {
            /* Pump events — keeps pulse animation + cancel button working */
            while (g_main_context_pending(NULL))
                g_main_context_iteration(NULL, FALSE);
            g_usleep(50000); /* 50ms */
        }
    }

    if (pd->cancelled && !done) {
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        progress_free(pd);
        fm_status(fm, "%s cancelled", title);
        return FALSE;
    }

    gboolean ok = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    progress_finish(pd);
    progress_free(pd);
    return ok;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Extract archive
 * ─────────────────────────────────────────────────────────────────── */

void fo_extract(FM *fm)
{
    Panel *p = active_panel(fm);
    if (p->ssh_conn) {
        fm_status(fm, "Extract is not supported on SFTP panels");
        return;
    }

    gchar *name = panel_cursor_name(p);
    if (!name || strcmp(name, "..") == 0 || !arc_is_archive(name)) {
        g_free(name);
        fm_status(fm, "No archive selected");
        return;
    }

    gchar *archive = g_build_filename(p->cwd, name, NULL);
    ArcType type = arc_detect(name);

    /* Ask destination */
    Panel *dst = other_panel(fm);
    gchar *dest = ask_destination(fm, "Extract to", dst->cwd);
    if (!dest) { g_free(name); g_free(archive); return; }

    const gchar *argv[10];
    int a = 0;
    gboolean ok = FALSE;

    switch (type) {
    case ARC_TAR_GZ:
    case ARC_TAR_BZ2:
    case ARC_TAR_XZ:
    case ARC_TAR_ZST:
    case ARC_TAR:
        argv[a++] = "tar";
        argv[a++] = "xf";
        argv[a++] = archive;
        argv[a++] = "-C";
        argv[a++] = dest;
        argv[a]   = NULL;
        ok = run_archive_cmd(fm, "Extract", "Extracting…", argv);
        break;
    case ARC_ZIP:
        argv[a++] = "unzip";
        argv[a++] = "-o";
        argv[a++] = archive;
        argv[a++] = "-d";
        argv[a++] = dest;
        argv[a]   = NULL;
        ok = run_archive_cmd(fm, "Extract", "Extracting…", argv);
        break;
    case ARC_7Z:
        argv[a++] = "7z";
        argv[a++] = "x";
        argv[a++] = archive;
        argv[a++] = "-y";
        {
            gchar *outdir = g_strdup_printf("-o%s", dest);
            argv[a++] = outdir;
            argv[a]   = NULL;
            ok = run_archive_cmd(fm, "Extract", "Extracting…", argv);
            g_free(outdir);
        }
        break;
    case ARC_RAR:
        argv[a++] = "unrar";
        argv[a++] = "x";
        argv[a++] = "-y";
        argv[a++] = archive;
        argv[a++] = dest;
        argv[a]   = NULL;
        ok = run_archive_cmd(fm, "Extract", "Extracting…", argv);
        break;
    default:
        fm_status(fm, "Unknown archive format");
        break;
    }

    if (ok) fm_status(fm, "Extracted: %s", name);
    else if (type != ARC_UNKNOWN) fm_status(fm, "Extract failed: %s", name);

    g_free(name);
    g_free(archive);
    g_free(dest);
    panel_reload(&fm->panels[0]);
    panel_reload(&fm->panels[1]);

    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Pack (create archive)
 * ─────────────────────────────────────────────────────────────────── */

void fo_pack(FM *fm)
{
    Panel *p = active_panel(fm);
    if (p->ssh_conn) {
        fm_status(fm, "Pack is not supported on SFTP panels");
        return;
    }

    GList *targets = panel_selection(p);
    if (!targets) { fm_status(fm, "Nothing is selected"); return; }

    /* Dialog: archive name + format */
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Create archive");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    /* Format dropdown */
    GtkWidget *lbl_fmt = gtk_label_new("Format:");
    gtk_widget_set_halign(lbl_fmt, GTK_ALIGN_END);

    static const char *fmt_labels[] = {
        "tar.gz", "tar.bz2", "tar.xz", "tar.zst", "zip", "7z", NULL
    };
    GtkStringList *fmt_list = gtk_string_list_new(fmt_labels);
    GtkWidget *fmt_dd = gtk_drop_down_new(G_LIST_MODEL(fmt_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(fmt_dd), 0);  /* default: tar.gz */
    gtk_widget_set_hexpand(fmt_dd, TRUE);

    /* Archive name */
    GtkWidget *lbl_name = gtk_label_new("Archive name:");
    gtk_widget_set_halign(lbl_name, GTK_ALIGN_END);

    GtkWidget *name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(name_entry, TRUE);

    /* Default name from first selected item */
    const char *first = targets->data;
    gchar *default_name = g_strdup_printf("%s.tar.gz", first);
    gtk_editable_set_text(GTK_EDITABLE(name_entry), default_name);
    g_free(default_name);

    /* Destination */
    GtkWidget *lbl_dst = gtk_label_new("Save in:");
    gtk_widget_set_halign(lbl_dst, GTK_ALIGN_END);

    GtkWidget *dst_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(dst_entry), other_panel(fm)->cwd);
    gtk_widget_set_hexpand(dst_entry, TRUE);

    GtkWidget *browse = gtk_button_new_with_label("...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), dst_entry);

    GtkWidget *dst_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(dst_box), dst_entry);
    gtk_box_append(GTK_BOX(dst_box), browse);

    /* Layout */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    int row = 0;
    gtk_grid_attach(GTK_GRID(grid), lbl_fmt,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), fmt_dd,     1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_name,   0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name_entry, 1, row++, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl_dst,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dst_box,    1, row++, 1, 1);

    /* Update name extension when format changes */
    /* (done manually after dialog, simpler) */

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), grid);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(name_entry, "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    if (dlg_ctx_run(&ctx) != 1) {
        gtk_window_destroy(GTK_WINDOW(dlg));
        g_list_free_full(targets, g_free);
        return;
    }

    const gchar *arc_name = gtk_editable_get_text(GTK_EDITABLE(name_entry));
    const gchar *dst_dir  = gtk_editable_get_text(GTK_EDITABLE(dst_entry));
    guint fmt_sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(fmt_dd));

    if (!arc_name || !arc_name[0] || !dst_dir || !dst_dir[0]) {
        gtk_window_destroy(GTK_WINDOW(dlg));
        g_list_free_full(targets, g_free);
        return;
    }

    gchar *arc_path = g_build_filename(dst_dir, arc_name, NULL);
    gchar *s_arc_name = g_strdup(arc_name);

    gtk_window_destroy(GTK_WINDOW(dlg));

    /* Build command */
    gboolean ok = FALSE;

    if (fmt_sel <= 3 || fmt_sel == ARC_TAR) {
        /* tar-based: tar cf archive.tar.gz -C srcdir file1 file2 ... */
        static const char *tar_flags[] = {
            "czf",   /* 0: tar.gz  */
            "cjf",   /* 1: tar.bz2 */
            "cJf",   /* 2: tar.xz  */
            NULL,    /* 3: tar.zst (needs --zstd) */
        };

        /* argv: tar + flag + archive + -C + srcdir + files... + NULL */
        int n_targets = g_list_length(targets);
        const gchar **argv = g_new0(const gchar *, n_targets + 6);
        int a = 0;
        argv[a++] = "tar";
        if (fmt_sel == 3) {
            argv[a++] = "--zstd";
            argv[a++] = "-cf";
        } else {
            argv[a++] = tar_flags[fmt_sel];
        }
        argv[a++] = arc_path;
        argv[a++] = "-C";
        argv[a++] = p->cwd;
        for (GList *l = targets; l; l = l->next)
            argv[a++] = (const char *)l->data;
        argv[a] = NULL;

        ok = run_archive_cmd(fm, "Pack", "Packing…", argv);
        g_free(argv);

    } else if (fmt_sel == 4) {
        /* zip -r archive.zip file1 file2 ... (run from srcdir) */
        int n_targets = g_list_length(targets);
        const gchar **argv = g_new0(const gchar *, n_targets + 4);
        int a = 0;
        argv[a++] = "zip";
        argv[a++] = "-r";
        argv[a++] = arc_path;
        for (GList *l = targets; l; l = l->next)
            argv[a++] = (const char *)l->data;
        argv[a] = NULL;

        /* zip needs to run from source directory */
        gchar *saved_cwd = g_get_current_dir();
        if (chdir(p->cwd) == 0) {
            ok = run_archive_cmd(fm, "Pack", "Packing…", argv);
            if (saved_cwd) chdir(saved_cwd);
        }
        g_free(saved_cwd);
        g_free(argv);

    } else if (fmt_sel == 5) {
        /* 7z a archive.7z file1 file2 ... */
        int n_targets = g_list_length(targets);
        const gchar **argv = g_new0(const gchar *, n_targets + 4);
        int a = 0;
        argv[a++] = "7z";
        argv[a++] = "a";
        argv[a++] = arc_path;
        for (GList *l = targets; l; l = l->next) {
            gchar *full = g_build_filename(p->cwd, (const char *)l->data, NULL);
            argv[a++] = full;
        }
        argv[a] = NULL;

        ok = run_archive_cmd(fm, "Pack", "Packing…", argv);

        /* Free full paths (stored starting at argv[3]) */
        for (int i = 3; argv[i]; i++)
            g_free((gchar *)argv[i]);
        g_free(argv);
    }

    if (ok) fm_status(fm, "Created: %s", s_arc_name);
    else    fm_status(fm, "Pack failed: %s", s_arc_name);

    g_free(arc_path);
    g_free(s_arc_name);
    g_list_free_full(targets, g_free);
    if (p->marks) g_hash_table_remove_all(p->marks);
    panel_reload(&fm->panels[0]);
    panel_reload(&fm->panels[1]);

    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
}
