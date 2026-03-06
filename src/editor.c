#include "fm.h"
#include <string.h>
#include <errno.h>
#ifdef HAVE_GTKSOURCEVIEW
#  include <gtksourceview/gtksource.h>
#endif

/* ─────────────────────────────────────────────────────────────────── *
 *  Simple text editor (F4)
 *  – classic window: text view + status bar, no custom toolbar
 *  – Ctrl+S saves; title shows ● when modified
 *  – optional line-number gutter
 *  – font controlled by editor_css_provider (fm->editor_css_provider)
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GtkWidget     *window;
    GtkWidget     *text_view;
    GtkWidget     *line_num_area;
    GtkWidget     *status_lbl;
    GtkTextBuffer *buffer;
    GtkAdjustment *vadj;
    gchar         *filepath;      /* local path, or NULL for SSH */
    gchar         *remote_path;   /* full remote path when editing via SFTP */
    SshConn       *ssh_conn;      /* non-NULL when editing a remote file */
    FM            *fm;
    gboolean       modified;
    guint          hl_idle;
} EditorCtx;

/* ── line-number gutter ──────────────────────────────────────────── */

#ifndef HAVE_GTKSOURCEVIEW
static void line_num_draw(GtkDrawingArea *da, cairo_t *cr,
                           int width, int height, gpointer user_data)
{
    EditorCtx *ctx = user_data;
    GtkTextView *tv = GTK_TEXT_VIEW(ctx->text_view);

    cairo_set_source_rgb(cr, 0.93, 0.93, 0.93);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_move_to(cr, width - 1, 0);
    cairo_line_to(cr, width - 1, height);
    cairo_stroke(cr);

    GdkRectangle vis;
    gtk_text_view_get_visible_rect(tv, &vis);

    GtkTextIter it, end_it;
    gtk_text_view_get_iter_at_location(tv, &it,     vis.x, vis.y);
    gtk_text_view_get_iter_at_location(tv, &end_it, vis.x, vis.y + vis.height);
    gtk_text_iter_set_line_offset(&it, 0);

    const char *fam = ctx->fm->editor_font_family && ctx->fm->editor_font_family[0]
                      ? ctx->fm->editor_font_family : "Monospace";
    int fsz = ctx->fm->editor_linenum_font_size > 0 ? ctx->fm->editor_linenum_font_size : 10;

    PangoLayout *layout = gtk_widget_create_pango_layout(GTK_WIDGET(da), "0");
    PangoFontDescription *fd = pango_font_description_new();
    pango_font_description_set_family(fd, fam);
    pango_font_description_set_size(fd, fsz * PANGO_SCALE);
    pango_layout_set_font_description(layout, fd);
    pango_font_description_free(fd);

    cairo_set_source_rgb(cr, 0.45, 0.45, 0.45);

    while (TRUE) {
        GdkRectangle loc;
        gtk_text_view_get_iter_location(tv, &it, &loc);

        int wy;
        gtk_text_view_buffer_to_window_coords(tv, GTK_TEXT_WINDOW_WIDGET,
                                               0, loc.y, NULL, &wy);
        if (wy > height) break;

        if (wy + loc.height >= 0) {
            char num[16];
            g_snprintf(num, sizeof(num), "%d", gtk_text_iter_get_line(&it) + 1);
            pango_layout_set_text(layout, num, -1);
            int tw, th;
            pango_layout_get_pixel_size(layout, &tw, &th);
            cairo_move_to(cr, width - tw - 6, wy + (loc.height - th) / 2.0);
            pango_cairo_show_layout(cr, layout);
        }

        if (gtk_text_iter_compare(&it, &end_it) >= 0) break;
        if (!gtk_text_iter_forward_line(&it)) break;
    }
    g_object_unref(layout);
}

static void on_scroll_changed(GtkAdjustment *adj, gpointer user_data)
{
    (void)adj;
    EditorCtx *ctx = user_data;
    if (ctx->line_num_area)
        gtk_widget_queue_draw(ctx->line_num_area);
}
#endif /* !HAVE_GTKSOURCEVIEW */

/* ── syntax highlighting ─────────────────────────────────────────── */

static gboolean hl_idle_cb(gpointer user_data)
{
    EditorCtx *ctx = user_data;
    ctx->hl_idle = 0;
    highlight_apply(ctx->buffer, ctx->filepath, ctx->fm->syntax_highlight);
    return G_SOURCE_REMOVE;
}

static void schedule_highlight(EditorCtx *ctx)
{
    if (ctx->hl_idle) g_source_remove(ctx->hl_idle);
    ctx->hl_idle = g_timeout_add(300, hl_idle_cb, ctx);
}

/* ── modification tracking ───────────────────────────────────────── */

static void on_buffer_changed(GtkTextBuffer *buf, gpointer user_data)
{
    (void)buf;
    EditorCtx *ctx = user_data;
    if (!ctx->modified) {
        ctx->modified = TRUE;
        const gchar *dp = ctx->remote_path ? ctx->remote_path
                        : ctx->filepath   ? ctx->filepath
                        : "untitled";
        gchar *t = g_strdup_printf("● %s", dp);
        gtk_window_set_title(GTK_WINDOW(ctx->window), t);
        g_free(t);
    }
    if (ctx->line_num_area)
        gtk_widget_queue_draw(ctx->line_num_area);
    schedule_highlight(ctx);
}

/* ── status bar ──────────────────────────────────────────────────── */

static void update_status(EditorCtx *ctx)
{
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer,
        &it, gtk_text_buffer_get_insert(ctx->buffer));
    gchar *msg = g_strdup_printf("Line %d, Column %d",
                                  gtk_text_iter_get_line(&it) + 1,
                                  gtk_text_iter_get_line_offset(&it) + 1);
    gtk_label_set_text(GTK_LABEL(ctx->status_lbl), msg);
    g_free(msg);
}

static void on_cursor_moved(GtkTextBuffer *buf, GtkTextIter *loc,
                             GtkTextMark *mark, gpointer user_data)
{
    (void)buf; (void)loc;
    EditorCtx *ctx = user_data;
    if (mark == gtk_text_buffer_get_insert(ctx->buffer))
        update_status(ctx);
}

/* ── save ────────────────────────────────────────────────────────── */

static void editor_save(EditorCtx *ctx)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(ctx->buffer, &start, &end);
    gchar *text = gtk_text_buffer_get_text(ctx->buffer, &start, &end, FALSE);
    gsize  len  = strlen(text);

    gboolean ok = FALSE;
    const gchar *display_path = NULL;

    if (ctx->ssh_conn) {
        display_path = ctx->remote_path;
        ok = ssh_write_file(ctx->ssh_conn, ctx->remote_path, text, len);
        if (!ok)
            fm_status(ctx->fm, "SFTP save error: %s", ctx->remote_path);
    } else if (ctx->filepath) {
        display_path = ctx->filepath;
        GError *err = NULL;
        ok = g_file_set_contents(ctx->filepath, text, (gssize)len, &err);
        if (!ok) {
            fm_status(ctx->fm, "Save error: %s", err ? err->message : "?");
            g_clear_error(&err);
        }
    }

    g_free(text);

    if (ok) {
        ctx->modified = FALSE;
        gtk_window_set_title(GTK_WINDOW(ctx->window), display_path);
        fm_status(ctx->fm, "Saved: %s", display_path);
    }
}

/* ── keyboard ────────────────────────────────────────────────────── */

static gboolean on_editor_key(GtkEventControllerKey *ctrl,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    (void)ctrl; (void)keycode;
    EditorCtx *ctx = user_data;
    if ((state & GDK_CONTROL_MASK) && (keyval == GDK_KEY_s || keyval == GDK_KEY_S)) {
        editor_save(ctx);
        return TRUE;
    }
    return FALSE;
}

/* ── cleanup ─────────────────────────────────────────────────────── */

static gboolean on_editor_close(GtkWindow *win, gpointer user_data)
{
    (void)win;
    EditorCtx *ctx = user_data;
    if (ctx->hl_idle) g_source_remove(ctx->hl_idle);
    FM *fm = ctx->fm;
    g_free(ctx->filepath);
    g_free(ctx->remote_path);
    g_free(ctx);
    Panel *ap = active_panel(fm);
    ap->inhibit_sel = TRUE;
    gtk_widget_grab_focus(ap->column_view);
    return FALSE;
}

/* ── public entry point ──────────────────────────────────────────── */

void editor_show(FM *fm)
{
    Panel *p = active_panel(fm);

    gchar *name = panel_cursor_name(p);
    if (!name || strcmp(name, "..") == 0) { g_free(name); return; }

    gchar  *content     = NULL;
    gsize   content_len = 0;
    gchar  *filepath    = NULL;
    gchar  *remote_path = NULL;
    SshConn *ssh_conn   = NULL;

    if (p->ssh_conn) {
        remote_path = g_build_filename(p->ssh_conn->remote_path, name, NULL);
        g_free(name);
        content = ssh_read_file(p->ssh_conn, remote_path, &content_len);
        if (!content) {
            fm_status(fm, "Editor: SFTP read error – %s", remote_path);
            g_free(remote_path);
            return;
        }
        ssh_conn = p->ssh_conn;
    } else {
        filepath = g_build_filename(p->cwd, name, NULL);
        g_free(name);

        struct stat st;
        if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) {
            g_free(filepath);
            fm_status(fm, "Editor: not a file");
            return;
        }

        GError *err = NULL;
        g_file_get_contents(filepath, &content, &content_len, &err);
        if (err) {
            fm_status(fm, "Editor: read error – %s", err->message);
            g_clear_error(&err);
            g_free(filepath);
            return;
        }
    }

    if (!g_utf8_validate(content, (gssize)content_len, NULL)) {
        gchar *fixed = g_utf8_make_valid(content, (gssize)content_len);
        g_free(content);
        content = fixed;
    }

    EditorCtx *ctx = g_new0(EditorCtx, 1);
    ctx->fm          = fm;
    ctx->filepath    = filepath;
    ctx->remote_path = remote_path;
    ctx->ssh_conn    = ssh_conn;
    ctx->modified    = FALSE;

    const gchar *title = ctx->remote_path ? ctx->remote_path : ctx->filepath;

    /* window */
    ctx->window = gtk_window_new();
    gtk_widget_add_css_class(ctx->window, "fm-editor-win");
    gtk_window_set_title(GTK_WINDOW(ctx->window), title);
    gtk_window_set_default_size(GTK_WINDOW(ctx->window), 860, 640);
    gtk_window_set_transient_for(GTK_WINDOW(ctx->window),
                                  GTK_WINDOW(fm->window));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(ctx->window), vbox);

    /* main area: [gutter] + scrolled textview */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_append(GTK_BOX(vbox), hbox);

#ifdef HAVE_GTKSOURCEVIEW
    GtkSourceBuffer *_sbuf = gtk_source_buffer_new(NULL);
    if (fm->editor_style_scheme && fm->editor_style_scheme[0]) {
        GtkSourceStyleSchemeManager *_sm =
            gtk_source_style_scheme_manager_get_default();
        GtkSourceStyleScheme *_sc =
            gtk_source_style_scheme_manager_get_scheme(_sm, fm->editor_style_scheme);
        if (_sc) gtk_source_buffer_set_style_scheme(_sbuf, _sc);
    }
    ctx->buffer    = GTK_TEXT_BUFFER(_sbuf);
    ctx->text_view = gtk_source_view_new_with_buffer(_sbuf);
    gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(ctx->text_view), 4);
    gtk_source_view_set_highlight_current_line(GTK_SOURCE_VIEW(ctx->text_view), TRUE);
#else
    ctx->buffer    = gtk_text_buffer_new(NULL);
    ctx->text_view = gtk_text_view_new_with_buffer(ctx->buffer);
#endif
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx->text_view), GTK_WRAP_NONE);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(ctx->text_view), 4);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(ctx->text_view), 4);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(ctx->text_view), 4);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(ctx->text_view), 4);
    /* CSS class targeted by fm->editor_css_provider */
    gtk_widget_add_css_class(ctx->text_view, "fm-editor");

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ctx->text_view);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_widget_set_vexpand(scroll, TRUE);

    ctx->vadj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));

#ifndef HAVE_GTKSOURCEVIEW
    /* line number gutter */
    if (fm->editor_line_numbers) {
        int n_lines = 1;
        for (const gchar *q = content; *q; q++)
            if (*q == '\n') n_lines++;
        int digits = 1;
        for (int v = n_lines; v >= 10; v /= 10) digits++;
        int gutter_w = MAX(36, digits * (fm->editor_linenum_font_size > 0
                                          ? fm->editor_linenum_font_size : 10) + 16);

        ctx->line_num_area = gtk_drawing_area_new();
        gtk_widget_set_size_request(ctx->line_num_area, gutter_w, -1);
        gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ctx->line_num_area),
                                       line_num_draw, ctx, NULL);
        gtk_box_append(GTK_BOX(hbox), ctx->line_num_area);

        g_signal_connect(ctx->vadj, "value-changed",
                         G_CALLBACK(on_scroll_changed), ctx);
    }
#else
    /* GtkSourceView handles line numbers natively */
    gtk_source_view_set_show_line_numbers(
        GTK_SOURCE_VIEW(ctx->text_view), fm->editor_line_numbers);
    ctx->line_num_area = NULL;
    ctx->vadj = NULL;
#endif
    gtk_box_append(GTK_BOX(hbox), scroll);

    /* status bar */
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(vbox), sep);
    ctx->status_lbl = gtk_label_new("Line 1, Column 1");
    gtk_widget_add_css_class(ctx->status_lbl, "fm-editor-status");
    gtk_widget_set_halign(ctx->status_lbl, GTK_ALIGN_START);
    gtk_widget_set_margin_start(ctx->status_lbl, 6);
    gtk_widget_set_margin_top(ctx->status_lbl, 2);
    gtk_widget_set_margin_bottom(ctx->status_lbl, 2);
    gtk_box_append(GTK_BOX(vbox), ctx->status_lbl);

    /* load content – suppress modified flag */
    g_signal_handlers_block_by_func(ctx->buffer, on_buffer_changed, ctx);
    gtk_text_buffer_set_text(ctx->buffer, content, -1);
    g_free(content);
    g_signal_handlers_unblock_by_func(ctx->buffer, on_buffer_changed, ctx);
#ifdef HAVE_GTKSOURCEVIEW
    {
        GtkSourceLanguageManager *_lm = gtk_source_language_manager_get_default();
        GtkSourceLanguage *_lang =
            gtk_source_language_manager_guess_language(_lm, title, NULL);
        gtk_source_buffer_set_language(GTK_SOURCE_BUFFER(ctx->buffer), _lang);
        gtk_source_buffer_set_highlight_syntax(
            GTK_SOURCE_BUFFER(ctx->buffer), fm->syntax_highlight);
    }
#endif

    /* signals */
    g_signal_connect(ctx->buffer, "changed",
                     G_CALLBACK(on_buffer_changed), ctx);
    g_signal_connect(ctx->buffer, "mark-set",
                     G_CALLBACK(on_cursor_moved), ctx);

    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_editor_key), ctx);
    gtk_widget_add_controller(ctx->window, kc);

    g_signal_connect(ctx->window, "close-request",
                     G_CALLBACK(on_editor_close), ctx);

    highlight_apply(ctx->buffer, title, fm->syntax_highlight);

    gtk_window_present(GTK_WINDOW(ctx->window));
}
