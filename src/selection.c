#include "fm.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Marking (TC-style): Space/Insert toggle, Shift+arrows, Num+ / Num-
 *  select / unselect group by pattern, Num* invert, Ctrl+A all.
 *
 *  Marks live in Panel.marks (keys, see sel_mark_key) and FileItem.marked
 *  (rendering).
 *  Only visible items (after the file mask) are affected.
 * ─────────────────────────────────────────────────────────────────── */

static void ensure_marks(Panel *p)
{
    if (!p->marks)
        p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

/* Search results may repeat a name (a/Makefile, b/Makefile): key them
 * by full path; a directory listing keys by bare name. */
gchar *sel_mark_key(Panel *p, FileItem *it)
{
    if (p->search_mode && it->dir_path)
        return g_build_filename(it->dir_path, it->name, NULL);
    return g_strdup(it->name);
}

static void set_mark(Panel *p, FileItem *it, gboolean mark)
{
    file_item_set_marked(it, mark);
    gchar *key = sel_mark_key(p, it);
    if (mark) g_hash_table_add(p->marks, key);          /* takes key */
    else    { g_hash_table_remove(p->marks, key); g_free(key); }
}

/* Rows re-render themselves (mark-changed); only keep the cursor and
 * update the status.  No items-changed – that would re-sort the panel. */
void sel_refresh(Panel *p)
{
    guint cur = p->cursor_pos;

    p->inhibit_sel = TRUE;
    guint vis = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (cur < vis) {
        gboolean active = (p->fm->active == p->idx);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), cur, NULL,
            active ? GTK_LIST_SCROLL_SELECT : GTK_LIST_SCROLL_NONE, NULL);
    }
    p->inhibit_sel = FALSE;
    panel_update_status(p);
}

void sel_toggle_cursor(Panel *p, gboolean advance)
{
    ensure_marks(p);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (p->cursor_pos >= n) return;

    FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                           p->cursor_pos);
    if (item && strcmp(item->name, "..") != 0) {
        set_mark(p, item, !item->marked);   /* row re-renders via mark-changed */
    }
    if (item) g_object_unref(item);

    if (advance && p->cursor_pos < n - 1) p->cursor_pos++;
    p->inhibit_sel = TRUE;
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
        p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
    p->inhibit_sel = FALSE;
    panel_update_status(p);
}

/* Apply fn to every visible item except ".." */
static void for_visible(Panel *p, void (*fn)(Panel *, FileItem *, gpointer), gpointer d)
{
    ensure_marks(p);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (!it) continue;
        if (strcmp(it->name, "..") != 0) fn(p, it, d);
        g_object_unref(it);
    }
}

static void do_mark(Panel *p, FileItem *it, gpointer d)
{
    set_mark(p, it, GPOINTER_TO_INT(d));
}

static void do_invert(Panel *p, FileItem *it, gpointer d)
{
    (void)d;
    set_mark(p, it, !it->marked);
}

void sel_mark_all(Panel *p, gboolean mark)
{
    for_visible(p, do_mark, GINT_TO_POINTER(mark));
    if (!mark) g_hash_table_remove_all(p->marks);
    sel_refresh(p);
}

void sel_invert(Panel *p)
{
    for_visible(p, do_invert, NULL);
    sel_refresh(p);
}

/* ── group select / unselect ─────────────────────────────────────── */

typedef struct {
    const char *patterns;
    gboolean    mark;
    gboolean    dirs;
    int         count;
} GroupOp;

static void do_group(Panel *p, FileItem *it, gpointer d)
{
    GroupOp *g = d;
    if (it->is_dir && !g->dirs) return;
    if (!fs_match_patterns(g->patterns, it->name)) return;
    if (it->marked != g->mark) g->count++;
    set_mark(p, it, g->mark);
}

void sel_group_dialog(FM *fm, gboolean mark)
{
    static gchar   *last_pattern = NULL;
    static gboolean last_dirs    = FALSE;
    Panel *p = active_panel(fm);

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), mark ? "Select group" : "Unselect group");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 360, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *lbl = gtk_label_new("Pattern (e.g. *.c;*.h):");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), last_pattern ? last_pattern : "*.*");
    GtkWidget *dirs = gtk_check_button_new_with_label("Include directories");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dirs), last_dirs);

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bb, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *ok     = gtk_button_new_with_label(mark ? "Select" : "Unselect");
    gtk_widget_add_css_class(ok, "suggested-action");
    gtk_box_append(GTK_BOX(bb), cancel);
    gtk_box_append(GTK_BOX(bb), ok);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), dirs);
    gtk_box_append(GTK_BOX(vbox), bb);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,  "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,    "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(entry);

    if (dlg_ctx_run(&ctx) == 1) {
        g_free(last_pattern);
        last_pattern = g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry)));
        last_dirs = gtk_check_button_get_active(GTK_CHECK_BUTTON(dirs));

        GroupOp g = { last_pattern, mark, last_dirs, 0 };
        for_visible(p, do_group, &g);
        sel_refresh(p);
        fm_status(fm, "%s %d item%s", mark ? "Selected" : "Unselected",
                  g.count, g.count == 1 ? "" : "s");
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    p->inhibit_sel = TRUE;
    gtk_widget_grab_focus(p->column_view);
}
