#include "fm.h"
#include <string.h>
#ifdef HAVE_GTKSOURCEVIEW
#  include <gtksourceview/gtksource.h>
#endif

#define VIEWER_MAX_BYTES (1024 * 1024)  /* 1 MB */

/* ─────────────────────────────────────────────────────────────────── *
 *  File viewer – F3 (local + SSH)
 * ─────────────────────────────────────────────────────────────────── */

static gboolean on_viewer_key_press(GtkEventControllerKey *ctrl,
                                     guint keyval, guint keycode,
                                     GdkModifierType state, GMainLoop *loop)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_F3) {
        g_main_loop_quit(loop);
        return TRUE;
    }
    return FALSE;
}

void viewer_show(FM *fm)
{
    Panel *p = active_panel(fm);

    gchar *name = panel_cursor_name(p);
    if (!name || strcmp(name, "..") == 0) {
        g_free(name);
        fm_status(fm, "No file selected");
        return;
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
        gchar *fullpath = g_build_filename(p->cwd, name, NULL);
        struct stat st;
        if (lstat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
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

    gchar *display_text;
    if (g_utf8_validate(content, (gssize)length, NULL)) {
        display_text = g_strndup(content, length);
    } else {
        display_text = g_utf8_make_valid(content, (gssize)length);
    }
    g_free(content);

    /* Build window */
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);

    gchar *title = g_strdup_printf("View: %s", name);
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);
    g_free(title);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(vbox, 4);
    gtk_widget_set_margin_end(vbox, 4);
    gtk_widget_set_margin_top(vbox, 4);
    gtk_widget_set_margin_bottom(vbox, 4);

    if (truncated) {
        GtkWidget *notice = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(notice),
            "<small><i>File is large – showing first 1 MB</i></small>");
        gtk_widget_set_halign(notice, GTK_ALIGN_START);
        gtk_box_append(GTK_BOX(vbox), notice);
    }

#ifdef HAVE_GTKSOURCEVIEW
    GtkSourceBuffer *_vsbuf = gtk_source_buffer_new(NULL);
    if (fm->editor_style_scheme && fm->editor_style_scheme[0]) {
        GtkSourceStyleSchemeManager *_sm =
            gtk_source_style_scheme_manager_get_default();
        GtkSourceStyleScheme *_sc =
            gtk_source_style_scheme_manager_get_scheme(_sm, fm->editor_style_scheme);
        if (_sc) gtk_source_buffer_set_style_scheme(_vsbuf, _sc);
    }
    GtkTextBuffer *buf = GTK_TEXT_BUFFER(_vsbuf);
    GtkWidget *text_view = gtk_source_view_new_with_buffer(_vsbuf);
    gtk_widget_add_css_class(text_view, "fm-viewer");
    gtk_source_view_set_show_line_numbers(GTK_SOURCE_VIEW(text_view), FALSE);
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);
    gtk_text_buffer_set_text(buf, display_text, (gint)strlen(display_text));
    g_free(display_text);
    {
        GtkSourceLanguageManager *_lm = gtk_source_language_manager_get_default();
        GtkSourceLanguage *_lang =
            gtk_source_language_manager_guess_language(_lm, name, NULL);
        gtk_source_buffer_set_language(_vsbuf, _lang);
        gtk_source_buffer_set_highlight_syntax(_vsbuf, fm->syntax_highlight);
    }
#else
    GtkWidget *text_view = gtk_text_view_new();
    gtk_widget_add_css_class(text_view, "fm-viewer");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(text_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(text_view), GTK_WRAP_NONE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(text_view), TRUE);
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(text_view));
    gtk_text_buffer_set_text(buf, display_text, (gint)strlen(display_text));
    g_free(display_text);
    highlight_apply(buf, name, fm->syntax_highlight);
#endif

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), text_view);
    gtk_widget_set_vexpand(scroll, TRUE);

    /* Close button */
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(close_btn, 4);

    gtk_box_append(GTK_BOX(vbox), scroll);
    gtk_box_append(GTK_BOX(vbox), close_btn);
    gtk_window_set_child(GTK_WINDOW(win), vbox);

    /* Key handler: Esc or F3 to close */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_viewer_key_press), loop);
    gtk_widget_add_controller(win, key_ctrl);

    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(g_main_loop_quit), loop);
    g_signal_connect_swapped(win, "close-request", G_CALLBACK(g_main_loop_quit), loop);

    gtk_window_present(GTK_WINDOW(win));
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    gtk_window_destroy(GTK_WINDOW(win));

    Panel *ap = active_panel(fm);
    ap->inhibit_sel = TRUE;
    gtk_widget_grab_focus(ap->column_view);

    g_free(name);
}
