/* Checksums: Alt+K creates MD5/SHA-1/SHA-256/SHA-512 sums of the marked
 * files (directories recursively), Alt+Shift+K verifies a checksum file
 * (GNU "hash  name" or BSD "SHA256 (name) = hash").  Local files only;
 * hashing runs in a Job (progress, cancel). */
#include "fm.h"
#include "dialogs.h"
#include "job.h"
#include <string.h>

typedef struct {
    gchar *path;      /* full path on disk                      */
    gchar *name;      /* as written into / read from the file   */
    gchar *expect;    /* verify: expected hash, NULL = create   */
    GChecksumType type;
    gchar *result;    /* hex digest, NULL = error / missing     */
    gboolean missing;
} CksItem;

static void cks_item_free(gpointer d)
{
    CksItem *c = d;
    g_free(c->path); g_free(c->name); g_free(c->expect); g_free(c->result);
    g_free(c);
}

typedef struct {
    GPtrArray *items;  /* CksItem* */
    gint64     total;
    /* create: what to scan in the worker (a big tree / slow mount froze
     * the UI with no cancel when this ran on the main thread) */
    GPtrArray *roots;  /* CksItem*: path + name of each selected entry */
    GChecksumType type;
    gboolean   verify; /* items need a stat (missing / size) first */
} CksJob;

static void collect(Job *job, GPtrArray *items, gint64 *total, const char *path,
                    const char *name, GChecksumType type, guint depth);

/* scanning part of the worker (job may be NULL: no cancel) */
static void cks_scan_job(Job *job, CksJob *cj)
{
    if (cj->roots)
        for (guint i = 0; i < cj->roots->len && !(job && job_cancelled(job)); i++) {
            CksItem *r = g_ptr_array_index(cj->roots, i);
            collect(job, cj->items, &cj->total, r->path, r->name, cj->type, 0);
        }
    if (cj->verify)
        for (guint i = 0; i < cj->items->len && !(job && job_cancelled(job)); i++) {
            CksItem *c = g_ptr_array_index(cj->items, i);
            struct stat st;
            if (!c->path || stat(c->path, &st) != 0 || !S_ISREG(st.st_mode)) c->missing = TRUE;
            else cj->total += st.st_size;
        }
}

static void cks_worker(Job *job, gpointer d)
{
    CksJob *cj = d;
    job_set_phase(job, "Scanning…");
    cks_scan_job(job, cj);
    if (job_cancelled(job)) return;
    job_set_phase(job, "Hashing…");
    job_set_total(job, cj->total);
    for (guint i = 0; i < cj->items->len && !job_cancelled(job); i++) {
        CksItem *c = g_ptr_array_index(cj->items, i);
        if (c->missing) continue;
        job_set_file(job, c->name);
        c->result = fs_checksum_file(&job->ctx, c->path, c->type);
        if (!c->result) g_clear_pointer(&job->ctx.error, g_free); /* per item */
    }
}

static gboolean run_job(FM *fm, const char *title, CksJob *cj)
{
    Job *job = job_new(fm, title);
    job->allow_background = FALSE;   /* the result window follows */
    job_run(job, cks_worker, cj);
    gboolean ok = !job_cancelled(job);
    job_free(job);
    if (!ok) fm_status(fm, "%s cancelled", title);
    return ok;
}

/* ── result window ─────────────────────────────────────────────────── */

typedef struct {
    FM    *fm;
    gchar *text;        /* checksum file contents (create)  */
    gchar *dir;         /* where Save writes                 */
    gchar *def_name;
    GtkWidget *win;
} ResultWin;

static void result_free(gpointer d)
{
    ResultWin *r = d;
    g_free(r->text); g_free(r->dir); g_free(r->def_name);
    g_free(r);
}

static void on_res_copy(GtkButton *b, ResultWin *r)
{
    (void)b;
    gdk_clipboard_set_text(gtk_widget_get_clipboard(r->win), r->text);
    fm_status(r->fm, "Checksums copied to the clipboard");
}

static void on_res_save(GtkButton *b, ResultWin *r)
{
    (void)b;
    gchar *name = ask_text(r->fm, "Save checksums", "Save as (in the current directory):",
                           r->def_name, "Save", TRUE);
    if (!name) return;
    if (!fs_name_is_safe(name)) {
        fm_status(r->fm, "Invalid file name");
        g_free(name);
        return;
    }
    gchar *path = g_build_filename(r->dir, name, NULL);
    gboolean go = TRUE;
    if (g_file_test(path, G_FILE_TEST_EXISTS)) {
        gchar *q = g_markup_printf_escaped("'%s' already exists. Overwrite it?", name);
        go = ask_yes_no(r->fm, "Save checksums", q);
        g_free(q);
    }
    GError *e = NULL;
    if (go && g_file_set_contents(path, r->text, -1, &e)) {
        fm_status(r->fm, "Saved %s", path);
        for (int i = 0; i < 2; i++)
            if (!r->fm->panels[i].ssh_conn && strcmp(r->fm->panels[i].cwd, r->dir) == 0)
                panel_reload(&r->fm->panels[i]);
        gtk_window_destroy(GTK_WINDOW(r->win));
    } else if (go) {
        fm_status(r->fm, "Cannot save: %s", e->message);
        g_clear_error(&e);
    }
    g_free(path);
    g_free(name);
}

static gboolean on_res_key(GtkEventControllerKey *k, guint keyval, guint code,
                           GdkModifierType state, GtkWindow *w)
{
    (void)k; (void)code; (void)state;
    if (keyval == GDK_KEY_Escape) { gtk_window_destroy(w); return TRUE; }
    return FALSE;
}

static void on_res_close(GtkButton *b, GtkWindow *w) { (void)b; gtk_window_destroy(w); }

/* summary = markup; body = plain text shown in the list */
static void show_result(FM *fm, const char *title, const char *summary,
                        const char *body, const char *save_text,
                        const char *dir, const char *def_name)
{
    ResultWin *r = g_new0(ResultWin, 1);
    r->fm = fm;
    r->text = g_strdup(save_text ? save_text : body);
    r->dir = g_strdup(dir);
    r->def_name = g_strdup(def_name);

    GtkWidget *win = gtk_window_new();
    r->win = win;
    g_object_set_data_full(G_OBJECT(win), "fm-res", r, result_free);
    gtk_window_set_title(GTK_WINDOW(win), title);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fm->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(win), 820, 420);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 12);
    gtk_widget_set_margin_end(box, 12);
    gtk_widget_set_margin_top(box, 10);
    gtk_widget_set_margin_bottom(box, 10);

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), summary);
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);

    GtkWidget *tv = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)), body, -1);
    GtkWidget *sc = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
    gtk_widget_set_vexpand(sc, TRUE);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *copy = gtk_button_new_with_label("Copy");
    g_signal_connect(copy, "clicked", G_CALLBACK(on_res_copy), r);
    gtk_box_append(GTK_BOX(row), copy);
    if (def_name) {
        GtkWidget *save = gtk_button_new_with_label("Save to file…");
        g_signal_connect(save, "clicked", G_CALLBACK(on_res_save), r);
        gtk_box_append(GTK_BOX(row), save);
    }
    GtkWidget *close = gtk_button_new_with_label("Close");
    g_signal_connect(close, "clicked", G_CALLBACK(on_res_close), win);
    gtk_box_append(GTK_BOX(row), close);

    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), sc);
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(GTK_WINDOW(win), box);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_res_key), win);
    gtk_widget_add_controller(win, key);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(close);
}

/* ── create ───────────────────────────────────────────────────────── */

static gboolean local_panel(Panel *p)
{
    if (p->ssh_conn || p->arc_file) {
        fm_status(p->fm, "Checksums work with local files only");
        return FALSE;
    }
    return TRUE;
}

static gint cmp_names(gconstpointer a, gconstpointer b)
{
    return g_strcmp0(*(const char *const *)a, *(const char *const *)b);
}

/* Files below `path`, named relative to the panel directory */
static void collect(Job *job, GPtrArray *items, gint64 *total, const char *path,
                    const char *name, GChecksumType type, guint depth)
{
    struct stat st;
    if ((job && job_cancelled(job)) || lstat(path, &st) != 0 || depth > 64) return;
    if (S_ISREG(st.st_mode)) {
        CksItem *c = g_new0(CksItem, 1);
        c->path = g_strdup(path);
        c->name = g_strdup(name);
        c->type = type;
        g_ptr_array_add(items, c);
        *total += st.st_size;
    } else if (S_ISDIR(st.st_mode)) {
        GDir *d = g_dir_open(path, 0, NULL);
        if (!d) return;
        GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
        const char *n;
        while ((n = g_dir_read_name(d))) g_ptr_array_add(names, g_strdup(n));
        g_dir_close(d);
        g_ptr_array_sort(names, cmp_names);
        for (guint i = 0; i < names->len; i++) {
            const char *sub = g_ptr_array_index(names, i);
            gchar *fp = g_build_filename(path, sub, NULL);
            gchar *fn = g_build_filename(name, sub, NULL);
            collect(job, items, total, fp, fn, type, depth + 1);
            g_free(fp); g_free(fn);
        }
        g_ptr_array_unref(names);
    }
    /* symlinks, devices: skipped */
}

void cks_create(FM *fm)
{
    Panel *p = active_panel(fm);
    if (!local_panel(p)) return;
    GList *paths = panel_selection_paths(p);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }

    static const char *const btns[] = { "MD5", "SHA-1", "SHA-512", "SHA-256", NULL };
    static const GChecksumType types[] = { G_CHECKSUM_MD5, G_CHECKSUM_SHA1,
                                           G_CHECKSUM_SHA512, G_CHECKSUM_SHA256 };
    gchar *q = dlg_count_prompt("Calculate checksums of", paths, "");
    int a = ask_choice(fm, "Create checksums", q, NULL, btns, -1);
    g_free(q);
    if (a < 0) { g_list_free_full(paths, g_free); return; }
    GChecksumType type = types[a];

    CksJob cj = { g_ptr_array_new_with_free_func(cks_item_free), 0,
                  g_ptr_array_new_with_free_func(cks_item_free), type, FALSE };
    const char *dir = p->search_mode ? p->search_prev_cwd : p->cwd;
    for (GList *l = paths; l; l = l->next) {
        const char *full = l->data;
        CksItem *r = g_new0(CksItem, 1);
        r->path = g_strdup(full);
        /* search results: names relative to the search start if inside it */
        r->name = fs_path_is_inside(full, dir) && strlen(full) > strlen(dir)
                  ? g_strdup(full + strlen(dir) + (dir[strlen(dir) - 1] == '/' ? 0 : 1))
                  : g_path_get_basename(full);
        g_ptr_array_add(cj.roots, r);
    }
    gchar *first = paths->next ? NULL : g_path_get_basename(paths->data);
    g_list_free_full(paths, g_free);

    gboolean ran = run_job(fm, "Calculating checksums", &cj);
    if (ran && !cj.items->len) {
        fm_status(fm, "No regular files to checksum");
    } else if (ran) {
        GString *out = g_string_new(NULL);
        guint fail = 0;
        for (guint i = 0; i < cj.items->len; i++) {
            CksItem *c = g_ptr_array_index(cj.items, i);
            if (c->result) g_string_append_printf(out, "%s  %s\n", c->result, c->name);
            else fail++;
        }
        gchar *fail_txt = fail ? g_strdup_printf(" – %u could not be read", fail)
                               : g_strdup("");
        gchar *summary = g_markup_printf_escaped("<b>%s</b> of %u file(s)%s",
            fs_checksum_name(type), cj.items->len - fail, fail_txt);
        g_free(fail_txt);
        gchar *def;
        if (first) def = g_strconcat(first, fs_checksum_ext(type), NULL);
        else {
            gchar *b = g_path_get_basename(dir);
            def = g_strconcat(strcmp(b, "/") ? b : "checksums", fs_checksum_ext(type), NULL);
            g_free(b);
        }
        show_result(fm, "Checksums", summary, out->str, NULL, dir, def);
        g_free(def);
        g_free(summary);
        g_string_free(out, TRUE);
    }
    g_free(first);
    g_ptr_array_unref(cj.roots);
    g_ptr_array_unref(cj.items);
}

/* ── verify ───────────────────────────────────────────────────────── */

void cks_verify(FM *fm)
{
    Panel *p = active_panel(fm);
    if (!local_panel(p)) return;
    gchar *file = panel_cursor_fullpath(p);
    gchar *data = NULL;
    gsize len = 0;
    gboolean canc = FALSE;
    if (file)       /* worker read – the size limit is checked while reading */
        data = fm_read_file(fm, NULL, file, 64 * 1024 * 1024 + 1, &len, NULL, &canc);
    if (canc) {
        fm_status(fm, "Verify: reading cancelled");
        g_free(file);
        return;
    }
    if (!data || len > 64 * 1024 * 1024) {
        fm_status(fm, "Verify: put the cursor on a checksum file (.sha256, .md5, …)");
        g_free(file); g_free(data);
        return;
    }
    gchar *dir = g_path_get_dirname(file);
    CksJob cj = { g_ptr_array_new_with_free_func(cks_item_free), 0, NULL, 0, TRUE };
    guint bad_lines = 0;
    gchar **lines = g_strsplit(data, "\n", -1);
    g_free(data);
    for (gchar **ln = lines; *ln; ln++) {
        gchar *hash, *name;
        if (!fs_parse_checksum_line(*ln, &hash, &name)) {
            gchar *t = g_strstrip(g_strdup(*ln));
            if (t[0] && t[0] != '#') bad_lines++;
            g_free(t);
            continue;
        }
        CksItem *c = g_new0(CksItem, 1);
        fs_checksum_type_for_hex(strlen(hash), &c->type);
        c->expect = hash;
        c->name = name;
        const char *rel = g_str_has_prefix(name, "./") ? name + 2 : name;
        if (g_path_is_absolute(name)) c->path = g_strdup(name);
        else if (fs_relpath_is_safe(rel)) c->path = g_build_filename(dir, rel, NULL);
        /* stat (missing / size) happens in the worker */
        g_ptr_array_add(cj.items, c);
    }
    g_strfreev(lines);

    if (!cj.items->len) {
        fm_status(fm, "Verify: no checksums found in this file");
    } else if (run_job(fm, "Verifying checksums", &cj)) {
        guint ok = 0, failed = 0, missing = 0;
        GString *out = g_string_new(NULL);
        GString *bad = g_string_new(NULL);
        for (guint i = 0; i < cj.items->len; i++) {
            CksItem *c = g_ptr_array_index(cj.items, i);
            const char *st;
            if (c->missing)                          { st = "MISSING"; missing++; }
            else if (!c->result)                     { st = "ERROR  "; failed++; }
            else if (strcmp(c->result, c->expect))   { st = "FAILED "; failed++; }
            else                                     { st = "OK     "; ok++; }
            GString *dst = (st[0] == 'O') ? out : bad;   /* problems first */
            g_string_append_printf(dst, "%s  %s\n", st, c->name);
        }
        g_string_prepend(out, bad->str);
        gchar *base = g_path_get_basename(file);
        gchar *summary = g_markup_printf_escaped(
            "<b>%s</b>: <span foreground=\"%s\"><b>%s</b></span> – "
            "%u OK, %u failed, %u missing%s",
            base, failed || missing ? "#d33" : "#2a2",
            failed || missing ? "PROBLEMS FOUND" : "ALL OK",
            ok, failed, missing,
            bad_lines ? " (some lines were not understood)" : "");
        show_result(fm, "Verify checksums", summary, out->str, NULL, dir, NULL);
        fm_status(fm, "Verify: %u OK, %u failed, %u missing", ok, failed, missing);
        g_free(summary);
        g_free(base);
        g_string_free(bad, TRUE);
        g_string_free(out, TRUE);
    }
    g_ptr_array_unref(cj.items);
    g_free(dir);
    g_free(file);
}
