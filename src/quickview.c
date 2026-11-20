/* Quick view (Ctrl+Q, like Total Commander): the other panel shows the
 * file under the cursor of the active panel – text, image or a short
 * summary for directories and binary files.  Local files only. */
#include "fm.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#define QV_TEXT_MAX   (512 * 1024)  /* read at most this much text      */
#define QV_BINARY_CHK 8192          /* NUL in the first bytes → binary  */
#define QV_DIR_MAX    100000        /* stop counting directory entries  */
#define QV_POLL_MS    150

static GtkWidget *qv_title(Panel *p)  { return g_object_get_data(G_OBJECT(p->qv_box), "title"); }
static GtkWidget *qv_text(Panel *p)   { return g_object_get_data(G_OBJECT(p->qv_box), "text");  }
static GtkWidget *qv_tscroll(Panel *p){ return g_object_get_data(G_OBJECT(p->qv_box), "tscroll"); }
static GtkWidget *qv_pic(Panel *p)    { return g_object_get_data(G_OBJECT(p->qv_box), "pic");   }

gboolean qv_active(FM *fm) { return fm->qv_on; }

static void qv_build(Panel *p)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_vexpand(box, TRUE);
    gtk_widget_add_css_class(box, "fm-quickview");

    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(title, 6);
    gtk_widget_set_margin_end(title, 6);
    gtk_widget_set_margin_top(title, 2);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 6);
    gtk_widget_set_focusable(tv, FALSE);
    gtk_widget_add_css_class(tv, "fm-viewer");
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
    gtk_widget_set_vexpand(sc, TRUE);

    GtkWidget *pic = gtk_picture_new();
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_SCALE_DOWN);
    gtk_widget_set_vexpand(pic, TRUE);
    gtk_widget_set_visible(pic, FALSE);

    gtk_box_append(GTK_BOX(box), title);
    gtk_box_append(GTK_BOX(box), sc);
    gtk_box_append(GTK_BOX(box), pic);
    g_object_set_data(G_OBJECT(box), "title", title);
    g_object_set_data(G_OBJECT(box), "text", tv);
    g_object_set_data(G_OBJECT(box), "tscroll", sc);
    g_object_set_data(G_OBJECT(box), "pic", pic);
    gtk_box_insert_child_after(GTK_BOX(p->frame), box, p->list_scroll);
    p->qv_box = box;
}

static void show_text(Panel *q, const char *title, const char *text)
{
    gtk_label_set_text(GTK_LABEL(qv_title(q)), title);
    gtk_widget_set_visible(qv_pic(q), FALSE);
    gtk_picture_set_paintable(GTK_PICTURE(qv_pic(q)), NULL);
    gtk_widget_set_visible(qv_tscroll(q), TRUE);
    GtkTextBuffer *b = gtk_text_view_get_buffer(GTK_TEXT_VIEW(qv_text(q)));
    gtk_text_buffer_set_text(b, text ? text : "", -1);
    GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(qv_tscroll(q)));
    gtk_adjustment_set_value(adj, 0);
}

static gchar *dir_summary(const char *path)
{
    /* d_type avoids one stat() per entry on the main thread */
    DIR *d = opendir(path);
    if (!d) return g_strdup_printf("Directory\n\n(cannot be read: %s)", g_strerror(errno));
    guint dirs = 0, files = 0;
    struct dirent *de;
    while ((de = readdir(d)) && dirs + files < QV_DIR_MAX) {
        const char *n = de->d_name;
        if (n[0] == '.' && (!n[1] || (n[1] == '.' && !n[2]))) continue;
        gboolean isdir;
        if (de->d_type == DT_DIR) isdir = TRUE;
        else if (de->d_type != DT_UNKNOWN && de->d_type != DT_LNK) isdir = FALSE;
        else {
            struct stat st;
            isdir = fstatat(dirfd(d), n, &st, 0) == 0 && S_ISDIR(st.st_mode);
        }
        if (isdir) dirs++; else files++;
    }
    closedir(d);
    return g_strdup_printf("Directory\n\n%u subdirectories\n%u files%s",
                           dirs, files, dirs + files >= QV_DIR_MAX ? " (or more)" : "");
}

/* ── images decode in a worker ──────────────────────────────────────
 * A big photo took hundreds of ms on the main thread at every cursor
 * move.  One decode runs at a time; a request made meanwhile replaces
 * the queued one, so holding an arrow key decodes only where it stops.
 * Results for a file the cursor has already left are dropped. */
#define QV_IMG_MAX_PX 2048          /* downscale bigger images on load */

typedef struct { FM *fm; gchar *path; } QvImg;

static gboolean qv_img_busy;
static QvImg   *qv_img_next;

static void qv_img_free(QvImg *r)
{
    if (!r) return;
    g_free(r->path);
    g_free(r);
}

static void qv_img_thread(GTask *t, gpointer src, gpointer data, GCancellable *c)
{
    (void)src; (void)c;
    QvImg *r = data;
    GError *e = NULL;
    int w = 0, h = 0;
    GdkPixbuf *pb;
    if (gdk_pixbuf_get_file_info(r->path, &w, &h) &&
        (w > QV_IMG_MAX_PX || h > QV_IMG_MAX_PX))
        pb = gdk_pixbuf_new_from_file_at_scale(r->path, QV_IMG_MAX_PX,
                                               QV_IMG_MAX_PX, TRUE, &e);
    else
        pb = gdk_pixbuf_new_from_file(r->path, &e);
    if (!pb) { g_task_return_error(t, e); return; }
    GdkPixbuf *o = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    g_task_return_pointer(t, o, g_object_unref);
}

static void qv_img_start(QvImg *r);

static void qv_img_done(GObject *src, GAsyncResult *res, gpointer data)
{
    (void)src;
    QvImg *r = data;
    FM *fm = r->fm;
    GError *e = NULL;
    GdkPixbuf *pb = g_task_propagate_pointer(G_TASK(res), &e);
    qv_img_busy = FALSE;
    /* still showing this file? */
    if (fm->qv_on && g_strcmp0(fm->qv_path, r->path) == 0) {
        Panel *q = &fm->panels[fm->qv_panel];
        if (pb) {
            GdkTexture *tx = gdk_texture_new_for_pixbuf(pb);
            gtk_picture_set_paintable(GTK_PICTURE(qv_pic(q)), GDK_PAINTABLE(tx));
            g_object_unref(tx);
        } else {
            gchar *base = g_path_get_basename(r->path);
            gchar *msg = g_strdup_printf("Cannot show the image: %s",
                                         e ? e->message : "unknown error");
            show_text(q, base, msg);
            g_free(msg); g_free(base);
        }
    }
    if (pb) g_object_unref(pb);
    g_clear_error(&e);
    qv_img_free(r);
    if (qv_img_next) {
        QvImg *n = qv_img_next;
        qv_img_next = NULL;
        if (fm->qv_on && g_strcmp0(fm->qv_path, n->path) == 0) qv_img_start(n);
        else qv_img_free(n);
    }
}

static void qv_img_start(QvImg *r)
{
    qv_img_busy = TRUE;
    GTask *t = g_task_new(NULL, NULL, qv_img_done, r);
    g_task_set_task_data(t, r, NULL);
    g_task_run_in_thread(t, qv_img_thread);
    g_object_unref(t);
}

static void qv_img_request(FM *fm, const char *path)
{
    QvImg *r = g_new0(QvImg, 1);
    r->fm = fm;
    r->path = g_strdup(path);
    if (qv_img_busy) {
        qv_img_free(qv_img_next);
        qv_img_next = r;
    } else {
        qv_img_start(r);
    }
}

static void qv_render(FM *fm, Panel *q, const char *path)
{
    gchar *base = g_path_get_basename(path);
    struct stat st;
    if (stat(path, &st) != 0) {
        show_text(q, base, g_strerror(errno));
        g_free(base);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        gchar *s = dir_summary(path);
        show_text(q, base, s);
        g_free(s);
        g_free(base);
        return;
    }
    gchar *size = fmt_size(st.st_size);
    gchar *title = g_strdup_printf("%s  —  %s", base, size);

    /* decoded in a worker (and downscaled): see qv_img_request */
    if (fm->viewer_image_preview && viewer_is_image(base) &&
        S_ISREG(st.st_mode) && st.st_size <= 64 * 1024 * 1024) {
        gtk_label_set_text(GTK_LABEL(qv_title(q)), title);
        gtk_widget_set_visible(qv_tscroll(q), FALSE);
        gtk_picture_set_paintable(GTK_PICTURE(qv_pic(q)), NULL);
        gtk_widget_set_visible(qv_pic(q), TRUE);
        qv_img_request(fm, path);
        goto out;
    }
    if (!S_ISREG(st.st_mode)) {
        show_text(q, title, "Special file (device, socket or pipe)");
        goto out;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) { show_text(q, title, g_strerror(errno)); goto out; }
    gchar *buf = g_malloc(QV_TEXT_MAX + 1);
    gssize got = 0;
    while (got < QV_TEXT_MAX) {
        gssize n = read(fd, buf + got, QV_TEXT_MAX - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += n;
    }
    close(fd);
    buf[got] = '\0';
    if (memchr(buf, '\0', MIN(got, QV_BINARY_CHK))) {
        show_text(q, title, "Binary file – F3 opens the viewer");
    } else {
        gchar *valid = g_utf8_make_valid(buf, got);
        if (st.st_size > got) {
            gchar *t = g_strconcat(valid, "\n\n… (only the beginning is shown – F3 views all)", NULL);
            g_free(valid);
            valid = t;
        }
        show_text(q, title, valid);
        g_free(valid);
    }
    g_free(buf);
out:
    g_free(title);
    g_free(size);
    g_free(base);
}

static gboolean qv_tick(gpointer d)
{
    FM *fm = d;
    if (!fm->qv_on) return G_SOURCE_REMOVE;
    Panel *q = &fm->panels[fm->qv_panel];
    Panel *a = &fm->panels[fm->qv_panel ^ 1];

    gchar *path = NULL;
    const char *msg = NULL;
    if (a->ssh_conn)      msg = "Quick view shows local files only (SFTP panel)";
    else if (a->arc_file) msg = "Quick view: inside an archive – F3 views a file";
    else {
        gchar *name = panel_cursor_name(a);
        if (name && strcmp(name, "..") != 0) path = panel_cursor_fullpath(a);
        g_free(name);
        if (!path) msg = "";
    }
    const char *key = path ? path : msg;
    if (g_strcmp0(key, fm->qv_path) != 0) {
        g_free(fm->qv_path);
        fm->qv_path = g_strdup(key);
        if (path) qv_render(fm, q, path);
        else      show_text(q, "Quick view", msg);
    }
    g_free(path);
    return G_SOURCE_CONTINUE;
}

void qv_close(FM *fm)
{
    if (!fm->qv_on) return;
    Panel *q = &fm->panels[fm->qv_panel];
    fm->qv_on = FALSE;
    if (fm->qv_timer) { g_source_remove(fm->qv_timer); fm->qv_timer = 0; }
    g_clear_pointer(&fm->qv_path, g_free);
    if (q->qv_box) {
        gtk_picture_set_paintable(GTK_PICTURE(qv_pic(q)), NULL);
        gtk_widget_set_visible(q->qv_box, FALSE);
    }
    gtk_widget_set_visible(q->list_scroll, TRUE);
}

/* pin the divider where it is now; an unset GtkPaned position follows the
 * children's natural widths, so swapping list <-> quick view would move it */
static void qv_pin_paned(FM *fm)
{
    if (!fm->paned) return;
    gboolean set = FALSE;
    g_object_get(fm->paned, "position-set", &set, NULL);
    if (!set)
        gtk_paned_set_position(GTK_PANED(fm->paned),
                               gtk_paned_get_position(GTK_PANED(fm->paned)));
}

void qv_toggle(FM *fm)
{
    qv_pin_paned(fm);
    if (qv_active(fm)) {
        qv_close(fm);
        panel_focus(active_panel(fm));
        fm_status(fm, "Quick view closed");
        return;
    }
    Panel *q = other_panel(fm);
    if (!q->qv_box) qv_build(q);
    fm->qv_panel = q->idx;
    fm->qv_on = TRUE;
    gtk_widget_set_visible(q->filter_bar, FALSE);
    gtk_widget_set_visible(q->list_scroll, FALSE);
    gtk_widget_set_visible(q->qv_box, TRUE);
    qv_tick(fm);
    fm->qv_timer = g_timeout_add(QV_POLL_MS, qv_tick, fm);
    panel_focus(active_panel(fm));
    fm_status(fm, "Quick view – Ctrl+Q closes it");
}
