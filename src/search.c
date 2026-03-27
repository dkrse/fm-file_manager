#include "fm.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fnmatch.h>

/* No hard result limit – search continues until the whole tree is walked */

/* ── SearchItem GObject (file-local) ─────────────────────────────── */

#define SEARCH_ITEM_TYPE (search_item_get_type())
G_DECLARE_FINAL_TYPE(SearchItem, search_item, SEARCH, ITEM, GObject)

struct _SearchItem {
    GObject   parent_instance;
    gchar    *dir_path;
    gchar    *name;
    gchar    *size;
    gchar    *date;
    gboolean  is_dir;
    gchar    *icon_name;
};

G_DEFINE_TYPE(SearchItem, search_item, G_TYPE_OBJECT)

static void search_item_finalize(GObject *obj)
{
    SearchItem *s = SEARCH_ITEM(obj);
    g_free(s->dir_path);
    g_free(s->name);
    g_free(s->size);
    g_free(s->date);
    g_free(s->icon_name);
    G_OBJECT_CLASS(search_item_parent_class)->finalize(obj);
}

static void search_item_class_init(SearchItemClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = search_item_finalize;
}

static void search_item_init(SearchItem *self) { (void)self; }

static SearchItem *search_item_new(const char *dir, const char *name,
                                    const char *size, const char *date,
                                    gboolean is_dir, const char *icon)
{
    SearchItem *s = g_object_new(SEARCH_ITEM_TYPE, NULL);
    s->dir_path  = g_strdup(dir);
    s->name      = g_strdup(name);
    s->size      = g_strdup(size);
    s->date      = g_strdup(date);
    s->is_dir    = is_dir;
    s->icon_name = g_strdup(icon);
    return s;
}

/* ── Recursive search ─────────────────────────────────────────────── */

typedef struct {
    GListStore  *store;
    const char  *pattern;
    int          count;
    GtkWidget   *counter_label;
} SearchCtx;

static void search_dir(SearchCtx *ctx, const char *base, int depth)
{
    if (depth > 64) return;

    DIR *dir = opendir(base);
    if (!dir) return;

    struct dirent *de;
    struct stat st;
    char full[4096];

    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;

        g_snprintf(full, sizeof(full), "%s/%s", base, de->d_name);
        if (lstat(full, &st) != 0) continue;

        if (fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0) {
            gboolean is_dir = S_ISDIR(st.st_mode);
            gchar *sz = is_dir ? g_strdup("<DIR>") : fmt_size(st.st_size);
            gchar *dt = fmt_date(st.st_mtime);
            gboolean is_link = S_ISLNK(st.st_mode);
            gboolean is_exec = !is_dir && (st.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH));
            const char *icon = icon_for_entry(de->d_name, is_dir, is_link, is_exec);

            SearchItem *item = search_item_new(base, de->d_name, sz, dt, is_dir, icon);
            g_list_store_append(ctx->store, item);
            g_object_unref(item);
            g_free(sz); g_free(dt);
            ctx->count++;

            if (ctx->count % 20 == 0) {
                gchar *msg = g_strdup_printf("Found: %d", ctx->count);
                gtk_label_set_text(GTK_LABEL(ctx->counter_label), msg);
                g_free(msg);
                while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
            }
        }

        if (S_ISDIR(st.st_mode))
            search_dir(ctx, full, depth + 1);
    }
    closedir(dir);
}

/* ── Column factories for results ─────────────────────────────────── */

static void res_name_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *image = gtk_image_new();
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(li, box);
}

static void res_name_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *box   = gtk_list_item_get_child(li);
    GtkWidget *image = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(image);
    SearchItem *item = gtk_list_item_get_item(li);
    gtk_image_set_from_icon_name(GTK_IMAGE(image), item->icon_name);
    gtk_label_set_text(GTK_LABEL(label), item->name);
}

static void res_text_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_item_set_child(li, label);
}

static void res_path_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    SearchItem *item = gtk_list_item_get_item(li);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), item->dir_path);
}

static void res_size_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    SearchItem *item = gtk_list_item_get_item(li);
    gtk_label_set_text(GTK_LABEL(gtk_list_item_get_child(li)), item->size);
}

/* ── Result row activated → navigate ─────────────────────────────── */

typedef struct {
    FM         *fm;
    GListStore *store;
    GtkWidget  *dialog;
    GMainLoop  *loop;
} SearchDlgData;

static void on_result_activated(GtkColumnView *cv, guint pos, SearchDlgData *d)
{
    (void)cv;
    SearchItem *item = g_list_model_get_item(G_LIST_MODEL(d->store), pos);
    if (!item) return;

    Panel *p = active_panel(d->fm);
    panel_load(p, item->dir_path);

    /* position cursor on found file */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *fi = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (fi && strcmp(fi->name, item->name) == 0) {
            g_object_unref(fi);
            p->cursor_pos = i;
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                      NULL, GTK_LIST_SCROLL_FOCUS, NULL);
            break;
        }
        if (fi) g_object_unref(fi);
    }

    g_object_unref(item);
    g_main_loop_quit(d->loop);
}

/* ── Main search dialog ──────────────────────────────────────────── */

void search_run(FM *fm)
{
    if (active_panel(fm)->ssh_conn) {
        fm_status(fm, "Search on remote panel is not supported");
        return;
    }

    /* ── input dialog ── */
    DlgCtx ictx;
    dlg_ctx_init(&ictx);

    GtkWidget *input_dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(input_dlg), "Search files");
    gtk_window_set_modal(GTK_WINDOW(input_dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(input_dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(input_dlg), 420, -1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);

    GtkWidget *lbl1 = gtk_label_new("Pattern (glob):");
    gtk_widget_set_halign(lbl1, GTK_ALIGN_END);
    GtkWidget *lbl2 = gtk_label_new("Search in:");
    gtk_widget_set_halign(lbl2, GTK_ALIGN_END);

    GtkWidget *pat_entry = gtk_entry_new();
    GtkWidget *dir_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(dir_entry), active_panel(fm)->cwd);
    gtk_widget_set_hexpand(pat_entry, TRUE);
    gtk_widget_set_hexpand(dir_entry, TRUE);

    GtkWidget *browse = gtk_button_new_with_label("...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), dir_entry);

    GtkWidget *dir_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(dir_box, TRUE);
    gtk_box_append(GTK_BOX(dir_box), dir_entry);
    gtk_box_append(GTK_BOX(dir_box), browse);

    /* Buttons */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *search_btn = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(search_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), search_btn);

    gtk_grid_attach(GTK_GRID(grid), lbl1,      0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pat_entry,  1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), lbl2,      0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dir_box,   1, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), btn_box,   0, 2, 2, 1);

    gtk_window_set_child(GTK_WINDOW(input_dlg), grid);

    g_signal_connect(search_btn,  "clicked",       G_CALLBACK(dlg_ctx_ok),       &ictx);
    g_signal_connect(cancel_btn,  "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ictx);
    g_signal_connect(pat_entry,   "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ictx);
    g_signal_connect(input_dlg,   "close-request", G_CALLBACK(dlg_ctx_close),    &ictx);

    gtk_window_present(GTK_WINDOW(input_dlg));

    if (dlg_ctx_run(&ictx) != 1) {
        gtk_window_destroy(GTK_WINDOW(input_dlg));
        return;
    }

    const gchar *raw_pat = gtk_editable_get_text(GTK_EDITABLE(pat_entry));
    const gchar *dir_val = gtk_editable_get_text(GTK_EDITABLE(dir_entry));

    gchar pattern[512];
    if (raw_pat[0] && !strchr(raw_pat, '*') && !strchr(raw_pat, '?'))
        g_snprintf(pattern, sizeof(pattern), "*%s*", raw_pat);
    else if (raw_pat[0])
        g_strlcpy(pattern, raw_pat, sizeof(pattern));
    else
        g_strlcpy(pattern, "*", sizeof(pattern));

    gchar *search_dir_path = g_strdup(dir_val[0] ? dir_val : active_panel(fm)->cwd);
    gtk_window_destroy(GTK_WINDOW(input_dlg));

    /* ── result dialog ── */
    GMainLoop *res_loop = g_main_loop_new(NULL, FALSE);

    GtkWidget *res_dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(res_dlg), "Search results");
    gtk_window_set_transient_for(GTK_WINDOW(res_dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(res_dlg), 800, 520);

    GtkWidget *rc = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_margin_start(rc, 8);
    gtk_widget_set_margin_end(rc, 8);
    gtk_widget_set_margin_top(rc, 8);
    gtk_widget_set_margin_bottom(rc, 8);

    /* info bar */
    GtkWidget *info_box  = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gchar *info_text = g_strdup_printf("Searching for <b>%s</b> in %s", pattern, search_dir_path);
    GtkWidget *info_lbl  = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(info_lbl), info_text);
    g_free(info_text);
    gtk_widget_set_halign(info_lbl, GTK_ALIGN_START);
    gtk_widget_set_hexpand(info_lbl, TRUE);
    GtkWidget *count_lbl = gtk_label_new("Searching...");
    gtk_box_append(GTK_BOX(info_box), info_lbl);
    gtk_box_append(GTK_BOX(info_box), count_lbl);

    /* result store and column view */
    GListStore *res_store = g_list_store_new(SEARCH_ITEM_TYPE);
    GtkNoSelection *res_sel = gtk_no_selection_new(g_object_ref(G_LIST_MODEL(res_store)));

    GtkWidget *res_cv = gtk_column_view_new(GTK_SELECTION_MODEL(res_sel));
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(res_cv), TRUE);

    /* Name column */
    GtkListItemFactory *nf = gtk_signal_list_item_factory_new();
    g_signal_connect(nf, "setup", G_CALLBACK(res_name_setup), NULL);
    g_signal_connect(nf, "bind",  G_CALLBACK(res_name_bind),  NULL);
    GtkColumnViewColumn *cn = gtk_column_view_column_new("Name", nf);
    gtk_column_view_column_set_expand(cn, TRUE);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(res_cv), cn);
    g_object_unref(cn);

    /* Path column */
    GtkListItemFactory *pf = gtk_signal_list_item_factory_new();
    g_signal_connect(pf, "setup", G_CALLBACK(res_text_setup), NULL);
    g_signal_connect(pf, "bind",  G_CALLBACK(res_path_bind),  NULL);
    GtkColumnViewColumn *cp = gtk_column_view_column_new("Directory", pf);
    gtk_column_view_column_set_resizable(cp, TRUE);
    gtk_column_view_column_set_fixed_width(cp, 220);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(res_cv), cp);
    g_object_unref(cp);

    /* Size column */
    GtkListItemFactory *sf = gtk_signal_list_item_factory_new();
    g_signal_connect(sf, "setup", G_CALLBACK(res_text_setup), NULL);
    g_signal_connect(sf, "bind",  G_CALLBACK(res_size_bind),  NULL);
    GtkColumnViewColumn *cs = gtk_column_view_column_new("Size", sf);
    gtk_column_view_column_set_fixed_width(cs, 80);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(res_cv), cs);
    g_object_unref(cs);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), res_cv);
    gtk_widget_set_vexpand(scroll, TRUE);

    /* Close button */
    GtkWidget *close_btn = gtk_button_new_with_label("Close");
    gtk_widget_set_halign(close_btn, GTK_ALIGN_END);
    gtk_widget_set_margin_top(close_btn, 4);

    gtk_box_append(GTK_BOX(rc), info_box);
    gtk_box_append(GTK_BOX(rc), scroll);
    gtk_box_append(GTK_BOX(rc), close_btn);
    gtk_window_set_child(GTK_WINDOW(res_dlg), rc);

    /* double-click → navigate */
    SearchDlgData dlg_data = { fm, res_store, res_dlg, res_loop };
    g_signal_connect(res_cv, "activate", G_CALLBACK(on_result_activated), &dlg_data);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(g_main_loop_quit), res_loop);
    g_signal_connect_swapped(res_dlg, "close-request",
        G_CALLBACK(g_main_loop_quit), res_loop);

    gtk_window_present(GTK_WINDOW(res_dlg));

    /* ── run search ── */
    SearchCtx ctx = {
        .store         = res_store,
        .pattern       = pattern,
        .count         = 0,
        .counter_label = count_lbl,
    };
    search_dir(&ctx, search_dir_path, 0);
    g_free(search_dir_path);

    gchar *done_msg = g_strdup_printf("Found: %d results", ctx.count);
    gtk_label_set_text(GTK_LABEL(count_lbl), done_msg);
    g_free(done_msg);

    g_main_loop_run(res_loop);
    g_main_loop_unref(res_loop);
    gtk_window_destroy(GTK_WINDOW(res_dlg));
    g_object_unref(res_store);
}
