/* Compare directories (Alt+C) with optional synchronization, and
 * compare two files (Alt+D) with an external tool or a built-in diff. */
#include "fm.h"
#include "dialogs.h"
#include <string.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Directory compare – marks what differs, like Total Commander's
 *  "Compare directories": files missing on the other side or newer
 *  than their namesake.  Directories are not compared (recursion would
 *  mean listing whole trees over SFTP).
 * ─────────────────────────────────────────────────────────────────── */

#define CMP_TIME_TOLERANCE 2   /* seconds – FAT stores even seconds only */

static gboolean cmp_usable(Panel *p)
{
    if (p->search_mode) {
        fm_status(p->fm, "Compare: leave the search results first");
        return FALSE;
    }
    if (p->busy) return FALSE;
    return TRUE;
}

static void mark_item(Panel *p, FileItem *it)
{
    if (!p->marks)
        p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    file_item_set_marked(it, TRUE);
    g_hash_table_add(p->marks, sel_mark_key(p, it));
}

/* name → FileItem (borrowed; the model keeps them alive) of plain files */
static GHashTable *files_of(Panel *p)
{
    GHashTable *h = g_hash_table_new(g_str_hash, g_str_equal);
    GListModel *m = G_LIST_MODEL(p->filter_model);
    guint n = g_list_model_get_n_items(m);
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(m, i);
        if (!it->is_dir && strcmp(it->name, "..") != 0)
            g_hash_table_insert(h, it->name, it);
        g_object_unref(it);
    }
    return h;
}

typedef struct { guint only, newer, different; } CmpCount;

/* Marks in `p` what should be copied to `o`; DIFFERENT (same time,
 * other size) is only counted – no side is obviously right. */
static CmpCount mark_side(Panel *p, GHashTable *mine, GHashTable *theirs)
{
    CmpCount c = { 0 };
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, mine);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        FileItem *a = v;
        FileItem *b = g_hash_table_lookup(theirs, k);
        FsCmp r = fs_compare_files(b != NULL, a->size_raw, a->date_raw,
                                   b ? b->size_raw : 0, b ? b->date_raw : 0,
                                   CMP_TIME_TOLERANCE);
        if (r == FS_CMP_ONLY)      { c.only++;  mark_item(p, a); }
        else if (r == FS_CMP_NEWER){ c.newer++; mark_item(p, a); }
        else if (r == FS_CMP_DIFFERENT) c.different++;
    }
    return c;
}

static void sync_copy(FM *fm, Panel *from, Panel *to)
{
    if (!from->marks || !g_hash_table_size(from->marks)) return;
    if (from->arc_file || to->arc_file) {
        fm_status(fm, "Synchronize: archives are read-only – extract with F5 instead");
        return;
    }
    GList *paths = panel_selection_paths(from);
    if (!paths) return;
    fo_transfer(fm, paths, from->ssh_conn, panel_dir(to), to->ssh_conn, FALSE);
    g_list_free_full(paths, g_free);
}

/* Clears the marks, then marks in each panel what the other one lacks */
static void do_mark(FM *fm, CmpCount *cl, CmpCount *cr, guint *same)
{
    Panel *l = &fm->panels[0], *r = &fm->panels[1];
    sel_mark_all(l, FALSE);
    sel_mark_all(r, FALSE);
    GHashTable *fl = files_of(l), *fr = files_of(r);
    *cl = mark_side(l, fl, fr);
    *cr = mark_side(r, fr, fl);
    *same = g_hash_table_size(fl) - cl->only - cl->newer - cl->different;
    g_hash_table_destroy(fl);
    g_hash_table_destroy(fr);
    sel_refresh(l);
    sel_refresh(r);
}

/* Marks both panels; counts per side (left = panels[0]).  FALSE = not usable */
gboolean cmp_mark_dirs(FM *fm, guint *left, guint *right, guint *different)
{
    if (!cmp_usable(&fm->panels[0]) || !cmp_usable(&fm->panels[1])) return FALSE;
    CmpCount cl, cr;
    guint same;
    do_mark(fm, &cl, &cr, &same);
    *left = cl.only + cl.newer;
    *right = cr.only + cr.newer;
    *different = cl.different;
    return TRUE;
}

void cmp_dirs(FM *fm)
{
    Panel *l = &fm->panels[0], *r = &fm->panels[1];
    if (!cmp_usable(l) || !cmp_usable(r)) return;
    CmpCount cl, cr;
    guint same;
    do_mark(fm, &cl, &cr, &same);

    guint nl = cl.only + cl.newer, nr = cr.only + cr.newer;
    fm_status(fm, "Compare: left %u marked (%u only, %u newer), right %u marked "
                  "(%u only, %u newer), %u differ in size only, %u same",
              nl, cl.only, cl.newer, nr, cr.only, cr.newer, cl.different, same);
    if (!nl && !nr) {
        gchar *msg = cl.different
            ? g_strdup_printf("No newer or missing files.\n\n%u file(s) have the same "
                              "time but a different size – check them with Alt+D.",
                              cl.different)
            : g_strdup("The directories contain the same files.");
        const char *ok[] = { "Close", NULL };
        ask_choice(fm, "Compare directories", msg, NULL, ok, -1);
        g_free(msg);
        return;
    }

    GString *m = g_string_new(NULL);
    g_string_append_printf(m, "<b>Left:</b> %u file(s) to copy (%u only here, %u newer)\n"
                              "<b>Right:</b> %u file(s) to copy (%u only here, %u newer)",
                           nl, cl.only, cl.newer, nr, cr.only, cr.newer);
    if (cl.different)
        g_string_append_printf(m, "\n\n%u file(s) have the same time but a different "
                                  "size – not marked, check them with Alt+D.", cl.different);
    g_string_append(m, "\n\nThe files are marked in both panels. Older files on the "
                       "target side are offered for overwriting.");
    const char *btns[] = { "Copy left → right", "Copy right → left", "Both ways", "Close", NULL };
    int a = ask_choice(fm, "Synchronize directories", m->str, NULL, btns, -1);
    g_string_free(m, TRUE);

    if ((a == 0 || a == 2) && nl) sync_copy(fm, l, r);
    if ((a == 1 || a == 2) && nr) sync_copy(fm, r, l);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Compare two files
 * ─────────────────────────────────────────────────────────────────── */

/* Two local files: two marked in the active panel, or the cursor file
 * in each panel (the other panel's namesake when its cursor is on ".."
 * or a directory). */
static gboolean pick_files(FM *fm, gchar **f1, gchar **f2)
{
    Panel *a = active_panel(fm), *o = other_panel(fm);
    *f1 = *f2 = NULL;
    if (a->ssh_conn || o->ssh_conn || a->arc_file || o->arc_file) {
        fm_status(fm, "Compare files works with local files only "
                      "(copy remote or archived files out first)");
        return FALSE;
    }
    GList *sel = a->marks && g_hash_table_size(a->marks) == 2
                 ? panel_selection_paths(a) : NULL;
    if (sel && g_list_length(sel) == 2) {
        *f1 = g_strdup(sel->data);
        *f2 = g_strdup(sel->next->data);
        g_list_free_full(sel, g_free);
    } else {
        g_list_free_full(sel, g_free);
        *f1 = panel_cursor_fullpath(a);
        *f2 = panel_cursor_fullpath(o);
        if (*f1 && (!*f2 || g_file_test(*f2, G_FILE_TEST_IS_DIR))) {
            g_free(*f2);
            gchar *base = g_path_get_basename(*f1);
            *f2 = g_build_filename(o->cwd, base, NULL);
            g_free(base);
        }
    }
    for (int i = 0; i < 2; i++) {
        const char *f = i ? *f2 : *f1;
        if (!f || !g_file_test(f, G_FILE_TEST_IS_REGULAR)) {
            fm_status(fm, "Compare files: put the cursor on a file in each panel "
                          "(or mark two files)");
            g_free(*f1); g_free(*f2);
            *f1 = *f2 = NULL;
            return FALSE;
        }
    }
    return TRUE;
}

static gchar *find_tool(FM *fm)
{
    if (fm->diff_tool && fm->diff_tool[0])
        return g_ascii_strcasecmp(fm->diff_tool, "builtin") == 0
               ? NULL : g_strdup(fm->diff_tool);
    static const char *const tools[] = { "meld", "kdiff3", "kompare", "diffuse", "bcompare" };
    for (gsize i = 0; i < G_N_ELEMENTS(tools); i++) {
        gchar *p = g_find_program_in_path(tools[i]);
        if (p) { g_free(p); return g_strdup(tools[i]); }
    }
    return NULL;
}

static gboolean on_diff_key(GtkEventControllerKey *k, guint keyval, guint code,
                            GdkModifierType state, GtkWindow *w)
{
    (void)k; (void)code; (void)state;
    if (keyval == GDK_KEY_Escape) { gtk_window_destroy(w); return TRUE; }
    return FALSE;
}

/* Byte-for-byte equality when `diff` is missing (files ≤ 256 MB) */
static int same_bytes(const char *a, const char *b)
{
    GMappedFile *ma = g_mapped_file_new(a, FALSE, NULL);
    GMappedFile *mb = g_mapped_file_new(b, FALSE, NULL);
    int r = -1;
    if (ma && mb) {
        gsize la = g_mapped_file_get_length(ma), lb = g_mapped_file_get_length(mb);
        r = la == lb && (la == 0 || memcmp(g_mapped_file_get_contents(ma),
                                           g_mapped_file_get_contents(mb), la) == 0);
    }
    if (ma) g_mapped_file_unref(ma);
    if (mb) g_mapped_file_unref(mb);
    return r;
}

#define DIFF_MAX_SHOW (8 * 1024 * 1024)

static void builtin_diff(FM *fm, const char *f1, const char *f2)
{
    gchar *out = NULL, *err = NULL;
    gint status = 0;
    GError *e = NULL;
    const gchar *argv[] = { "diff", "-u", "--", f1, f2, NULL };
    gboolean ran = g_spawn_sync(NULL, (gchar **)argv, NULL,
                                G_SPAWN_SEARCH_PATH, NULL, NULL,
                                &out, &err, &status, &e);
    int code = ran && WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (!ran) {
        g_clear_error(&e);
        int same = same_bytes(f1, f2);
        fm_status(fm, same == 1 ? "Files are identical" :
                      same == 0 ? "Files differ (install diffutils for a line-by-line view)"
                                : "Compare files: cannot read the files");
        g_free(out); g_free(err);
        return;
    }
    if (code == 0) {
        fm_status(fm, "Files are identical");
        g_free(out); g_free(err);
        return;
    }
    if (code != 1 || !out || !out[0]) {
        fm_status(fm, "Compare files: %s", err && err[0] ? g_strstrip(err) : "diff failed");
        g_free(out); g_free(err);
        return;
    }
    g_free(err);

    gsize len = strlen(out);
    gboolean cut = len > DIFF_MAX_SHOW;
    if (cut) out[DIFF_MAX_SHOW] = '\0';
    gchar *valid = g_utf8_make_valid(out, -1);
    g_free(out);

    GtkWidget *win = gtk_window_new();
    gchar *b1 = g_path_get_basename(f1), *b2 = g_path_get_basename(f2);
    gchar *title = g_strdup_printf("Compare: %s ↔ %s", b1, b2);
    gtk_window_set_title(GTK_WINDOW(win), title);
    g_free(title); g_free(b1); g_free(b2);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(win), 1000, 700);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gchar *hdr = g_markup_printf_escaped("<small><span foreground=\"#d33\">− %s</span>\n"
                                         "<span foreground=\"#2a2\">+ %s</span>%s</small>",
                                         f1, f2, cut ? "\n(output truncated)" : "");
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), hdr);
    g_free(hdr);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_margin_start(lbl, 8);
    gtk_widget_set_margin_top(lbl, 6);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_widget_add_css_class(tv, "fm-viewer");
    GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv));
    gboolean dark = fm_dark(fm);
    GtkTextTag *t_add = gtk_text_buffer_create_tag(buf, NULL,
        "paragraph-background", dark ? "#1f3b24" : "#e6ffec", NULL);
    GtkTextTag *t_del = gtk_text_buffer_create_tag(buf, NULL,
        "paragraph-background", dark ? "#4b2125" : "#ffebe9", NULL);
    GtkTextTag *t_hunk = gtk_text_buffer_create_tag(buf, NULL,
        "foreground", dark ? "#79b8ff" : "#0550ae", "weight", PANGO_WEIGHT_BOLD, NULL);

    GtkTextIter end;
    gchar **lines = g_strsplit(valid, "\n", -1);
    for (gchar **ln = lines; *ln; ln++) {
        if (!ln[1] && !**ln) break;             /* trailing newline */
        /* skip the "--- / +++" header – shown above */
        if ((g_str_has_prefix(*ln, "--- ") || g_str_has_prefix(*ln, "+++ ")) &&
            ln - lines < 2) continue;
        GtkTextTag *tag = **ln == '+' ? t_add : **ln == '-' ? t_del :
                          g_str_has_prefix(*ln, "@@") ? t_hunk : NULL;
        gtk_text_buffer_get_end_iter(buf, &end);
        gchar *line = g_strconcat(*ln, "\n", NULL);
        if (tag) gtk_text_buffer_insert_with_tags(buf, &end, line, -1, tag, NULL);
        else     gtk_text_buffer_insert(buf, &end, line, -1);
        g_free(line);
    }
    g_strfreev(lines);
    g_free(valid);

    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
    gtk_widget_set_vexpand(sc, TRUE);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), sc);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_diff_key), win);
    gtk_widget_add_controller(win, key);
    gtk_window_present(GTK_WINDOW(win));
}

void cmp_files(FM *fm)
{
    gchar *f1, *f2;
    if (!pick_files(fm, &f1, &f2)) return;

    gchar *tool = find_tool(fm);
    if (tool) {
        gint argc = 0;
        gchar **targv = NULL;
        GError *e = NULL;
        if (g_shell_parse_argv(tool, &argc, &targv, &e)) {
            GPtrArray *argv = g_ptr_array_new();
            for (gint i = 0; i < argc; i++) g_ptr_array_add(argv, targv[i]);
            g_ptr_array_add(argv, f1);
            g_ptr_array_add(argv, f2);
            g_ptr_array_add(argv, NULL);
            if (!g_spawn_async(NULL, (gchar **)argv->pdata, NULL,
                               G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &e)) {
                fm_status(fm, "Cannot start %s: %s – using the built-in compare",
                          targv[0], e->message);
                g_clear_error(&e);
                builtin_diff(fm, f1, f2);
            }
            g_ptr_array_free(argv, TRUE);
            g_strfreev(targv);
        } else {
            fm_status(fm, "Compare tool setting: %s", e->message);
            g_clear_error(&e);
        }
        g_free(tool);
    } else {
        builtin_diff(fm, f1, f2);
    }
    g_free(f1);
    g_free(f2);
}
