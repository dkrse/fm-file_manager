#include "fm.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>

/* ── String interning for directory paths ────────────────────────── */
/* Millions of files often share the same dir_path.  Instead of
 * g_strdup() per file, we intern strings so identical paths share
 * one allocation.  The pool is created per-search and freed at end. */

typedef struct {
    GHashTable *table;   /* char* → char*  (key == value, owned by table) */
} StringPool;

static void string_pool_init(StringPool *sp)
{
    sp->table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

static const char *string_pool_intern(StringPool *sp, const char *s)
{
    gpointer orig_key;
    if (g_hash_table_lookup_extended(sp->table, s, &orig_key, NULL))
        return (const char *)orig_key;
    gchar *dup = g_strdup(s);
    g_hash_table_add(sp->table, dup);
    return dup;
}

static void string_pool_destroy(StringPool *sp)
{
    g_hash_table_destroy(sp->table);
}

/* ── Content search helper ───────────────────────────────────────── */

static gchar *file_contains(const char *path, const char *needle_lower)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return NULL;

    unsigned char probe[512];
    size_t nr = fread(probe, 1, sizeof(probe), fp);
    for (size_t i = 0; i < nr; i++) {
        if (probe[i] == 0) { fclose(fp); return NULL; }
    }
    rewind(fp);

    char buf[4096];
    gchar *result = NULL;
    while (fgets(buf, sizeof(buf), fp)) {
        gchar *lower = g_utf8_strdown(buf, -1);
        if (strstr(lower, needle_lower)) {
            g_strstrip(buf);
            if (g_utf8_strlen(buf, -1) > 200) {
                gchar *p = g_utf8_offset_to_pointer(buf, 200);
                *p = '\0';
            }
            result = g_strdup(buf);
            g_free(lower);
            break;
        }
        g_free(lower);
    }
    fclose(fp);
    return result;
}

/* ── Lightweight result record (NOT a GObject) ───────────────────── */
/* We collect results as small structs first, then create FileItem
 * GObjects only for the final grouped list.  This halves peak memory
 * because we don't keep two sets of GObjects alive simultaneously. */

typedef struct {
    const char *dir;     /* interned – do NOT free */
    char       *name;    /* owned */
    gint64      size;
    gint64      mtime;
    mode_t      mode;
} SearchResult;

static void search_result_free(gpointer p)
{
    SearchResult *r = p;
    g_free(r->name);
    g_free(r);
}

/* ── Recursive search ─────────────────────────────────────────────── */

typedef struct {
    GPtrArray   *results;       /* SearchResult* */
    StringPool  *pool;
    const char  *pattern;
    const char  *content;
    gint64       size_min;
    gint64       size_max;
    int          count;         /* matched results */
    int          scanned;       /* total files visited */
    gboolean    *cancelled;    /* pointer to cancel flag (set by UI button) */
    dev_t        root_dev;     /* device of search root (for -xdev semantics) */
    GHashTable  *visited;      /* set of visited (dev,ino) pairs — loop detection */
    GtkWidget   *counter_label;
} SearchCtx;

static void add_result(SearchCtx *ctx, const char *base_interned,
                       const char *name, const struct stat *st)
{
    SearchResult *r = g_new(SearchResult, 1);
    r->dir   = base_interned;
    r->name  = g_strdup(name);
    r->size  = st->st_size;
    r->mtime = st->st_mtime;
    r->mode  = st->st_mode;
    g_ptr_array_add(ctx->results, r);
    ctx->count++;

    if ((ctx->count & 0xFFF) == 0) {   /* every 4096 results */
        gchar msg[64];
        g_snprintf(msg, sizeof(msg), "Found: %d", ctx->count);
        gtk_label_set_text(GTK_LABEL(ctx->counter_label), msg);
        while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
    }
}

/* Hash/equal for (dev_t, ino_t) pair — used for visited-directory tracking */
static guint devino_hash(gconstpointer p)
{
    const guint64 *v = p;   /* v[0]=dev, v[1]=ino */
    return g_int64_hash(&v[0]) ^ g_int64_hash(&v[1]);
}

static gboolean devino_equal(gconstpointer a, gconstpointer b)
{
    const guint64 *va = a, *vb = b;
    return va[0] == vb[0] && va[1] == vb[1];
}

static void search_dir(SearchCtx *ctx, const char *base, int depth)
{
    if (depth > 64 || *ctx->cancelled) return;

    struct stat base_st;
    if (lstat(base, &base_st) != 0) return;

    /* Skip symlinks to directories — prevents loops (like find -P) */
    if (S_ISLNK(base_st.st_mode)) return;

    /* Stay on same filesystem as search root (like find -xdev).
     * Skips /proc, /sys, /dev etc. when searching from / */
    if (base_st.st_dev != ctx->root_dev) return;

    /* Loop detection: skip already-visited (dev, ino) pairs.
     * Handles hard-linked directories and bind mounts. */
    guint64 key[2] = { base_st.st_dev, base_st.st_ino };
    if (g_hash_table_contains(ctx->visited, key)) return;
    guint64 *stored = g_memdup2(key, sizeof(key));
    g_hash_table_add(ctx->visited, stored);

    DIR *dir = opendir(base);
    if (!dir) return;

    const char *base_interned = string_pool_intern(ctx->pool, base);
    int dfd = dirfd(dir);

    struct dirent *de;
    struct stat st;

    while ((de = readdir(dir)) != NULL) {
        if (*ctx->cancelled) break;

        if (de->d_name[0] == '.') {
            if (de->d_name[1] == '\0') continue;
            if (de->d_name[1] == '.' && de->d_name[2] == '\0') continue;
        }

        if (fstatat(dfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;

        gboolean is_dir  = S_ISDIR(st.st_mode);
        gboolean is_link = S_ISLNK(st.st_mode);

        ctx->scanned++;
        if ((ctx->scanned & 0x1FFF) == 0) {   /* every 8192 files scanned */
            gchar msg[80];
            g_snprintf(msg, sizeof(msg), "Scanning: %d files, %d found",
                       ctx->scanned, ctx->count);
            gtk_label_set_text(GTK_LABEL(ctx->counter_label), msg);
            while (g_main_context_pending(NULL))
                g_main_context_iteration(NULL, FALSE);
        }

        if (!is_dir) {
            gboolean name_match = fnmatch(ctx->pattern, de->d_name, FNM_CASEFOLD) == 0;
            if (name_match) {
                if (ctx->size_min >= 0 && st.st_size < ctx->size_min) name_match = FALSE;
                if (ctx->size_max >= 0 && st.st_size > ctx->size_max) name_match = FALSE;
            }
            if (name_match) {
                if (ctx->content) {
                    gchar *ml = NULL;
                    if (!is_link) {
                        char full[4096];
                        g_snprintf(full, sizeof(full), "%s/%s", base, de->d_name);
                        ml = file_contains(full, ctx->content);
                    }
                    if (ml) {
                        add_result(ctx, base_interned, de->d_name, &st);
                        g_free(ml);
                    }
                } else {
                    add_result(ctx, base_interned, de->d_name, &st);
                }
            }
        }

        if (is_dir && !is_link) {
            char full[4096];
            g_snprintf(full, sizeof(full), "%s/%s", base, de->d_name);
            search_dir(ctx, full, depth + 1);
        }
    }
    closedir(dir);
}

/* ── Build grouped FileItem list from SearchResults ──────────────── */

static int cmp_result_by_dir(gconstpointer a, gconstpointer b)
{
    const SearchResult *ra = *(const SearchResult *const *)a;
    const SearchResult *rb = *(const SearchResult *const *)b;
    /* interned pointers: if same pointer, same string */
    if (ra->dir != rb->dir) {
        int r = strcmp(ra->dir, rb->dir);
        if (r != 0) return r;
    }
    return strcmp(ra->name, rb->name);
}

static GPtrArray *build_grouped_list(GPtrArray *results, FM *fm)
{
    if (results->len == 0)
        return g_ptr_array_new_with_free_func(g_object_unref);

    g_ptr_array_sort(results, cmp_result_by_dir);

    /* Estimate: results + ~1 header per 10 files */
    GPtrArray *out = g_ptr_array_sized_new(results->len + results->len / 10 + 100);
    g_ptr_array_set_free_func(out, g_object_unref);

    const char *prev_dir = NULL;

    for (guint i = 0; i < results->len; i++) {
        SearchResult *r = g_ptr_array_index(results, i);

        /* Directory header when dir changes (pointer comparison first) */
        if (r->dir != prev_dir && (!prev_dir || strcmp(r->dir, prev_dir) != 0)) {
            prev_dir = r->dir;
            gchar *vdc = fm_visible_color(fm, fm->dir_color);
            FileItem *hdr = file_item_new("folder-symbolic", r->dir,
                                          "", "", TRUE,
                                          vdc,
                                          PANGO_WEIGHT_BOLD,
                                          (gint64)-1, (gint64)0);
            g_free(vdc);
            g_ptr_array_add(out, hdr);
        }

        gboolean is_link = S_ISLNK(r->mode);
        gboolean is_exec = (r->mode & (S_IXUSR|S_IXGRP|S_IXOTH));
        const char *icon = icon_for_entry(r->name, FALSE, is_link, is_exec);

        const gchar *fg = NULL;
        gint wt = PANGO_WEIGHT_NORMAL;
        if (is_link)      fg = fm_link_color(fm);
        else if (is_exec) fg = fm_exec_color(fm);

        FileItem *fi = file_item_new_take(icon, r->name,
                          fmt_size(r->size), fmt_date(r->mtime),
                          FALSE, fg, wt,
                          r->size, r->mtime);
        fi->dir_path = g_strdup(r->dir);
        g_ptr_array_add(out, fi);
    }

    return out;
}

/* ── Size parsing helper ─────────────────────────────────────────── */

static gint64 parse_size_with_unit(const char *text, int unit_idx)
{
    if (!text || !text[0]) return -1;
    char *end;
    double val = g_ascii_strtod(text, &end);
    if (end == text || val < 0) return -1;
    static const gint64 mult[] = { 1, 1024, 1024*1024, (gint64)1024*1024*1024 };
    return (gint64)(val * mult[unit_idx]);
}

/* ── Search parameters (saved between calls) ─────────────────────── */

typedef struct {
    gchar *pattern;
    gchar *content;
    gchar *dir_path;
    gchar *size_min_text;
    gchar *size_max_text;
    int    size_min_unit;
    int    size_max_unit;
} SearchParams;

static SearchParams last_params = { NULL, NULL, NULL, NULL, NULL, 2, 2 };

static void search_params_save(const char *pattern, const char *content,
                               const char *dir_path,
                               const char *size_min_text, const char *size_max_text,
                               int size_min_unit, int size_max_unit)
{
    g_free(last_params.pattern);
    g_free(last_params.content);
    g_free(last_params.dir_path);
    g_free(last_params.size_min_text);
    g_free(last_params.size_max_text);
    last_params.pattern       = g_strdup(pattern);
    last_params.content       = g_strdup(content);
    last_params.dir_path      = g_strdup(dir_path);
    last_params.size_min_text = g_strdup(size_min_text);
    last_params.size_max_text = g_strdup(size_max_text);
    last_params.size_min_unit = size_min_unit;
    last_params.size_max_unit = size_max_unit;
}

/* ── Input dialog ────────────────────────────────────────────────── */

static GtkWidget *make_unit_dropdown(void)
{
    const char *units[] = { "B", "KB", "MB", "GB", NULL };
    GtkStringList *sl = gtk_string_list_new(units);
    GtkWidget *dd = gtk_drop_down_new(G_LIST_MODEL(sl), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), 2);
    gtk_widget_set_size_request(dd, 70, -1);
    return dd;
}

static gboolean show_input_dialog(FM *fm, gchar **out_pattern, gchar **out_content,
                                  gchar **out_dir, gint64 *out_size_min, gint64 *out_size_max)
{
    DlgCtx ictx;
    dlg_ctx_init(&ictx);

    GtkWidget *input_dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(input_dlg), "Search files");
    gtk_window_set_modal(GTK_WINDOW(input_dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(input_dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(input_dlg), 480, -1);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_widget_set_margin_start(grid, 12);
    gtk_widget_set_margin_end(grid, 12);
    gtk_widget_set_margin_top(grid, 12);
    gtk_widget_set_margin_bottom(grid, 12);

    int row = 0;

    GtkWidget *lbl_pat = gtk_label_new("File name (glob):");
    gtk_widget_set_halign(lbl_pat, GTK_ALIGN_END);
    GtkWidget *pat_entry = gtk_entry_new();
    gtk_widget_set_hexpand(pat_entry, TRUE);
    if (last_params.pattern)
        gtk_editable_set_text(GTK_EDITABLE(pat_entry), last_params.pattern);
    gtk_grid_attach(GTK_GRID(grid), lbl_pat,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), pat_entry,  1, row, 2, 1);
    row++;

    GtkWidget *lbl_cont = gtk_label_new("Content (text):");
    gtk_widget_set_halign(lbl_cont, GTK_ALIGN_END);
    GtkWidget *cont_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(cont_entry), "search text in files...");
    gtk_widget_set_hexpand(cont_entry, TRUE);
    if (last_params.content)
        gtk_editable_set_text(GTK_EDITABLE(cont_entry), last_params.content);
    gtk_grid_attach(GTK_GRID(grid), lbl_cont,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cont_entry,  1, row, 2, 1);
    row++;

    GtkWidget *lbl_dir = gtk_label_new("Search in:");
    gtk_widget_set_halign(lbl_dir, GTK_ALIGN_END);
    GtkWidget *dir_entry = gtk_entry_new();
    Panel *ap = active_panel(fm);
    const char *def_dir = ap->search_mode ? ap->search_prev_cwd : ap->cwd;
    gtk_editable_set_text(GTK_EDITABLE(dir_entry), def_dir);
    gtk_widget_set_hexpand(dir_entry, TRUE);
    GtkWidget *browse = gtk_button_new_with_label("...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), dir_entry);

    GtkWidget *dir_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(dir_box, TRUE);
    gtk_box_append(GTK_BOX(dir_box), dir_entry);
    gtk_box_append(GTK_BOX(dir_box), browse);
    gtk_grid_attach(GTK_GRID(grid), lbl_dir, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), dir_box, 1, row, 2, 1);
    row++;

    GtkWidget *lbl_smin = gtk_label_new("Size min:");
    gtk_widget_set_halign(lbl_smin, GTK_ALIGN_END);
    GtkWidget *smin_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(smin_entry), "e.g. 10");
    gtk_widget_set_hexpand(smin_entry, TRUE);
    if (last_params.size_min_text)
        gtk_editable_set_text(GTK_EDITABLE(smin_entry), last_params.size_min_text);
    GtkWidget *smin_unit = make_unit_dropdown();
    gtk_drop_down_set_selected(GTK_DROP_DOWN(smin_unit), last_params.size_min_unit);

    GtkWidget *smin_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(smin_box, TRUE);
    gtk_box_append(GTK_BOX(smin_box), smin_entry);
    gtk_box_append(GTK_BOX(smin_box), smin_unit);
    gtk_grid_attach(GTK_GRID(grid), lbl_smin, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), smin_box, 1, row, 2, 1);
    row++;

    GtkWidget *lbl_smax = gtk_label_new("Size max:");
    gtk_widget_set_halign(lbl_smax, GTK_ALIGN_END);
    GtkWidget *smax_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(smax_entry), "e.g. 100");
    gtk_widget_set_hexpand(smax_entry, TRUE);
    if (last_params.size_max_text)
        gtk_editable_set_text(GTK_EDITABLE(smax_entry), last_params.size_max_text);
    GtkWidget *smax_unit = make_unit_dropdown();
    gtk_drop_down_set_selected(GTK_DROP_DOWN(smax_unit), last_params.size_max_unit);

    GtkWidget *smax_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_hexpand(smax_box, TRUE);
    gtk_box_append(GTK_BOX(smax_box), smax_entry);
    gtk_box_append(GTK_BOX(smax_box), smax_unit);
    gtk_grid_attach(GTK_GRID(grid), lbl_smax, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), smax_box, 1, row, 2, 1);
    row++;

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *search_btn = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(search_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), search_btn);
    gtk_grid_attach(GTK_GRID(grid), btn_box, 0, row, 3, 1);

    gtk_window_set_child(GTK_WINDOW(input_dlg), grid);

    g_signal_connect(search_btn,  "clicked",       G_CALLBACK(dlg_ctx_ok),       &ictx);
    g_signal_connect(cancel_btn,  "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ictx);
    g_signal_connect(pat_entry,   "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ictx);
    g_signal_connect(cont_entry,  "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ictx);
    g_signal_connect(input_dlg,   "close-request", G_CALLBACK(dlg_ctx_close),    &ictx);

    gtk_window_present(GTK_WINDOW(input_dlg));

    if (dlg_ctx_run(&ictx) != 1) {
        gtk_window_destroy(GTK_WINDOW(input_dlg));
        return FALSE;
    }

    const gchar *raw_pat = gtk_editable_get_text(GTK_EDITABLE(pat_entry));
    const gchar *raw_cont = gtk_editable_get_text(GTK_EDITABLE(cont_entry));
    const gchar *dir_val = gtk_editable_get_text(GTK_EDITABLE(dir_entry));
    const gchar *smin_text = gtk_editable_get_text(GTK_EDITABLE(smin_entry));
    const gchar *smax_text = gtk_editable_get_text(GTK_EDITABLE(smax_entry));
    int smin_u = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(smin_unit));
    int smax_u = (int)gtk_drop_down_get_selected(GTK_DROP_DOWN(smax_unit));

    gchar pat_buf[512];
    if (!raw_pat[0] || strcmp(raw_pat, "*.*") == 0)
        g_strlcpy(pat_buf, "*", sizeof(pat_buf));
    else if (!strchr(raw_pat, '*') && !strchr(raw_pat, '?'))
        g_snprintf(pat_buf, sizeof(pat_buf), "*%s*", raw_pat);
    else
        g_strlcpy(pat_buf, raw_pat, sizeof(pat_buf));

    *out_pattern = g_strdup(pat_buf);
    *out_content = (raw_cont[0]) ? g_utf8_strdown(raw_cont, -1) : NULL;
    *out_dir = g_strdup(dir_val[0] ? dir_val : active_panel(fm)->cwd);
    *out_size_min = parse_size_with_unit(smin_text, smin_u);
    *out_size_max = parse_size_with_unit(smax_text, smax_u);

    search_params_save(raw_pat, raw_cont, dir_val, smin_text, smax_text, smin_u, smax_u);

    gtk_window_destroy(GTK_WINDOW(input_dlg));
    return TRUE;
}

/* ── Cancel callback ─────────────────────────────────────────────── */

static void on_search_cancel(GtkButton *btn, gpointer data)
{
    (void)btn;
    *((gboolean *)data) = TRUE;
}

/* ── Main search entry point ─────────────────────────────────────── */

void search_run(FM *fm)
{
    if (active_panel(fm)->ssh_conn) {
        fm_status(fm, "Search on remote panel is not supported");
        return;
    }

    gchar *pattern = NULL, *content = NULL, *search_dir_path = NULL;
    gint64 size_min, size_max;

    if (!show_input_dialog(fm, &pattern, &content, &search_dir_path, &size_min, &size_max))
        return;

    Panel *p = active_panel(fm);

    if (!p->search_mode)
        g_strlcpy(p->search_prev_cwd, p->cwd, sizeof(p->search_prev_cwd));

    gchar *info_text;
    if (content)
        info_text = g_strdup_printf("Search: %s containing \"%s\" in %s",
                                    pattern, content, search_dir_path);
    else
        info_text = g_strdup_printf("Search: %s in %s", pattern, search_dir_path);

    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), info_text);
    gtk_label_set_text(GTK_LABEL(p->status_label), "Searching...");

    /* Block selection callbacks during search to prevent freeze on mouse click */
    p->inhibit_sel = TRUE;
    fm->panels[0].inhibit_sel = TRUE;
    fm->panels[1].inhibit_sel = TRUE;

    /* Show cancel button below status label */
    gboolean search_cancelled = FALSE;
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel search");
    gtk_widget_add_css_class(cancel_btn, "fm-fkey");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_search_cancel), &search_cancelled);
    GtkWidget *frame = gtk_widget_get_parent(p->status_label);
    gtk_box_append(GTK_BOX(frame), cancel_btn);

    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);

    /* Phase 1: collect lightweight result records */
    StringPool pool;
    string_pool_init(&pool);

    /* Get device of search root for -xdev semantics */
    struct stat root_st;
    dev_t root_dev = 0;
    if (lstat(search_dir_path, &root_st) == 0)
        root_dev = root_st.st_dev;

    SearchCtx ctx = {
        .results       = g_ptr_array_sized_new(4096),
        .pool          = &pool,
        .pattern       = pattern,
        .content       = content,
        .size_min      = size_min,
        .size_max      = size_max,
        .count         = 0,
        .scanned       = 0,
        .cancelled     = &search_cancelled,
        .root_dev      = root_dev,
        .visited       = g_hash_table_new_full(devino_hash, devino_equal, g_free, NULL),
        .counter_label = p->status_label,
    };
    g_ptr_array_set_free_func(ctx.results, search_result_free);

    search_dir(&ctx, search_dir_path, 0);
    int result_count = ctx.count;
    g_hash_table_destroy(ctx.visited);

    /* Remove cancel button */
    gtk_box_remove(GTK_BOX(frame), cancel_btn);

    /* Phase 2: sort + build grouped FileItem list */
    gtk_label_set_text(GTK_LABEL(p->status_label), "Building list...");
    while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);

    GPtrArray *grouped = build_grouped_list(ctx.results, fm);

    /* Free lightweight results — FileItems in grouped now own the data */
    g_ptr_array_unref(ctx.results);
    string_pool_destroy(&pool);

    /* Phase 3: load into panel */
    gtk_sort_list_model_set_sorter(p->sort_model, NULL);

    /* Restore inhibit on both panels (was set to block clicks during search) */
    fm->panels[0].inhibit_sel = FALSE;
    fm->panels[1].inhibit_sel = FALSE;
    p->inhibit_sel = TRUE;
    if (p->marks)
        g_hash_table_remove_all(p->marks);

    /* Replace store to avoid expensive remove_all on large lists */
    GListStore *new_store = g_list_store_new(FILE_ITEM_TYPE);
    if (grouped->len > 0)
        g_list_store_splice(new_store, 0, 0,
                            (gpointer *)grouped->pdata, grouped->len);
    g_ptr_array_unref(grouped);
    gtk_sort_list_model_set_model(p->sort_model, G_LIST_MODEL(new_store));
    g_object_unref(p->store);
    p->store = new_store;

    p->search_mode = TRUE;
    gtk_widget_set_visible(p->search_btn, TRUE);
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), info_text);

    const char *suffix = search_cancelled ? " (cancelled)" : "";
    gchar *done_msg = g_strdup_printf("Found: %d files%s  (Enter=go to file, Backspace=back)",
                                      result_count, suffix);
    gtk_label_set_text(GTK_LABEL(p->status_label), done_msg);
    g_free(done_msg);
    g_free(info_text);

    p->cursor_pos = 0;
    if (g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0) {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL, GTK_LIST_SCROLL_SELECT, NULL);
    }
    p->inhibit_sel = FALSE;
    gtk_widget_grab_focus(p->column_view);

    g_free(pattern);
    g_free(content);
    g_free(search_dir_path);
}
