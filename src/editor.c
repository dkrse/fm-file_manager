#include "fm.h"
#include <string.h>
#include <errno.h>
#ifdef HAVE_GTKSOURCEVIEW
#  include <gtksourceview/gtksource.h>
#endif

#define EDITOR_MAX_BYTES (100 * 1024 * 1024)  /* 100 MB max for editing */

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
    GtkWidget     *scroll;        /* scrolled window */
    gchar         *filepath;      /* local path, or NULL for SSH */
    gchar         *remote_path;   /* full remote path when editing via SFTP */
    SshConn       *ssh_conn;      /* non-NULL when editing a remote file */
    FM            *fm;
    gboolean       modified;
    guint          hl_idle;
    gboolean       word_wrap;
    /* search/replace */
    GtkWidget     *search_bar;
    GtkWidget     *search_entry;
    GtkWidget     *replace_entry;
    GtkWidget     *replace_row;
    GtkWidget     *match_map;     /* scrollbar match indicator */
    GtkWidget     *match_lbl;     /* "X of Y" label */
    GtkTextTag    *match_tag;
    GArray        *match_lines;   /* line numbers of matches (for scrollbar) */
    int            current_match;
    int            total_matches;
    int            total_lines;
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

/* ── menu actions ────────────────────────────────────────────────── */

static void action_save(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    editor_save(user_data);
}

static void action_close(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    gtk_window_close(GTK_WINDOW(ctx->window));
}

static void action_undo(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    if (gtk_text_buffer_get_can_undo(ctx->buffer))
        gtk_text_buffer_undo(ctx->buffer);
}

static void action_redo(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    if (gtk_text_buffer_get_can_redo(ctx->buffer))
        gtk_text_buffer_redo(ctx->buffer);
}

static void action_cut(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    GdkClipboard *clip = gtk_widget_get_clipboard(ctx->text_view);
    gtk_text_buffer_cut_clipboard(ctx->buffer, clip, TRUE);
}

static void action_copy(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    GdkClipboard *clip = gtk_widget_get_clipboard(ctx->text_view);
    gtk_text_buffer_copy_clipboard(ctx->buffer, clip);
}

static void action_paste(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    GdkClipboard *clip = gtk_widget_get_clipboard(ctx->text_view);
    gtk_text_buffer_paste_clipboard(ctx->buffer, clip, NULL, TRUE);
}

static void action_select_all(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(ctx->buffer, &start, &end);
    gtk_text_buffer_select_range(ctx->buffer, &start, &end);
}

static void action_word_wrap(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)param;
    EditorCtx *ctx = user_data;
    GVariant *state = g_action_get_state(G_ACTION(action));
    gboolean active = !g_variant_get_boolean(state);
    g_variant_unref(state);
    g_simple_action_set_state(action, g_variant_new_boolean(active));
    ctx->word_wrap = active;
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(ctx->text_view),
                                 active ? GTK_WRAP_WORD_CHAR : GTK_WRAP_NONE);
}

/* ── search/replace ──────────────────────────────────────────────── */

static void search_clear_highlights(EditorCtx *ctx)
{
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(ctx->buffer, &start, &end);
    gtk_text_buffer_remove_tag(ctx->buffer, ctx->match_tag, &start, &end);
    if (ctx->match_lines) {
        g_array_set_size(ctx->match_lines, 0);
    }
    ctx->total_matches = 0;
    ctx->current_match = 0;
    if (ctx->match_lbl)
        gtk_label_set_text(GTK_LABEL(ctx->match_lbl), "");
    if (ctx->match_map)
        gtk_widget_queue_draw(ctx->match_map);
}

static void search_highlight_all(EditorCtx *ctx, const gchar *needle)
{
    search_clear_highlights(ctx);

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

    /* Update match count label */
    if (ctx->match_lbl) {
        if (count > 0) {
            gchar *txt = g_strdup_printf("%d z %d", ctx->current_match, ctx->total_matches);
            gtk_label_set_text(GTK_LABEL(ctx->match_lbl), txt);
            g_free(txt);
        } else {
            gtk_label_set_text(GTK_LABEL(ctx->match_lbl), "0 results");
        }
    }

    /* Redraw match map */
    if (ctx->match_map)
        gtk_widget_queue_draw(ctx->match_map);
}

static void search_goto_match(EditorCtx *ctx, int index)
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
            gtk_text_buffer_place_cursor(ctx->buffer, &match_start);
            gtk_text_buffer_select_range(ctx->buffer, &match_start, &match_end);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(ctx->text_view),
                                          &match_start, 0.2, FALSE, 0, 0);
            ctx->current_match = index;

            if (ctx->match_lbl) {
                gchar *txt = g_strdup_printf("%d z %d", ctx->current_match, ctx->total_matches);
                gtk_label_set_text(GTK_LABEL(ctx->match_lbl), txt);
                g_free(txt);
            }
            return;
        }
        start = match_end;
    }
}

static void search_next(EditorCtx *ctx)
{
    if (ctx->total_matches == 0) return;
    int next = ctx->current_match + 1;
    if (next > ctx->total_matches) next = 1;
    search_goto_match(ctx, next);
}

static void search_prev(EditorCtx *ctx)
{
    if (ctx->total_matches == 0) return;
    int prev = ctx->current_match - 1;
    if (prev < 1) prev = ctx->total_matches;
    search_goto_match(ctx, prev);
}

static void on_search_changed(GtkEditable *editable, gpointer user_data)
{
    EditorCtx *ctx = user_data;
    const gchar *needle = gtk_editable_get_text(editable);
    search_highlight_all(ctx, needle);
    if (ctx->total_matches > 0) {
        search_goto_match(ctx, 1);
    }
}

static void on_search_next_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    search_next(user_data);
}

static void on_search_prev_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    search_prev(user_data);
}

static void on_search_close_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    EditorCtx *ctx = user_data;
    search_clear_highlights(ctx);
    gtk_widget_set_visible(ctx->search_bar, FALSE);
    gtk_widget_grab_focus(ctx->text_view);
}

static void on_replace_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    EditorCtx *ctx = user_data;

    if (ctx->total_matches == 0) return;

    const gchar *needle = gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
    const gchar *replacement = gtk_editable_get_text(GTK_EDITABLE(ctx->replace_entry));
    if (!needle || !needle[0]) return;

    /* Get current selection and verify it matches */
    GtkTextIter sel_start, sel_end;
    if (gtk_text_buffer_get_selection_bounds(ctx->buffer, &sel_start, &sel_end)) {
        gchar *selected = gtk_text_buffer_get_text(ctx->buffer, &sel_start, &sel_end, FALSE);
        if (g_ascii_strcasecmp(selected, needle) == 0) {
            gtk_text_buffer_delete(ctx->buffer, &sel_start, &sel_end);
            gtk_text_buffer_insert(ctx->buffer, &sel_start, replacement, -1);
        }
        g_free(selected);
    }

    /* Re-highlight and go to next */
    search_highlight_all(ctx, needle);
    if (ctx->total_matches > 0) {
        search_goto_match(ctx, ctx->current_match > ctx->total_matches ? 1 : ctx->current_match);
    }
}

static void on_replace_all_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    EditorCtx *ctx = user_data;

    const gchar *needle = gtk_editable_get_text(GTK_EDITABLE(ctx->search_entry));
    const gchar *replacement = gtk_editable_get_text(GTK_EDITABLE(ctx->replace_entry));
    if (!needle || !needle[0]) return;

    GtkTextIter start, match_start, match_end;
    gtk_text_buffer_get_start_iter(ctx->buffer, &start);

    int replaced = 0;
    gsize repl_len = strlen(replacement);

    gtk_text_buffer_begin_user_action(ctx->buffer);
    while (gtk_text_iter_forward_search(&start, needle,
                GTK_TEXT_SEARCH_CASE_INSENSITIVE, &match_start, &match_end, NULL)) {
        gtk_text_buffer_delete(ctx->buffer, &match_start, &match_end);
        gtk_text_buffer_insert(ctx->buffer, &match_start, replacement, -1);
        start = match_start;
        gtk_text_iter_forward_chars(&start, repl_len);
        replaced++;
    }
    gtk_text_buffer_end_user_action(ctx->buffer);

    search_highlight_all(ctx, needle);

    fm_status(ctx->fm, "Replaced: %d", replaced);
}

static gboolean on_search_key(GtkEventControllerKey *kc,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    (void)kc; (void)keycode;
    EditorCtx *ctx = user_data;

    if (keyval == GDK_KEY_Escape) {
        search_clear_highlights(ctx);
        gtk_widget_set_visible(ctx->search_bar, FALSE);
        gtk_widget_grab_focus(ctx->text_view);
        return TRUE;
    }
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        if (state & GDK_SHIFT_MASK)
            search_prev(ctx);
        else
            search_next(ctx);
        return TRUE;
    }
    if (keyval == GDK_KEY_F3) {
        if (state & GDK_SHIFT_MASK)
            search_prev(ctx);
        else
            search_next(ctx);
        return TRUE;
    }
    return FALSE;
}

/* Match map drawing (scrollbar indicator) */
static void match_map_draw(GtkDrawingArea *da, cairo_t *cr,
                            int width, int height, gpointer user_data)
{
    (void)da;
    EditorCtx *ctx = user_data;

    /* Background */
    cairo_set_source_rgb(cr, 0.92, 0.92, 0.92);
    cairo_paint(cr);

    /* Border */
    cairo_set_source_rgb(cr, 0.75, 0.75, 0.75);
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, 0, height);
    cairo_stroke(cr);

    if (!ctx->match_lines || ctx->match_lines->len == 0 || ctx->total_lines <= 0)
        return;

    /* Draw match markers */
    cairo_set_source_rgb(cr, 1.0, 0.8, 0.0);  /* Yellow/orange */

    for (guint i = 0; i < ctx->match_lines->len; i++) {
        int line = g_array_index(ctx->match_lines, int, i);
        double y = (double)line / ctx->total_lines * height;
        cairo_rectangle(cr, 2, y - 1, width - 4, 3);
        cairo_fill(cr);
    }
}

static void show_search_bar(EditorCtx *ctx, gboolean with_replace)
{
    gtk_widget_set_visible(ctx->search_bar, TRUE);
    gtk_widget_set_visible(ctx->replace_row, with_replace);
    gtk_widget_grab_focus(ctx->search_entry);

    /* If text is selected, use it as search term */
    GtkTextIter sel_start, sel_end;
    if (gtk_text_buffer_get_selection_bounds(ctx->buffer, &sel_start, &sel_end)) {
        gchar *selected = gtk_text_buffer_get_text(ctx->buffer, &sel_start, &sel_end, FALSE);
        if (selected && strlen(selected) < 100 && !strchr(selected, '\n')) {
            gtk_editable_set_text(GTK_EDITABLE(ctx->search_entry), selected);
            gtk_editable_select_region(GTK_EDITABLE(ctx->search_entry), 0, -1);
        }
        g_free(selected);
    }
}

static void action_find(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    show_search_bar(user_data, FALSE);
}

static void action_replace(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    show_search_bar(user_data, TRUE);
}

static void action_find_next(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    search_next(user_data);
}

static void action_find_prev(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    search_prev(user_data);
}

static void action_goto_line(GSimpleAction *action, GVariant *param, gpointer user_data)
{
    (void)action; (void)param;
    EditorCtx *ctx = user_data;

    /* Simple goto line dialog */
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Go to line");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(ctx->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 250, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);
    gtk_window_set_child(GTK_WINDOW(dlg), box);

    GtkWidget *lbl = gtk_label_new("Line number:");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(box), lbl);

    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_DIGITS);
    gtk_box_append(GTK_BOX(box), entry);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(box), btn_box);

    DlgCtx dctx;
    dlg_ctx_init(&dctx);
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(dlg_ctx_ok), &dctx);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(dlg_ctx_cancel), &dctx);
    g_signal_connect(dlg, "close-request", G_CALLBACK(dlg_ctx_close), &dctx);
    g_signal_connect(entry, "activate", G_CALLBACK(dlg_ctx_entry_ok), &dctx);

    gtk_window_present(GTK_WINDOW(dlg));

    if (dlg_ctx_run(&dctx) == 1) {
        const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
        int line = atoi(txt);
        if (line > 0) {
            GtkTextIter it;
            gtk_text_buffer_get_iter_at_line(ctx->buffer, &it, line - 1);
            gtk_text_buffer_place_cursor(ctx->buffer, &it);
            gtk_text_view_scroll_to_iter(GTK_TEXT_VIEW(ctx->text_view),
                                          &it, 0.1, FALSE, 0, 0);
        }
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
}

static GtkWidget *create_editor_menubar(EditorCtx *ctx)
{
    /* Action group */
    GSimpleActionGroup *actions = g_simple_action_group_new();

    /* File actions */
    GSimpleAction *a_save = g_simple_action_new("save", NULL);
    g_signal_connect(a_save, "activate", G_CALLBACK(action_save), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_save));

    GSimpleAction *a_close = g_simple_action_new("close", NULL);
    g_signal_connect(a_close, "activate", G_CALLBACK(action_close), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_close));

    /* Edit actions */
    GSimpleAction *a_undo = g_simple_action_new("undo", NULL);
    g_signal_connect(a_undo, "activate", G_CALLBACK(action_undo), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_undo));

    GSimpleAction *a_redo = g_simple_action_new("redo", NULL);
    g_signal_connect(a_redo, "activate", G_CALLBACK(action_redo), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_redo));

    GSimpleAction *a_cut = g_simple_action_new("cut", NULL);
    g_signal_connect(a_cut, "activate", G_CALLBACK(action_cut), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_cut));

    GSimpleAction *a_copy = g_simple_action_new("copy", NULL);
    g_signal_connect(a_copy, "activate", G_CALLBACK(action_copy), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_copy));

    GSimpleAction *a_paste = g_simple_action_new("paste", NULL);
    g_signal_connect(a_paste, "activate", G_CALLBACK(action_paste), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_paste));

    GSimpleAction *a_selall = g_simple_action_new("select-all", NULL);
    g_signal_connect(a_selall, "activate", G_CALLBACK(action_select_all), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_selall));

    GSimpleAction *a_goto = g_simple_action_new("goto-line", NULL);
    g_signal_connect(a_goto, "activate", G_CALLBACK(action_goto_line), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_goto));

    /* Search actions */
    GSimpleAction *a_find = g_simple_action_new("find", NULL);
    g_signal_connect(a_find, "activate", G_CALLBACK(action_find), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_find));

    GSimpleAction *a_replace = g_simple_action_new("replace", NULL);
    g_signal_connect(a_replace, "activate", G_CALLBACK(action_replace), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_replace));

    GSimpleAction *a_find_next = g_simple_action_new("find-next", NULL);
    g_signal_connect(a_find_next, "activate", G_CALLBACK(action_find_next), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_find_next));

    GSimpleAction *a_find_prev = g_simple_action_new("find-prev", NULL);
    g_signal_connect(a_find_prev, "activate", G_CALLBACK(action_find_prev), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_find_prev));

    /* View actions (toggle) */
    GSimpleAction *a_wrap = g_simple_action_new_stateful("word-wrap", NULL,
                                g_variant_new_boolean(ctx->word_wrap));
    g_signal_connect(a_wrap, "activate", G_CALLBACK(action_word_wrap), ctx);
    g_action_map_add_action(G_ACTION_MAP(actions), G_ACTION(a_wrap));

    gtk_widget_insert_action_group(ctx->window, "editor", G_ACTION_GROUP(actions));

    /* Build menu model */
    GMenu *menubar = g_menu_new();

    /* File */
    GMenu *file_menu = g_menu_new();
    GMenuItem *mi;
    mi = g_menu_item_new("Save", "editor.save");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>S");
    g_menu_append_item(file_menu, mi); g_object_unref(mi);
    g_menu_append(file_menu, "Close", "editor.close");
    g_menu_append_submenu(menubar, "File", G_MENU_MODEL(file_menu));

    /* Edit */
    GMenu *edit_menu = g_menu_new();
    mi = g_menu_item_new("Undo", "editor.undo");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>Z");
    g_menu_append_item(edit_menu, mi); g_object_unref(mi);
    mi = g_menu_item_new("Redo", "editor.redo");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl><Shift>Z");
    g_menu_append_item(edit_menu, mi); g_object_unref(mi);

    GMenu *edit_section2 = g_menu_new();
    mi = g_menu_item_new("Cut", "editor.cut");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>X");
    g_menu_append_item(edit_section2, mi); g_object_unref(mi);
    mi = g_menu_item_new("Copy", "editor.copy");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>C");
    g_menu_append_item(edit_section2, mi); g_object_unref(mi);
    mi = g_menu_item_new("Paste", "editor.paste");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>V");
    g_menu_append_item(edit_section2, mi); g_object_unref(mi);
    g_menu_append_section(edit_menu, NULL, G_MENU_MODEL(edit_section2));

    GMenu *edit_section3 = g_menu_new();
    mi = g_menu_item_new("Select all", "editor.select-all");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>A");
    g_menu_append_item(edit_section3, mi); g_object_unref(mi);
    mi = g_menu_item_new("Go to line...", "editor.goto-line");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>G");
    g_menu_append_item(edit_section3, mi); g_object_unref(mi);
    g_menu_append_section(edit_menu, NULL, G_MENU_MODEL(edit_section3));

    g_menu_append_submenu(menubar, "Edit", G_MENU_MODEL(edit_menu));

    /* Find */
    GMenu *search_menu = g_menu_new();
    mi = g_menu_item_new("Find...", "editor.find");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>F");
    g_menu_append_item(search_menu, mi); g_object_unref(mi);
    mi = g_menu_item_new("Replace...", "editor.replace");
    g_menu_item_set_attribute(mi, "accel", "s", "<Ctrl>H");
    g_menu_append_item(search_menu, mi); g_object_unref(mi);

    GMenu *search_section2 = g_menu_new();
    mi = g_menu_item_new("Find next", "editor.find-next");
    g_menu_item_set_attribute(mi, "accel", "s", "F3");
    g_menu_append_item(search_section2, mi); g_object_unref(mi);
    mi = g_menu_item_new("Find previous", "editor.find-prev");
    g_menu_item_set_attribute(mi, "accel", "s", "<Shift>F3");
    g_menu_append_item(search_section2, mi); g_object_unref(mi);
    g_menu_append_section(search_menu, NULL, G_MENU_MODEL(search_section2));

    g_menu_append_submenu(menubar, "Find", G_MENU_MODEL(search_menu));

    /* View */
    GMenu *view_menu = g_menu_new();
    g_menu_append(view_menu, "Word wrap", "editor.word-wrap");
    g_menu_append_submenu(menubar, "View", G_MENU_MODEL(view_menu));

    GtkWidget *menubar_widget = gtk_popover_menu_bar_new_from_model(G_MENU_MODEL(menubar));
    return menubar_widget;
}

/* ── keyboard ────────────────────────────────────────────────────── */

static gboolean on_editor_key(GtkEventControllerKey *kc,
                               guint keyval, guint keycode,
                               GdkModifierType state, gpointer user_data)
{
    (void)kc; (void)keycode;
    EditorCtx *ctx = user_data;
    gboolean ctrl = (state & GDK_CONTROL_MASK) != 0;
    gboolean shift = (state & GDK_SHIFT_MASK) != 0;

    if (keyval == GDK_KEY_Escape) {
        if (gtk_widget_get_visible(ctx->search_bar)) {
            search_clear_highlights(ctx);
            gtk_widget_set_visible(ctx->search_bar, FALSE);
            gtk_widget_grab_focus(ctx->text_view);
            return TRUE;
        }
        gtk_window_close(GTK_WINDOW(ctx->window));
        return TRUE;
    }

    /* Ctrl+S handled by menu action, but keeping as backup */
    if (ctrl && keyval == GDK_KEY_s) {
        editor_save(ctx);
        return TRUE;
    }

    /* Search shortcuts */
    if (ctrl && keyval == GDK_KEY_f) {
        show_search_bar(ctx, FALSE);
        return TRUE;
    }
    if (ctrl && keyval == GDK_KEY_h) {
        show_search_bar(ctx, TRUE);
        return TRUE;
    }
    if (keyval == GDK_KEY_F3) {
        if (shift)
            search_prev(ctx);
        else
            search_next(ctx);
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
    if (ctx->match_lines) g_array_free(ctx->match_lines, TRUE);
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
        filepath = panel_cursor_fullpath(p);
        g_free(name);

        struct stat st;
        if (stat(filepath, &st) != 0 || S_ISDIR(st.st_mode)) {
            g_free(filepath);
            fm_status(fm, "Editor: not a file");
            return;
        }

        /* Check file size before loading */
        if (st.st_size > EDITOR_MAX_BYTES) {
            fm_status(fm, "Editor: file is too large (%.1f MB, max %d MB)",
                      st.st_size / (1024.0 * 1024.0), EDITOR_MAX_BYTES / (1024 * 1024));
            g_free(filepath);
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

    /* Only convert if invalid UTF-8 (avoid unnecessary copy) */
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
    ctx->word_wrap   = FALSE;

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

    /* menu bar */
    GtkWidget *menubar = create_editor_menubar(ctx);
    gtk_box_append(GTK_BOX(vbox), menubar);

    /* search/replace bar (initially hidden) */
    ctx->search_bar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(ctx->search_bar, 6);
    gtk_widget_set_margin_end(ctx->search_bar, 6);
    gtk_widget_set_margin_top(ctx->search_bar, 4);
    gtk_widget_set_margin_bottom(ctx->search_bar, 4);
    gtk_widget_set_visible(ctx->search_bar, FALSE);

    /* Search row */
    GtkWidget *search_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *search_lbl = gtk_label_new("Search:");
    ctx->search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(ctx->search_entry, TRUE);
    GtkWidget *prev_btn = gtk_button_new_from_icon_name("go-up-symbolic");
    gtk_widget_set_tooltip_text(prev_btn, "Previous (Shift+F3)");
    GtkWidget *next_btn = gtk_button_new_from_icon_name("go-down-symbolic");
    gtk_widget_set_tooltip_text(next_btn, "Next (F3)");
    ctx->match_lbl = gtk_label_new("");
    gtk_widget_set_size_request(ctx->match_lbl, 80, -1);
    GtkWidget *close_search_btn = gtk_button_new_from_icon_name("window-close-symbolic");
    gtk_widget_set_tooltip_text(close_search_btn, "Close (Esc)");

    gtk_box_append(GTK_BOX(search_row), search_lbl);
    gtk_box_append(GTK_BOX(search_row), ctx->search_entry);
    gtk_box_append(GTK_BOX(search_row), prev_btn);
    gtk_box_append(GTK_BOX(search_row), next_btn);
    gtk_box_append(GTK_BOX(search_row), ctx->match_lbl);
    gtk_box_append(GTK_BOX(search_row), close_search_btn);
    gtk_box_append(GTK_BOX(ctx->search_bar), search_row);

    /* Replace row */
    ctx->replace_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *replace_lbl = gtk_label_new("Replace:");
    ctx->replace_entry = gtk_entry_new();
    gtk_widget_set_hexpand(ctx->replace_entry, TRUE);
    GtkWidget *replace_btn = gtk_button_new_with_label("Replace");
    GtkWidget *replace_all_btn = gtk_button_new_with_label("Replace all");

    gtk_box_append(GTK_BOX(ctx->replace_row), replace_lbl);
    gtk_box_append(GTK_BOX(ctx->replace_row), ctx->replace_entry);
    gtk_box_append(GTK_BOX(ctx->replace_row), replace_btn);
    gtk_box_append(GTK_BOX(ctx->replace_row), replace_all_btn);
    gtk_box_append(GTK_BOX(ctx->search_bar), ctx->replace_row);

    gtk_box_append(GTK_BOX(vbox), ctx->search_bar);

    /* Search bar signals */
    g_signal_connect(ctx->search_entry, "changed", G_CALLBACK(on_search_changed), ctx);
    g_signal_connect(next_btn, "clicked", G_CALLBACK(on_search_next_clicked), ctx);
    g_signal_connect(prev_btn, "clicked", G_CALLBACK(on_search_prev_clicked), ctx);
    g_signal_connect(close_search_btn, "clicked", G_CALLBACK(on_search_close_clicked), ctx);
    g_signal_connect(replace_btn, "clicked", G_CALLBACK(on_replace_clicked), ctx);
    g_signal_connect(replace_all_btn, "clicked", G_CALLBACK(on_replace_all_clicked), ctx);

    GtkEventController *search_kc = gtk_event_controller_key_new();
    g_signal_connect(search_kc, "key-pressed", G_CALLBACK(on_search_key), ctx);
    gtk_widget_add_controller(ctx->search_entry, search_kc);

    /* main area: [gutter] + scrolled textview + match_map */
    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_vexpand(hbox, TRUE);
    gtk_box_append(GTK_BOX(vbox), hbox);

#ifdef HAVE_GTKSOURCEVIEW
    GtkSourceBuffer *_sbuf = gtk_source_buffer_new(NULL);
    {
        const char *sid = fm_scheme_for_theme(fm);
        GtkSourceStyleSchemeManager *_sm =
            gtk_source_style_scheme_manager_get_default();
        GtkSourceStyleScheme *_sc =
            gtk_source_style_scheme_manager_get_scheme(_sm, sid);
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
    ctx->scroll = scroll;

    /* Match map (scrollbar indicator for search matches) */
    ctx->match_map = gtk_drawing_area_new();
    gtk_widget_set_size_request(ctx->match_map, 10, -1);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(ctx->match_map),
                                   match_map_draw, ctx, NULL);
    gtk_box_append(GTK_BOX(hbox), ctx->match_map);

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

    /* Initialize search structures */
    ctx->match_lines = g_array_new(FALSE, FALSE, sizeof(int));
    ctx->match_tag = gtk_text_buffer_create_tag(ctx->buffer, "search-match",
                        "background", "#FFFF00",
                        "foreground", "#000000",
                        NULL);

    /* Count lines for match map */
    ctx->total_lines = 1;
    for (const gchar *q = content; *q; q++)
        if (*q == '\n') ctx->total_lines++;

    /* load content – suppress modified flag */
    g_signal_handlers_block_by_func(ctx->buffer, on_buffer_changed, ctx);
    gtk_text_buffer_set_text(ctx->buffer, content, -1);
    g_free(content);
    g_signal_handlers_unblock_by_func(ctx->buffer, on_buffer_changed, ctx);
#ifdef HAVE_GTKSOURCEVIEW
    {
        GtkSourceLanguage *_lang = source_guess_language(title);
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

#ifndef HAVE_GTKSOURCEVIEW
    /* Only needed for fallback highlighter - GtkSourceView handles its own */
    highlight_apply(ctx->buffer, title, fm->syntax_highlight);
#endif

    gtk_window_present(GTK_WINDOW(ctx->window));
}
