#include "fm.h"
#include "job.h"
#include "dialogs.h"
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Archives – extract / create via external tools (tar, zip, 7z, unrar)
 *  The tool runs as a child process watched by a Job worker thread.
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GPtrArray *argv;      /* NULL-terminated, owned strings */
    gchar     *cwd;       /* working directory for the tool */
    gboolean   ok;
    gchar     *error;     /* tail of stderr / spawn error   */
} ArcJob;

static void arc_job_init(ArcJob *a, const char *cwd)
{
    memset(a, 0, sizeof(*a));
    a->argv = g_ptr_array_new_with_free_func(g_free);
    a->cwd  = g_strdup(cwd);
}

static void arc_job_clear(ArcJob *a)
{
    g_ptr_array_unref(a->argv);
    g_free(a->cwd);
    g_free(a->error);
}

static void arg(ArcJob *a, const char *s)
{
    g_ptr_array_add(a->argv, g_strdup(s));
}

static void arc_worker(Job *job, gpointer d)
{
    ArcJob *a = d;
    g_ptr_array_add(a->argv, NULL);
    job_set_file(job, a->argv->pdata[0]);

    GPid pid;
    gint err_fd = -1;
    GError *e = NULL;
    if (!g_spawn_async_with_pipes(a->cwd, (gchar **)a->argv->pdata, NULL,
            G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD |
            G_SPAWN_STDOUT_TO_DEV_NULL,
            NULL, NULL, &pid, NULL, NULL, &err_fd, &e)) {
        a->error = g_strdup(e ? e->message : "spawn failed");
        g_clear_error(&e);
        return;
    }
    fcntl(err_fd, F_SETFL, fcntl(err_fd, F_GETFL) | O_NONBLOCK);

    GString *err = g_string_new(NULL);
    int status = 0;
    gboolean done = FALSE;
    while (!done) {
        char buf[1024];
        ssize_t n;
        while ((n = read(err_fd, buf, sizeof(buf))) > 0) {
            g_string_append_len(err, buf, n);
            if (err->len > 4096) g_string_erase(err, 0, err->len - 4096);
        }
        if (waitpid(pid, &status, WNOHANG) > 0) { done = TRUE; break; }
        if (job_cancelled(job)) {
            kill(pid, SIGTERM);
            waitpid(pid, &status, 0);
            break;
        }
        g_usleep(50000);
    }
    close(err_fd);
    g_spawn_close_pid(pid);

    a->ok = done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!a->ok && !job_cancelled(job)) {
        g_strstrip(err->str);
        const char *last = strrchr(err->str, '\n');
        a->error = g_strdup(err->len ? (last ? last + 1 : err->str)
                                     : "tool exited with an error");
    }
    g_string_free(err, TRUE);
}

static gboolean run_archive(FM *fm, const char *title, const char *phase, ArcJob *a)
{
    Job *job = job_new(fm, title);
    job_set_phase(job, phase);
    job_run(job, arc_worker, a);
    gboolean cancelled = job_cancelled(job);
    job_free(job);
    if (cancelled) fm_status(fm, "%s cancelled", title);
    return a->ok;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Extract
 * ─────────────────────────────────────────────────────────────────── */

void fo_extract(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    if (p->ssh_conn) {
        fm_status(fm, "Extract is not supported on SFTP panels");
        return;
    }

    gchar *archive = panel_cursor_fullpath(p);
    gchar *name = archive ? g_path_get_basename(archive) : NULL;
    ArcType type = arc_detect(name);
    if (!name || strcmp(name, "..") == 0 || type == ARC_UNKNOWN) {
        fm_status(fm, "No archive selected");
        g_free(name); g_free(archive);
        return;
    }

    Panel *o = other_panel(fm);
    gchar *mk = g_markup_printf_escaped("Extract \"<b>%s</b>\" to:", name);
    gchar *dest = ask_destination(fm, "Extract", mk, o->ssh_conn ? p->cwd : o->cwd);
    g_free(mk);
    if (!dest) { g_free(name); g_free(archive); return; }
    /* relative → relative to the panel directory, not the process cwd */
    if (!g_path_is_absolute(dest)) {
        gchar *abs = g_build_filename(p->cwd, dest, NULL);
        g_free(dest);
        dest = abs;
    }
    if (!g_file_test(dest, G_FILE_TEST_IS_DIR) && g_mkdir_with_parents(dest, 0755) != 0) {
        fm_status(fm, "Cannot create %s: %s", dest, g_strerror(errno));
        g_free(name); g_free(archive); g_free(dest);
        return;
    }

    ArcJob a;
    arc_job_init(&a, dest);
    switch (type) {
    case ARC_TAR_GZ: case ARC_TAR_BZ2: case ARC_TAR_XZ:
    case ARC_TAR_ZST: case ARC_TAR:
        arg(&a, "tar"); arg(&a, "-xf"); arg(&a, archive);
        arg(&a, "-C"); arg(&a, dest);
        break;
    case ARC_ZIP:
        arg(&a, "unzip"); arg(&a, "-o"); arg(&a, archive);
        arg(&a, "-d"); arg(&a, dest);
        break;
    case ARC_7Z: {
        gchar *o_arg = g_strdup_printf("-o%s", dest);
        arg(&a, "7z"); arg(&a, "x"); arg(&a, "-y"); arg(&a, o_arg);
        arg(&a, "--"); arg(&a, archive);
        g_free(o_arg);
        break;
    }
    case ARC_RAR: {
        gchar *d_slash = g_strconcat(dest, "/", NULL);
        arg(&a, "unrar"); arg(&a, "x"); arg(&a, "-y");
        arg(&a, "--"); arg(&a, archive); arg(&a, d_slash);
        g_free(d_slash);
        break;
    }
    default: break;
    }

    if (run_archive(fm, "Extract", "Extracting…", &a))
        fm_status(fm, "Extracted: %s → %s", name, dest);
    else if (a.error)
        fm_status(fm, "Extract failed: %s – %s", name, a.error);

    arc_job_clear(&a);
    g_free(name); g_free(archive); g_free(dest);
    panel_reload(&fm->panels[0]);
    panel_reload(&fm->panels[1]);
    panel_focus(p);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Pack
 * ─────────────────────────────────────────────────────────────────── */

static const ArcType pack_types[] = {
    ARC_TAR_GZ, ARC_TAR_BZ2, ARC_TAR_XZ, ARC_TAR_ZST, ARC_ZIP, ARC_7Z,
};

static void on_pack_fmt_changed(GObject *dd, GParamSpec *ps, GtkWidget *name_entry)
{
    (void)ps;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
    if (sel >= G_N_ELEMENTS(pack_types)) return;
    gchar *n = arc_with_extension(gtk_editable_get_text(GTK_EDITABLE(name_entry)),
                                  pack_types[sel]);
    gtk_editable_set_text(GTK_EDITABLE(name_entry), n);
    g_free(n);
}

void fo_pack(FM *fm)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    if (p->ssh_conn) {
        fm_status(fm, "Pack is not supported on SFTP panels");
        return;
    }
    if (p->search_mode) {
        fm_status(fm, "Leave search results first");
        return;
    }

    GList *targets = panel_selection(p);
    if (!targets) { fm_status(fm, "Nothing is selected"); return; }

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Create archive");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    gchar *hdr = g_strdup_printf("Files (%u):", g_list_length(targets));
    GtkWidget *flbl = gtk_label_new(hdr);
    g_free(hdr);
    gtk_widget_set_halign(flbl, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), flbl);
    gtk_box_append(GTK_BOX(vbox), dlg_file_list(targets));

    static const char *fmt_labels[] = {
        "tar.gz", "tar.bz2", "tar.xz", "tar.zst", "zip", "7z", NULL
    };
    GtkWidget *fmt_dd = gtk_drop_down_new_from_strings(fmt_labels);
    gtk_widget_set_hexpand(fmt_dd, TRUE);

    GtkWidget *name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(name_entry, TRUE);
    /* default: first item, or the directory name for multiple items */
    gchar *stem = targets->next ? g_path_get_basename(p->cwd)
                                : g_strdup((const char *)targets->data);
    gchar *default_name = g_strconcat(stem, ".tar.gz", NULL);
    gtk_editable_set_text(GTK_EDITABLE(name_entry), default_name);
    g_free(default_name); g_free(stem);
    g_signal_connect(fmt_dd, "notify::selected", G_CALLBACK(on_pack_fmt_changed), name_entry);

    GtkWidget *dst_entry = gtk_entry_new();
    Panel *o = other_panel(fm);
    gtk_editable_set_text(GTK_EDITABLE(dst_entry), o->ssh_conn ? p->cwd : o->cwd);
    gtk_widget_set_hexpand(dst_entry, TRUE);
    GtkWidget *browse = gtk_button_new_with_label("...");
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), dst_entry);
    GtkWidget *dst_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(dst_box), dst_entry);
    gtk_box_append(GTK_BOX(dst_box), browse);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    const char *keys[] = { "Format:", "Archive name:", "Save in:" };
    GtkWidget *vals[] = { fmt_dd, name_entry, dst_box };
    for (int i = 0; i < 3; i++) {
        GtkWidget *l = gtk_label_new(keys[i]);
        gtk_widget_set_halign(l, GTK_ALIGN_END);
        gtk_grid_attach(GTK_GRID(grid), l,       0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), vals[i], 1, i, 1, 1);
    }

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn = gtk_button_new_with_label("Create");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), grid);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(name_entry, "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);
    gtk_window_present(GTK_WINDOW(dlg));

    int resp = dlg_ctx_run(&ctx);
    gchar *arc_name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(name_entry)));
    gchar *dst_dir  = g_strdup(gtk_editable_get_text(GTK_EDITABLE(dst_entry)));
    guint  fmt_sel  = gtk_drop_down_get_selected(GTK_DROP_DOWN(fmt_dd));
    gtk_window_destroy(GTK_WINDOW(dlg));

    if (resp != 1 || !fs_name_is_safe(arc_name) || !dst_dir[0] ||
        fmt_sel >= G_N_ELEMENTS(pack_types)) {
        if (resp == 1) fm_status(fm, "Invalid archive name");
        goto out;
    }

    /* relative → relative to the panel directory */
    if (!g_path_is_absolute(dst_dir)) {
        gchar *abs = g_build_filename(p->cwd, dst_dir, NULL);
        g_free(dst_dir);
        dst_dir = abs;
    }
    gchar *arc_path = g_build_filename(dst_dir, arc_name, NULL);

    /* never replace one of the items being packed */
    gchar *canon = g_canonicalize_filename(arc_path, NULL);
    gboolean is_target = FALSE;
    for (GList *l = targets; l && !is_target; l = l->next) {
        gchar *t = g_build_filename(p->cwd, (const char *)l->data, NULL);
        gchar *ct = g_canonicalize_filename(t, NULL);
        is_target = strcmp(ct, canon) == 0;
        g_free(t); g_free(ct);
    }
    g_free(canon);
    if (is_target) {
        fm_status(fm, "The archive would replace a selected item: %s", arc_name);
        g_free(arc_path);
        goto out;
    }

    if (g_file_test(arc_path, G_FILE_TEST_EXISTS)) {
        gchar *q = g_markup_printf_escaped("<b>%s</b> already exists. Replace it?", arc_name);
        static const char *const b[] = { "Cancel", "Replace", NULL };
        int c = ask_choice(fm, "Create archive", q, NULL, b, 1);
        g_free(q);
        if (c != 1) { g_free(arc_path); goto out; }
    }

    /* Pack into a fresh temp name (zip/7z would add to an existing file)
     * and rename over the target only on success – the old archive
     * survives a failure or cancel.  The name keeps the extension:
     * zip/7z append their own otherwise. */
    gchar *tmp_path = NULL;
    do {
        g_free(tmp_path);
        gchar *t = g_strdup_printf(".fm-%06x-%s",
                                   (unsigned)g_random_int_range(0, 0x1000000), arc_name);
        tmp_path = g_build_filename(dst_dir, t, NULL);
        g_free(t);
    } while (g_file_test(tmp_path, G_FILE_TEST_EXISTS | G_FILE_TEST_IS_SYMLINK));

    ArcJob a;
    arc_job_init(&a, p->cwd);
    switch (pack_types[fmt_sel]) {
    case ARC_TAR_ZST:
        arg(&a, "tar"); arg(&a, "--zstd"); arg(&a, "-cf");
        break;
    case ARC_TAR_BZ2: arg(&a, "tar"); arg(&a, "-cjf"); break;
    case ARC_TAR_XZ:  arg(&a, "tar"); arg(&a, "-cJf"); break;
    case ARC_ZIP:     arg(&a, "zip"); arg(&a, "-r"); arg(&a, "-q"); arg(&a, "-y"); break;
    case ARC_7Z:      arg(&a, "7z");  arg(&a, "a");  arg(&a, "-y"); break;
    default:          arg(&a, "tar"); arg(&a, "-czf"); break;
    }
    arg(&a, tmp_path);
    if (pack_types[fmt_sel] == ARC_ZIP) {
        /* zip has no "--": prefix with ./ so names never look like options */
        for (GList *l = targets; l; l = l->next) {
            gchar *rel = g_strconcat("./", (const char *)l->data, NULL);
            arg(&a, rel);
            g_free(rel);
        }
    } else {
        arg(&a, "--");
        for (GList *l = targets; l; l = l->next) arg(&a, l->data);
    }

    if (run_archive(fm, "Pack", "Packing…", &a)) {
        if (rename(tmp_path, arc_path) == 0) {
            fm_status(fm, "Created: %s", arc_path);
            if (p->marks) g_hash_table_remove_all(p->marks);
        } else {
            fm_status(fm, "Pack failed: %s – %s", arc_name, g_strerror(errno));
            unlink(tmp_path);
        }
    } else {
        if (a.error) fm_status(fm, "Pack failed: %s – %s", arc_name, a.error);
        unlink(tmp_path);                   /* no partial archives */
    }
    arc_job_clear(&a);
    g_free(tmp_path);
    g_free(arc_path);

    panel_reload(&fm->panels[0]);
    panel_reload(&fm->panels[1]);
out:
    g_free(arc_name);
    g_free(dst_dir);
    g_list_free_full(targets, g_free);
    panel_focus(p);
}
