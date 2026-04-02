#include "fm.h"
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#ifdef HAVE_GTKSOURCEVIEW
#  include <gtksourceview/gtksource.h>
#endif

#define VIEWER_MAX_BYTES (50 * 1024 * 1024)  /* 50 MB - reasonable for text viewing */

/* ─────────────────────────────────────────────────────────────────── *
 *  File viewer – F3 (local + SSH)
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GMainLoop     *loop;
    GtkWidget     *window;
    GtkWidget     *text_view;
    GtkTextBuffer *buffer;
    GtkWidget     *search_bar;
    GtkWidget     *search_entry;
    GtkWidget     *match_lbl;
    GtkWidget     *match_map;
    GtkTextTag    *match_tag;
    GArray        *match_lines;
    int            current_match;
    int            total_matches;
    int            total_lines;
} ViewerCtx;

/* ── search functions ────────────────────────────────────────────── */

static void viewer_search_clear(ViewerCtx *ctx)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(ctx->buffer, &start, &end);
    gtk_text_buffer_remove_tag(ctx->buffer, ctx->match_tag, &start, &end);
    if (ctx->match_lines)
        g_array_set_size(ctx->match_lines, 0);
    ctx->total_matches = 0;
    ctx->current_match = 0;
    if (ctx->match_lbl)
        gtk_label_set_text(GTK_LABEL(ctx->match_lbl), "");
    if (ctx->match_map)
        gtk_widget_queue_draw(ctx->match_map);
}

static void viewer_search_highlight_all(ViewerCtx *ctx, const gchar *needle)
{
    viewer_search_clear(ctx);
    if (!needle || !needle[0]) return;

    GtkTextIter start, match_start, match_end;
    gtk_text_buffer_get_start_iter(ctx->buffer, &start);

    int count = 0;
    while (gtk_text_iter_forward_search(&start, needle,
                GTK_TEXT_SEARCH_CASE_INSENSITIVE, &match_start, &match_end, NULL)) {
        gtk_text_buffer_apply_tag(ctx->buffer, ctx->match_tag, &match_start, &match_end);
        int line = gtk_text_iter_get_line(&match_start);
        g_array_append_val(ctx->match_lines, line);
        count++;
        start = match_end;
    }

    ctx->total_matches = count;
    ctx->current_match = count > 0 ? 1 : 0;

    if (ctx->match_lbl) {
        if (count > 0) {
            gchar *txt = g_strdup_printf("%d of %d", ctx->current_match, ctx->total_matches);
            gtk_label_set_text(GTK_LABEL(ctx->match_lbl), txt);
            g_free(txt);
        } else {
            gtk_label_set_text(GTK_LABEL(ctx->match_lbl), "0 results");
        }
    }
    if (ctx->match_map)
        gtk_widget_queue_draw(ctx->match_map);
}

static void viewer_search_goto_match(ViewerCtx *ctx, int index)
{
    if (ctx->total_matches == 0 || index < 1 || index > ctx->total_matches)
        return;

    const gchar *needle = gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
    if (!needle || !needle[0]) return;

    GtkTextIter start, match_start, match_end;
    gtk_text_buffer_get_start_iter(ctx->buffer, &start);

    int count = 0;
    while (gtk_text_iter_forward_search(&start, needle,
                GTK_TEXT_SEARCH_CASE_INSENSITIVE, &match_start, &match_end, NULL)) {
        count++;
        if (count == index) {
            gtk_text_buffer_select_range(ctx->buffer, &match_start, &match_end);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(ctx->text_view),
                                          &match_start, 0.2, FALSE, 0, 0);
            ctx->current_match = index;
            if (ctx->match_lbl) {
                gchar *txt = g_strdup_printf("%d of %d", ctx->current_match, ctx->total_matches);
                gtk_label_set_text(GTK_LABEL(ctx->match_lbl), txt);
                g_free(txt);
            }
            return;
        }
        start = match_end;
    }
}

static void viewer_search_next(ViewerCtx *ctx)
{
    if (ctx->total_matches == 0) return;
    int next = ctx->current_match + 1;
    if (next > ctx->total_matches) next = 1;
    viewer_search_goto_match(ctx, next);
}

static void viewer_search_prev(ViewerCtx *ctx)
{
    if (ctx->total_matches == 0) return;
    int prev = ctx->current_match - 1;
    if (prev < 1) prev = ctx->total_matches;
    viewer_search_goto_match(ctx, prev);
}

static void on_viewer_search_changed(GtkEditable *editable, gpointer user_data)
{
    ViewerCtx *ctx = user_data;
    const gchar *needle = gtk_editable_get_text(editable);
    viewer_search_highlight_all(ctx, needle);
    if (ctx->total_matches > 0)
        viewer_search_goto_match(ctx, 1);
}

static void on_viewer_search_next_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    viewer_search_next(user_data);
}

static void on_viewer_search_prev_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    viewer_search_prev(user_data);
}

static void on_viewer_search_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    ViewerCtx *ctx = user_data;
    viewer_search_clear(ctx);
    gtk_widget_set_visible(ctx->search_bar, FALSE);
    gtk_widget_grab_focus(ctx->text_view);
}

static gboolean on_viewer_search_key(GtkEventControllerKey *kc,
                                      guint keyval, guint keycode,
                                      GdkModifierType state, gpointer user_data)
{
    (void)kc; (void)keycode;
    ViewerCtx *ctx = user_data;

    if (keyval == GDK_KEY_Escape) {
        viewer_search_clear(ctx);
        gtk_widget_set_visible(ctx->search_bar, FALSE);
        gtk_widget_grab_focus(ctx->text_view);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (state & GDK_SHIFT_MASK)
            viewer_search_prev(ctx);
        else
            viewer_search_next(ctx);
        return TRUE;
    }
    if (keyval == GDK_KEY_F3) {
        if (state & GDK_SHIFT_MASK)
            viewer_search_prev(ctx);
        else
            viewer_search_next(ctx);
        return TRUE;
    }
    return FALSE;
}

static void viewer_match_map_draw(GtkDrawingArea *da, cairo_t *cr,
                                   int width, int height, gpointer user_data)
{
    (void)da;
    ViewerCtx *ctx = user_data;

    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_paint(cr);

    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, 0, height);
    cairo_stroke(cr);

    if (!ctx->match_lines || ctx->match_lines->len == 0 || ctx->total_lines <= 0)
        return;

    cairo_set_source_rgb(cr, 1.0, 0.8, 0.0);
    for (guint i = 0; i < ctx->match_lines->len; i++) {
        int line = g_array_index(ctx->match_lines, int, i);
        double y = (double)line / ctx->total_lines * height;
        cairo_rectangle(cr, 2, y - 1, width - 4, 3);
        cairo_fill(cr);
    }
}

static void viewer_show_search_bar(ViewerCtx *ctx)
{
    gtk_widget_set_visible(ctx->search_bar, TRUE);
    gtk_widget_grab_focus(ctx->search_entry);
}

static gboolean on_viewer_key_press(GtkEventControllerKey *ctrl,
                                     guint keyval, guint keycode,
                                     GdkModifierType state, gpointer user_data)
{
    (void)ctrl; (void)keycode;
    ViewerCtx *ctx = user_data;
    gboolean ctrl_mod = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    if (keyval == GDK_KEY_Escape) {
        if (gtk_widget_get_visible(ctx->search_bar)) {
            viewer_search_clear(ctx);
            gtk_widget_set_visible(ctx->search_bar, FALSE);
            gtk_widget_grab_focus(ctx->text_view);
            return TRUE;
        }
        g_main_loop_quit(ctx->loop);
        return TRUE;
    }
    if (keyval == GDK_KEY_F3 && !ctrl_mod) {
        if (gtk_widget_get_visible(ctx->search_bar)) {
            if (shift)
                viewer_search_prev(ctx);
            else
                viewer_search_next(ctx);
        } else {
            g_main_loop_quit(ctx->loop);
        }
        return TRUE;
    }
    if (ctrl_mod && keyval == GDK_KEY_f) {
        viewer_show_search_bar(ctx);
        return TRUE;
    }
    return FALSE;
}

/* Check if filename has an image extension */
static gboolean is_image_file(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return FALSE;
    const char *exts[] = {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".svg", ".webp",
        ".tiff", ".tif", ".ico", ".xpm", ".pbm", ".pgm", ".ppm",
        NULL
    };
    for (int i = 0; exts[i]; i++)
        if (g_ascii_strcasecmp(dot, exts[i]) == 0) return TRUE;
    return FALSE;
}

static gboolean on_image_viewer_key(GtkEventControllerKey *ctrl, guint kv,
                                    guint kc, GdkModifierType mod, gpointer data)
{
    (void)ctrl; (void)kc; (void)mod;
    if (kv == GDK_KEY_Escape || kv == GDK_KEY_q || kv == GDK_KEY_Q) {
        g_main_loop_quit(data);
        return TRUE;
    }
    return FALSE;
}

/* Image viewer — simple window with GtkPicture + close/key handling */
static void viewer_show_image(FM *fm, const char *name, const char *fullpath)
{
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    gchar *title = g_strdup_printf("View: %s", name);
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 700);
    g_free(title);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    GFile *file = g_file_new_for_path(fullpath);
    GtkWidget *picture = gtk_picture_new_for_file(file);
    g_object_unref(file);
    gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
    gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
    gtk_widget_set_vexpand(picture, TRUE);
    gtk_widget_set_hexpand(picture, TRUE);
    gtk_box_append(GTK_BOX(vbox), picture);

    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(close_btn, 4);
    gtk_widget_set_margin_end(close_btn, 4);
    gtk_widget_set_margin_bottom(close_btn, 4);
    gtk_box_append(GTK_BOX(vbox), close_btn);

    gtk_window_set_child(GTK_WINDOW(win), vbox);

    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(g_main_loop_quit), loop);
    g_signal_connect_swapped(win, "close-request", G_CALLBACK(g_main_loop_quit), loop);

    /* Escape / Q closes */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed",
        G_CALLBACK(on_image_viewer_key), loop);
    gtk_widget_add_controller(win, key_ctrl);

    gtk_window_present(GTK_WINDOW(win));
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    gtk_window_destroy(GTK_WINDOW(win));

    Panel *ap = active_panel(fm);
    ap->inhibit_sel = TRUE;
    gtk_widget_grab_focus(ap->column_view);
}

void viewer_show(FM *fm)
{
    Panel *p = active_panel(fm);

    gchar *name = panel_cursor_name(p);
    if (!name || strcmp(name, "..") == 0) {
        g_free(name);
        fm_status(fm, "No file is selected");
        return;
    }

    /* Image preview — local files only, when enabled */
    if (fm->viewer_image_preview && !p->ssh_conn && is_image_file(name)) {
        gchar *fullpath = panel_cursor_fullpath(p);
        if (fullpath) {
            struct stat st;
            if (lstat(fullpath, &st) == 0 && S_ISREG(st.st_mode)) {
                viewer_show_image(fm, name, fullpath);
                g_free(fullpath);
                g_free(name);
                return;
            }
            g_free(fullpath);
        }
    }

    gchar *content = NULL;
    gsize  length  = 0;

    if (p->ssh_conn) {
#ifdef HAVE_LIBSSH2
        gchar *remote_path = g_strdup_printf("%s/%s",
            p->ssh_conn->remote_path, name);
        content = ssh_read_file(p->ssh_conn, remote_path, &length);
        g_free(remote_path);
        if (!content) {
            fm_status(fm, "Viewer: error reading remote file '%s'", name);
            g_free(name);
            return;
        }
#else
        fm_status(fm, "Remote file viewer is not supported");
        g_free(name);
        return;
#endif
    } else {
        gchar *fullpath = panel_cursor_fullpath(p);
        struct stat st;
        if (!fullpath || (lstat(fullpath, &st) == 0 && S_ISDIR(st.st_mode))) {
            fm_status(fm, "Viewer: '%s' is a directory", name);
            g_free(name);
            g_free(fullpath);
            return;
        }

        GError *err = NULL;
        if (!g_file_get_contents(fullpath, &content, &length, &err)) {
            fm_status(fm, "Viewer: read error – %s", err ? err->message : "?");
            g_clear_error(&err);
            g_free(name);
            g_free(fullpath);
            return;
        }
        g_free(fullpath);
    }

    gboolean truncated = FALSE;
    if (length > VIEWER_MAX_BYTES) {
        length    = VIEWER_MAX_BYTES;
        truncated = TRUE;
    }

    /* Avoid unnecessary copy for valid UTF-8 */
    gchar *display_text;
    gsize  display_len;
    if (g_utf8_validate(content, (gssize)length, NULL)) {
        if (truncated) {
            content[length] = '\0';
        }
        display_text = content;
        display_len  = length;
        content = NULL;
    } else {
        display_text = g_utf8_make_valid(content, (gssize)length);
        display_len  = strlen(display_text);
        g_free(content);
    }

    /* Initialize viewer context */
    ViewerCtx ctx = {0};
    ctx.loop = g_main_loop_new(NULL, FALSE);
    ctx.match_lines = g_array_new(FALSE, FALSE, sizeof(int));

    /* Count lines for match map */
    ctx.total_lines = 1;
    for (const gchar *q = display_text; *q; q++)
        if (*q == '\n') ctx.total_lines++;

    /* Build window */
    gchar *title = g_strdup_printf("View: %s", name);
    ctx.window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(ctx.window), title);
    gtk_window_set_transient_for(GTK_WINDOW(ctx.window), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(ctx.window), 900, 600);
    g_free(title);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(vbox, 4);
    gtk_widget_set_margin_end(vbox, 4);
    gtk_widget_set_margin_top(vbox, 4);
    gtk_widget_set_margin_bottom(vbox, 4);

    /* Search bar (initially hidden) */
    ctx.search_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_bottom(ctx.search_bar, 4);
    gtk_widget_set_visible(ctx.search_bar, FALSE);

    GtkWidget *search_lbl = gtk_label_new("Search:");
    ctx.search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(ctx.search_entry, TRUE);
    GtkWidget *prev_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(prev_btn, "Previous (Shift+F3)");
    GtkWidget *next_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(next_btn, "Next (F3)");
    ctx.match_lbl = gtk_label_new("");
    gtk_widget_set_size_request(ctx.match_lbl, 80, -1);
    GtkWidget *close_search_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(close_search_btn, "Close (Esc)");

    gtk_box_append(GTK_BOX(ctx.search_bar), search_lbl);
    gtk_box_append(GTK_BOX(ctx.search_bar), ctx.search_entry);
    gtk_box_append(GTK_BOX(ctx.search_bar), prev_btn);
    gtk_box_append(GTK_BOX(ctx.search_bar), next_btn);
    gtk_box_append(GTK_BOX(ctx.search_bar), ctx.match_lbl);
    gtk_box_append(GTK_BOX(ctx.search_bar), close_search_btn);
    gtk_box_append(GTK_BOX(vbox), ctx.search_bar);

    g_signal_connect(ctx.search_entry, "changed", G_CALLBACK(on_viewer_search_changed), &ctx);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_viewer_search_next_clicked), &ctx);
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_viewer_search_prev_clicked), &ctx);
    g_signal_connect(close_search_btn, "clicked", G_CALLBACK(on_viewer_search_close_clicked), &ctx);

    GtkEventController *search_kc = gtk_event_controller_key_new();
    g_signal_connect(search_kc, "key-pressed", G_CALLBACK(on_viewer_search_key), &ctx);
    gtk_widget_add_controller(ctx.search_entry, search_kc);

    if (truncated) {
        GtkWidget *notice = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(notice),
            "<small><i>File is large – showing first 50 MB</i></small>");
        gtk_widget_set_halign(notice, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), notice);
    }

    /* Main area with scroll and match map */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_append(GTK_BOX(vbox), hbox);

#ifdef HAVE_GTKSOURCEVIEW
    GtkSourceBuffer *_vsbuf = gtk_source_buffer_new(NULL);
    {
        const char *sid = fm_scheme_for_theme(fm);
        GtkSourceStyleSchemeManager *_sm =
            gtk_source_style_scheme_manager_get_default();
        GtkSourceStyleScheme *_sc =
            gtk_source_style_scheme_manager_get_scheme(_sm, sid);
        if (_sc) gtk_source_buffer_set_style_scheme(_vsbuf, _sc);
    }
    ctx.buffer = GTK_TEXT_BUFFER(_vsbuf);
    ctx.text_view = gtk_source_view_new_with_buffer(_vsbuf);
    gtk_widget_add_css_class(ctx.text_view, "fm-viewer");
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(ctx.text_view), fm->viewer_line_numbers);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctx.text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(ctx.text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx.text_view), GTK_WRAP_NONE);
    gtk_text_buffer_set_text(ctx.buffer, display_text, (gint)display_len);
    {
        GtkSourceLanguage *_lang = source_guess_language(name);
        gtk_source_buffer_set_language(_vsbuf, _lang);
        gtk_source_buffer_set_highlight_syntax(_vsbuf, fm->viewer_syntax_highlight);
    }
#else
    ctx.text_view = gtk_text_view_new();
    gtk_widget_add_css_class(ctx.text_view, "fm-viewer");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(ctx.text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(ctx.text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx.text_view), GTK_WRAP_NONE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(ctx.text_view), TRUE);
    ctx.buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(ctx.text_view));
    gtk_text_buffer_set_text(ctx.buffer, display_text, (gint)display_len);
    highlight_apply(ctx.buffer, name, fm->viewer_syntax_highlight);
#endif
    g_free(display_text);

    /* Create match tag for search highlighting */
    ctx.match_tag = gtk_text_buffer_create_tag(ctx.buffer, "search-match",
                        "background", fm->theme == 0 ? "#665500" : "#FFFF00",
                        "foreground", fm->theme == 0 ? "#FFFF00" : "#000000",
                        NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), ctx.text_view);
    gtk_widget_set_hexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(hbox), scroll);

    /* Match map (scrollbar indicator) */
    ctx.match_map = gtk_drawing_area_new();
    gtk_widget_set_size_request(ctx.match_map, 10, -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ctx.match_map),
                                   viewer_match_map_draw, &ctx, NULL);
    gtk_box_append(GTK_BOX(hbox), ctx.match_map);

    /* Close button */
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(close_btn, 4);

    gtk_box_append(GTK_BOX(vbox), close_btn);
    gtk_window_set_child(GTK_WINDOW(ctx.window), vbox);

    /* Key handler */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_viewer_key_press), &ctx);
    gtk_widget_add_controller(ctx.window, key_ctrl);

    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(g_main_loop_quit), ctx.loop);
    g_signal_connect_swapped(ctx.window, "close-request", G_CALLBACK(g_main_loop_quit), ctx.loop);

    gtk_window_present(GTK_WINDOW(ctx.window));
    g_main_loop_run(ctx.loop);
    g_main_loop_unref(ctx.loop);
    g_array_free(ctx.match_lines, TRUE);
    gtk_window_destroy(GTK_WINDOW(ctx.window));

    Panel *ap = active_panel(fm);
    ap->inhibit_sel = TRUE;
    gtk_widget_grab_focus(ap->column_view);

    g_free(name);
}
