#include "dialogs.h"
#include "util.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Multi-rename (Ctrl+M): marked files renamed by a mask with a live
 *  preview.  Local panels only; names are computed by mrn_apply().
 * ─────────────────────────────────────────────────────────────────── */

#define MRN_PREVIEW_MAX 500

typedef struct {
    gchar   *path;          /* full old path   */
    gchar   *dir;
    gchar   *old_name;
    gchar   *new_name;      /* current preview */
    gboolean is_dir;
} MrnItem;

typedef struct {
    GPtrArray *items;       /* MrnItem*        */
    GtkWidget *mask, *find, *repl, *start, *step, *digits, *casedd;
    GtkWidget *scroll, *status, *ok_btn;
} MrnDlg;

static void mrn_item_free(gpointer d)
{
    MrnItem *it = d;
    g_free(it->path); g_free(it->dir); g_free(it->old_name); g_free(it->new_name);
    g_free(it);
}

/* Recompute all new names; returns an error text or NULL when renaming is possible */
static const char *mrn_compute(MrnDlg *d)
{
    const char *mask = gtk_editable_get_text(GTK_EDITABLE(d->mask));
    const char *find = gtk_editable_get_text(GTK_EDITABLE(d->find));
    const char *repl = gtk_editable_get_text(GTK_EDITABLE(d->repl));
    guint start  = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->start));
    guint step   = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->step));
    guint digits = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(d->digits));
    MrnCase cs   = (MrnCase)gtk_drop_down_get_selected(GTK_DROP_DOWN(d->casedd));

    const char *err = NULL;
    gboolean changed = FALSE;
    GHashTable *olds = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *news = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < d->items->len; i++)
        g_hash_table_add(olds, ((MrnItem *)d->items->pdata[i])->path);

    for (guint i = 0; i < d->items->len; i++) {
        MrnItem *it = d->items->pdata[i];
        g_free(it->new_name);
        it->new_name = mrn_apply(it->old_name, it->is_dir, mask, find, repl, cs,
                                 start + i * step, digits);
        if (err) continue;
        if (!fs_name_is_safe(it->new_name)) { err = "Invalid name (empty, \"..\" or contains \"/\")"; continue; }
        if (strcmp(it->new_name, it->old_name) != 0) changed = TRUE;
        gchar *np = g_build_filename(it->dir, it->new_name, NULL);
        struct stat st;
        if (!g_hash_table_add(news, np))
            err = "Two items would get the same name";
        else if (!g_hash_table_contains(olds, np) && lstat(np, &st) == 0) {
            /* case-insensitive FS: "A.TXT" may be the item itself */
            struct stat own;
            if (!(lstat(it->path, &own) == 0 && own.st_dev == st.st_dev && own.st_ino == st.st_ino))
                err = "A file with a new name already exists";
        }
    }
    g_hash_table_destroy(olds);
    g_hash_table_destroy(news);
    return err ? err : changed ? NULL : "No name changes";
}

static void mrn_update(MrnDlg *d)
{
    const char *err = mrn_compute(d);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
    for (guint i = 0; i < d->items->len && i < MRN_PREVIEW_MAX; i++) {
        MrnItem *it = d->items->pdata[i];
        GtkWidget *o = gtk_label_new(it->old_name);
        GtkWidget *a = gtk_label_new("→");
        GtkWidget *n = gtk_label_new(it->new_name);
        gtk_widget_set_halign(o, GTK_ALIGN_START);
        gtk_widget_set_halign(n, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(o), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_hexpand(o, TRUE);
        gtk_widget_set_hexpand(n, TRUE);
        gtk_widget_add_css_class(o, "dim-label");
        gtk_grid_attach(GTK_GRID(grid), o, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), a, 1, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), n, 2, (int)i, 1, 1);
    }
    if (d->items->len > MRN_PREVIEW_MAX) {
        gchar *more = g_strdup_printf("… and %u more", d->items->len - MRN_PREVIEW_MAX);
        GtkWidget *l = gtk_label_new(more);
        g_free(more);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_grid_attach(GTK_GRID(grid), l, 0, MRN_PREVIEW_MAX, 3, 1);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(d->scroll), grid);

    gtk_label_set_text(GTK_LABEL(d->status), err ? err : "");
    gtk_widget_set_sensitive(d->ok_btn, err == NULL);
}

static void on_mrn_changed(GtkWidget *w, MrnDlg *d) { (void)w; mrn_update(d); }

static void on_mrn_activate(GtkEntry *e, DlgCtx *ctx)
{
    MrnDlg *d = g_object_get_data(G_OBJECT(e), "mrn");
    if (gtk_widget_get_sensitive(d->ok_btn)) dlg_ctx_entry_ok(e, ctx);
}

static gboolean on_mrn_key(GtkEventControllerKey *k, guint keyval, guint code,
                           GdkModifierType st, DlgCtx *ctx)
{
    (void)k; (void)code; (void)st;
    if (keyval != GDK_KEY_Escape) return FALSE;
    dlg_ctx_cancel(NULL, ctx);
    return TRUE;
}

static GtkWidget *mrn_spin(int min, int max, int val)
{
    GtkWidget *s = gtk_spin_button_new_with_range(min, max, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(s), val);
    return s;
}

static void mrn_row(GtkWidget *grid, int row, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new(label);
    gtk_widget_set_halign(l, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), l, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), w, 1, row, 1, 1);
}

/* rename() that never replaces an existing file */
static int rename_noreplace(const char *from, const char *to)
{
#ifdef RENAME_NOREPLACE
    if (renameat2(AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE) == 0) return 0;
    if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
    struct stat st;
    if (lstat(to, &st) == 0) { errno = EEXIST; return -1; }
    return rename(from, to);
}

/* Two phases (old → temp → new) so that swaps and chains (a→b, b→c)
 * work.  Returns the number of renamed items; *err gets the first error. */
static guint mrn_execute(GPtrArray *items, gchar **err)
{
    guint n = items->len, done = 0;
    gchar **tmp = g_new0(gchar *, n);
    guint32 tag = g_random_int();

    for (guint i = 0; i < n; i++) {
        MrnItem *it = items->pdata[i];
        if (strcmp(it->old_name, it->new_name) == 0) continue;
        gchar *t = g_strdup_printf(".fm-rename-%08x-%u", tag, i);
        tmp[i] = g_build_filename(it->dir, t, NULL);
        g_free(t);
        if (rename_noreplace(it->path, tmp[i]) != 0) {
            *err = g_strdup_printf("%s: %s", it->old_name, g_strerror(errno));
            g_clear_pointer(&tmp[i], g_free);
            /* undo phase 1 */
            for (guint j = 0; j < i; j++)
                if (tmp[j]) { rename(tmp[j], ((MrnItem *)items->pdata[j])->path); g_clear_pointer(&tmp[j], g_free); }
            g_free(tmp);
            return 0;
        }
    }
    for (guint i = 0; i < n; i++) {
        if (!tmp[i]) continue;
        MrnItem *it = items->pdata[i];
        gchar *np = g_build_filename(it->dir, it->new_name, NULL);
        if (rename_noreplace(tmp[i], np) == 0) {
            done++;
        } else {
            gchar *e = g_strdup_printf("%s: %s", it->new_name, g_strerror(errno));
            if (rename_noreplace(tmp[i], it->path) != 0) {
                /* the worst case wins: tell where the file is now */
                g_free(e);
                e = g_strdup_printf("%s left as %s", it->old_name, tmp[i]);
                g_free(*err);
                *err = e;
            } else if (!*err) {
                *err = e;
            } else {
                g_free(e);
            }
        }
        g_free(np);
        g_free(tmp[i]);
    }
    g_free(tmp);
    return done;
}

void fo_multi_rename(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    if (p->ssh_conn) {
        fm_status(fm, "Multi-rename works on local files only");
        return;
    }
    GList *paths = panel_selection_paths(p);
    if (!paths) {
        fm_status(fm, "No file is selected");
        return;
    }

    MrnDlg d = { .items = g_ptr_array_new_with_free_func(mrn_item_free) };
    for (GList *l = paths; l; l = l->next) {
        MrnItem *it = g_new0(MrnItem, 1);
        struct stat st;
        it->path     = g_strdup(l->data);
        it->dir      = g_path_get_dirname(l->data);
        it->old_name = g_path_get_basename(l->data);
        it->is_dir   = lstat(it->path, &st) == 0 && S_ISDIR(st.st_mode);
        g_ptr_array_add(d.items, it);
    }
    g_list_free_full(paths, g_free);

    /* a directory together with something inside it (search results):
     * renaming the directory would move the other item away */
    for (guint i = 0; i < d.items->len; i++)
        for (guint j = 0; j < d.items->len; j++) {
            if (i == j) continue;
            const MrnItem *a = d.items->pdata[i], *b = d.items->pdata[j];
            if (a->is_dir && fs_path_is_inside(b->path, a->path)) {
                fm_status(fm, "Multi-rename: '%s' is inside the selected directory '%s'",
                          b->old_name, a->old_name);
                g_ptr_array_unref(d.items);
                return;
            }
        }

    DlgCtx ctx;
    dlg_ctx_init(&ctx);
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Multi-rename");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 640, 480);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    d.mask = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d.mask), "[N].[E]");
    gtk_widget_set_hexpand(d.mask, TRUE);
    mrn_row(grid, 0, "Name mask:", d.mask);
    GtkWidget *hint = gtk_label_new("[N] name   [E] extension   [C] counter");
    gtk_widget_add_css_class(hint, "dim-label");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(grid), hint, 1, 1, 1, 1);

    d.find = gtk_entry_new();
    d.repl = gtk_entry_new();
    mrn_row(grid, 2, "Find:", d.find);
    mrn_row(grid, 3, "Replace with:", d.repl);

    GtkWidget *cnt = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    d.start  = mrn_spin(0, 999999, 1);
    d.step   = mrn_spin(1, 1000, 1);
    d.digits = mrn_spin(1, 10, 1);
    gtk_box_append(GTK_BOX(cnt), d.start);
    gtk_box_append(GTK_BOX(cnt), gtk_label_new("step"));
    gtk_box_append(GTK_BOX(cnt), d.step);
    gtk_box_append(GTK_BOX(cnt), gtk_label_new("digits"));
    gtk_box_append(GTK_BOX(cnt), d.digits);
    mrn_row(grid, 4, "Counter start:", cnt);

    static const char *const cases[] = { "Unchanged", "lowercase", "UPPERCASE", NULL };
    d.casedd = gtk_drop_down_new_from_strings(cases);
    gtk_widget_set_halign(d.casedd, GTK_ALIGN_START);
    mrn_row(grid, 5, "Case:", d.casedd);
    gtk_box_append(GTK_BOX(vbox), grid);

    d.scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(d.scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(d.scroll, TRUE);
    gtk_box_append(GTK_BOX(vbox), d.scroll);

    d.status = gtk_label_new("");
    gtk_widget_set_halign(d.status, GTK_ALIGN_START);
    gtk_widget_add_css_class(d.status, "error");
    gtk_box_append(GTK_BOX(vbox), d.status);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(btn_box), TRUE);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    d.ok_btn = gtk_button_new_with_label("Rename");
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_size_request(d.ok_btn, 90, -1);
    gtk_widget_add_css_class(d.ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), d.ok_btn);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    GtkWidget *entries[] = { d.mask, d.find, d.repl };
    for (gsize i = 0; i < G_N_ELEMENTS(entries); i++) {
        g_object_set_data(G_OBJECT(entries[i]), "mrn", &d);
        g_signal_connect(entries[i], "changed",  G_CALLBACK(on_mrn_changed),  &d);
        g_signal_connect(entries[i], "activate", G_CALLBACK(on_mrn_activate), &ctx);
    }
    GtkWidget *spins[] = { d.start, d.step, d.digits };
    for (gsize i = 0; i < G_N_ELEMENTS(spins); i++)
        g_signal_connect(spins[i], "value-changed", G_CALLBACK(on_mrn_changed), &d);
    g_signal_connect(d.casedd, "notify::selected", G_CALLBACK(on_mrn_changed), &d);
    g_signal_connect(d.ok_btn,   "clicked",       G_CALLBACK(dlg_ctx_ok),     &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),  &ctx);
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_mrn_key), &ctx);
    gtk_widget_add_controller(dlg, key);

    mrn_update(&d);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(d.mask);
    gtk_editable_select_region(GTK_EDITABLE(d.mask), 0, 3);   /* "[N]" */

    int resp = dlg_ctx_run(&ctx);
    if (resp == 1 && mrn_compute(&d) == NULL) {   /* recheck: files may have changed */
        gchar *err = NULL;
        guint done = mrn_execute(d.items, &err);
        if (err) fm_status(fm, "Renamed %u, error: %s", done, err);
        else     fm_status(fm, "Renamed %u item%s", done, done == 1 ? "" : "s");
        g_free(err);
        if (p->marks) g_hash_table_remove_all(p->marks);
        panel_reload(&fm->panels[0]);
        panel_reload(&fm->panels[1]);
    } else if (resp == 1) {
        fm_status(fm, "Multi-rename: %s", mrn_compute(&d));
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    g_ptr_array_unref(d.items);
    panel_focus(p);
}
