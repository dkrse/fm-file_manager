#include "job.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Run a function on the main thread and wait (used by workers)
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    gpointer (*fn)(gpointer);
    gpointer  arg;
    gpointer  ret;
    GMutex    m;
    GCond     c;
    gboolean  done;
} MainCall;

static gboolean main_call_idle(gpointer d)
{
    MainCall *mc = d;
    gpointer r = mc->fn(mc->arg);
    g_mutex_lock(&mc->m);
    mc->ret  = r;
    mc->done = TRUE;
    g_cond_signal(&mc->c);
    g_mutex_unlock(&mc->m);
    return G_SOURCE_REMOVE;
}

gpointer fm_call_on_main(gpointer (*fn)(gpointer), gpointer arg)
{
    if (g_main_context_is_owner(g_main_context_default()))
        return fn(arg);

    MainCall mc = { .fn = fn, .arg = arg };
    g_mutex_init(&mc.m);
    g_cond_init(&mc.c);
    g_mutex_lock(&mc.m);
    g_idle_add_full(G_PRIORITY_DEFAULT, main_call_idle, &mc, NULL);
    while (!mc.done) g_cond_wait(&mc.c, &mc.m);
    g_mutex_unlock(&mc.m);
    g_mutex_clear(&mc.m);
    g_cond_clear(&mc.c);
    return mc.ret;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Overwrite question (runs on the main thread)
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    Job        *job;
    const char *src;
    const char *dst;
} ConflictReq;

typedef struct {
    GMainLoop *loop;
    FsAnswer   answer;
} ConflictDlg;

static void on_conflict_btn(GtkButton *b, ConflictDlg *cd)
{
    cd->answer = (FsAnswer)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "answer"));
    g_main_loop_quit(cd->loop);
}

static gboolean on_conflict_close(GtkWindow *w, ConflictDlg *cd)
{
    (void)w;
    cd->answer = FS_ANS_CANCEL;
    g_main_loop_quit(cd->loop);
    return TRUE;
}

static gchar *describe_local(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return g_strdup("");
    gchar *sz = S_ISDIR(st.st_mode) ? g_strdup("directory") : fmt_size(st.st_size);
    gchar *dt = fmt_date(st.st_mtime);
    gchar *r  = g_strdup_printf("%s, %s", sz, dt);
    g_free(sz); g_free(dt);
    return r;
}

static gpointer conflict_main(gpointer d)
{
    ConflictReq *rq = d;
    Job *job = rq->job;

    ConflictDlg cd = { .loop = g_main_loop_new(NULL, FALSE), .answer = FS_ANS_CANCEL };

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "File exists");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg),
        GTK_WINDOW(job->window ? job->window : job->fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    gchar *dname = g_path_get_basename(rq->dst);
    gchar *mk = g_markup_printf_escaped("<b>%s</b> already exists. Overwrite?", dname);
    GtkWidget *q = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(q), mk);
    gtk_label_set_wrap(GTK_LABEL(q), TRUE);
    gtk_widget_set_halign(q, GTK_ALIGN_START);
    g_free(mk); g_free(dname);
    gtk_box_append(GTK_BOX(vbox), q);

    /* Details only for local paths (remote ones would need the lock) */
    const char *labels[2] = { "New:", "Existing:" };
    const char *paths[2]  = { rq->src, rq->dst };
    const gboolean remote[2] = { job->src_remote, job->dst_remote };
    for (int i = 0; i < 2; i++) {
        gchar *info = (!remote[i] && paths[i][0] == '/' &&
                       g_file_test(paths[i], G_FILE_TEST_EXISTS))
                      ? describe_local(paths[i]) : g_strdup("");
        gchar *t = g_strdup_printf("%s %s  %s", labels[i], paths[i], info);
        GtkWidget *l = gtk_label_new(t);
        gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_set_halign(l, GTK_ALIGN_START);
        gtk_widget_add_css_class(l, "dim-label");
        gtk_box_append(GTK_BOX(vbox), l);
        g_free(t); g_free(info);
    }

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bb, GTK_ALIGN_END);
    gtk_widget_set_margin_top(bb, 8);
    static const struct { const char *label; FsAnswer a; } btns[] = {
        { "Cancel",        FS_ANS_CANCEL        },
        { "Skip all",      FS_ANS_SKIP_ALL      },
        { "Skip",          FS_ANS_SKIP          },
        { "Overwrite all", FS_ANS_OVERWRITE_ALL },
        { "Overwrite",     FS_ANS_OVERWRITE     },
    };
    GtkWidget *def = NULL;
    for (gsize i = 0; i < G_N_ELEMENTS(btns); i++) {
        GtkWidget *b = gtk_button_new_with_label(btns[i].label);
        g_object_set_data(G_OBJECT(b), "answer", GINT_TO_POINTER(btns[i].a));
        g_signal_connect(b, "clicked", G_CALLBACK(on_conflict_btn), &cd);
        gtk_box_append(GTK_BOX(bb), b);
        if (btns[i].a == FS_ANS_SKIP) def = b;
        if (btns[i].a == FS_ANS_OVERWRITE)
            gtk_widget_add_css_class(b, "destructive-action");
    }
    gtk_box_append(GTK_BOX(vbox), bb);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);
    g_signal_connect(dlg, "close-request", G_CALLBACK(on_conflict_close), &cd);

    gtk_window_present(GTK_WINDOW(dlg));
    if (def) gtk_widget_grab_focus(def);
    g_main_loop_run(cd.loop);
    g_main_loop_unref(cd.loop);
    gtk_window_destroy(GTK_WINDOW(dlg));
    return GINT_TO_POINTER(cd.answer);
}

static FsAnswer job_conflict_cb(FsCtx *ctx, const char *src, const char *dst)
{
    Job *job = ctx->user;
    ConflictReq rq = { job, src, dst };
    /* pause the speed clock while the user decides */
    gint64 t0 = g_get_monotonic_time();
    FsAnswer a = (FsAnswer)GPOINTER_TO_INT(fm_call_on_main(conflict_main, &rq));
    g_mutex_lock(&job->lock);
    job->start_time += g_get_monotonic_time() - t0;
    g_mutex_unlock(&job->lock);
    return a;
}

static void job_progress_cb(FsCtx *ctx, const char *name, gint64 bytes)
{
    Job *job = ctx->user;
    g_mutex_lock(&job->lock);
    if (name && (!job->file || strcmp(job->file, name) != 0)) {
        g_free(job->file);
        job->file = g_strdup(name);
    }
    if (!job->count_items) job->done += bytes;
    job->file_done += bytes;
    g_mutex_unlock(&job->lock);
}

static void job_file_start_cb(FsCtx *ctx, const char *src, const char *dst, gint64 size)
{
    Job *job = ctx->user;
    g_mutex_lock(&job->lock);
    g_free(job->from);
    g_free(job->to);
    job->from = g_strdup(src);
    job->to   = g_strdup(dst);
    job->file_total = size;
    job->file_done  = 0;
    g_mutex_unlock(&job->lock);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Worker-side setters
 * ─────────────────────────────────────────────────────────────────── */

void job_set_phase(Job *job, const char *phase)
{
    g_mutex_lock(&job->lock);
    g_free(job->phase);
    job->phase = g_strdup(phase);
    g_mutex_unlock(&job->lock);
}

void job_set_file(Job *job, const char *file)
{
    g_mutex_lock(&job->lock);
    g_free(job->file);
    job->file = g_strdup(file);
    g_mutex_unlock(&job->lock);
}

void job_set_total(Job *job, gint64 total)
{
    g_mutex_lock(&job->lock);
    job->total = total;
    job->done = 0;
    job->determinate = TRUE;
    job->start_time = g_get_monotonic_time();
    g_mutex_unlock(&job->lock);
}

void job_set_total_items(Job *job, gint64 n)
{
    job_set_total(job, n);
    g_mutex_lock(&job->lock);
    job->count_items = TRUE;
    g_mutex_unlock(&job->lock);
}

void job_add_done(Job *job, gint64 bytes)
{
    g_mutex_lock(&job->lock);
    job->done += bytes;
    g_mutex_unlock(&job->lock);
}

gboolean job_cancelled(Job *job)
{
    return fs_cancelled(&job->ctx);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Dialog
 * ─────────────────────────────────────────────────────────────────── */

static void on_job_cancel(GtkButton *b, Job *job)
{
    fs_cancel(&job->ctx);
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_label_set_text(GTK_LABEL(job->phase_lbl), "Cancelling…");
}

static gboolean on_job_close(GtkWindow *w, Job *job)
{
    (void)w;
    fs_cancel(&job->ctx);
    return TRUE;          /* window is destroyed by job_free */
}

static void set_bar(GtkWidget *bar, double frac)
{
    frac = CLAMP(frac, 0.0, 1.0);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), frac);
    gchar *pct = g_strdup_printf("%.0f %%", frac * 100.0);
    gtk_progress_bar_set_text(GTK_PROGRESS_BAR(bar), pct);
    g_free(pct);
}

static gboolean job_tick(gpointer d)
{
    Job *job = d;
    g_mutex_lock(&job->lock);
    gchar   *phase = g_strdup(job->phase);
    gchar   *from  = g_strdup(job->from ? job->from : job->file);
    gchar   *to    = g_strdup(job->to);
    gint64   ft = job->file_total, fd = job->file_done;
    gint64   total = job->total, done = job->done, t0 = job->start_time;
    gboolean det = job->determinate, items = job->count_items;
    g_mutex_unlock(&job->lock);

    if (!fs_cancelled(&job->ctx) && phase)
        gtk_label_set_text(GTK_LABEL(job->phase_lbl), phase);
    gtk_label_set_text(GTK_LABEL(job->from_lbl), from ? from : "");
    gtk_label_set_text(GTK_LABEL(job->to_lbl), to ? to : "");
    gtk_widget_set_visible(job->to_key, to != NULL);
    gtk_widget_set_visible(job->to_lbl, to != NULL);

    /* per-file bar only for byte transfers of non-empty files */
    gboolean show_file = det && !items && to && ft > 0;
    gtk_widget_set_visible(job->file_bar, show_file);
    if (show_file) set_bar(job->file_bar, (double)fd / (double)ft);

    gchar *detail = NULL;
    if (!det) {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(job->bar));
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(job->bar), NULL);
        int n = g_atomic_int_get(&job->ctx.files_done);
        if (n > 0) detail = g_strdup_printf("%d files", n);
    } else if (items) {
        set_bar(job->bar, total > 0 ? (double)done / (double)total : 0.0);
        detail = g_strdup_printf("%" G_GINT64_FORMAT " / %" G_GINT64_FORMAT " items",
                                 done, total);
    } else {
        set_bar(job->bar, total > 0 ? (double)done / (double)total : 0.0);
        gchar *s_done  = fmt_size(done);
        gchar *s_total = fmt_size(total);
        gint64 elapsed = g_get_monotonic_time() - t0;
        if (elapsed > 500000 && done > 0) {
            double speed = (double)done / (elapsed / 1e6);
            gchar *s_speed = fmt_size((goffset)speed);
            gint64 left = speed > 0 ? (gint64)((total - done) / speed) : 0;
            detail = g_strdup_printf("%s / %s     %s/s     %02d:%02d left",
                                     s_done, s_total, s_speed,
                                     (int)(left / 60), (int)(left % 60));
            g_free(s_speed);
        } else {
            detail = g_strdup_printf("%s / %s", s_done, s_total);
        }
        g_free(s_done); g_free(s_total);
    }
    gtk_label_set_text(GTK_LABEL(job->detail_lbl), detail ? detail : "");
    g_free(detail);
    g_free(phase); g_free(from); g_free(to);
    return G_SOURCE_CONTINUE;
}

static gboolean on_job_key(GtkEventControllerKey *k, guint keyval, guint code,
                           GdkModifierType state, Job *job)
{
    (void)k; (void)code; (void)state;
    if (keyval == GDK_KEY_Escape) {
        fs_cancel(&job->ctx);
        gtk_label_set_text(GTK_LABEL(job->phase_lbl), "Cancelling…");
        return TRUE;
    }
    return FALSE;
}

static GtkWidget *path_label(void)
{
    GtkWidget *l = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_width_chars(GTK_LABEL(l), 50);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 60);
    gtk_widget_set_hexpand(l, TRUE);
    return l;
}

Job *job_new(FM *fm, const char *title)
{
    Job *job = g_new0(Job, 1);
    job->fm = fm;
    g_mutex_init(&job->lock);
    job->phase = g_strdup(title);
    job->start_time = g_get_monotonic_time();
    job->ctx.user = job;
    job->ctx.progress   = job_progress_cb;
    job->ctx.conflict   = job_conflict_cb;
    job->ctx.file_start = job_file_start_cb;
    job->ctx.policy = FS_ASK;

    job->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(job->window), title);
    gtk_window_set_modal(GTK_WINDOW(job->window), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(job->window), GTK_WINDOW(fm->window));
    gtk_window_set_resizable(GTK_WINDOW(job->window), FALSE);
    gtk_window_set_default_size(GTK_WINDOW(job->window), 520, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    job->phase_lbl = gtk_label_new(title);
    gtk_widget_set_halign(job->phase_lbl, GTK_ALIGN_START);
    gtk_widget_add_css_class(job->phase_lbl, "heading");

    /* From: / To: rows */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 2);
    job->from_key = gtk_label_new("From:");
    job->to_key   = gtk_label_new("To:");
    gtk_label_set_xalign(GTK_LABEL(job->from_key), 1.0f);
    gtk_label_set_xalign(GTK_LABEL(job->to_key), 1.0f);
    gtk_widget_add_css_class(job->from_key, "dim-label");
    gtk_widget_add_css_class(job->to_key, "dim-label");
    job->from_lbl = path_label();
    job->to_lbl   = path_label();
    gtk_grid_attach(GTK_GRID(grid), job->from_key, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), job->from_lbl, 1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), job->to_key,   0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), job->to_lbl,   1, 1, 1, 1);
    gtk_widget_set_visible(job->to_key, FALSE);
    gtk_widget_set_visible(job->to_lbl, FALSE);

    /* current file bar + total bar */
    job->file_bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(job->file_bar), TRUE);
    gtk_widget_set_visible(job->file_bar, FALSE);

    job->bar = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(job->bar), TRUE);
    gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(job->bar), 0.03);

    job->detail_lbl = gtk_label_new("");
    gtk_widget_set_halign(job->detail_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(job->detail_lbl), PANGO_ELLIPSIZE_END);

    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_halign(cancel_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(cancel_btn, 100, -1);
    gtk_widget_set_margin_top(cancel_btn, 4);
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_job_cancel), job);

    gtk_box_append(GTK_BOX(vbox), job->phase_lbl);
    gtk_box_append(GTK_BOX(vbox), grid);
    gtk_box_append(GTK_BOX(vbox), job->file_bar);
    gtk_box_append(GTK_BOX(vbox), job->bar);
    gtk_box_append(GTK_BOX(vbox), job->detail_lbl);
    gtk_box_append(GTK_BOX(vbox), cancel_btn);
    gtk_window_set_child(GTK_WINDOW(job->window), vbox);
    g_signal_connect(job->window, "close-request", G_CALLBACK(on_job_close), job);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_job_key), job);
    gtk_widget_add_controller(job->window, key);
    return job;
}

static gboolean job_finished_idle(gpointer d)
{
    Job *job = d;
    g_main_loop_quit(job->loop);
    return G_SOURCE_REMOVE;
}

static gpointer job_thread(gpointer d)
{
    Job *job = d;
    job->fn(job, job->data);
    g_idle_add(job_finished_idle, job);
    return NULL;
}

void job_run(Job *job, JobFunc fn, gpointer data)
{
    job->fn   = fn;
    job->data = data;
    job->loop = g_main_loop_new(NULL, FALSE);

    /* Modal: the panels cannot start a second operation meanwhile */
    gtk_window_present(GTK_WINDOW(job->window));
    job->timer_id = g_timeout_add(100, job_tick, job);

    GThread *t = g_thread_new("fm-job", job_thread, job);
    g_main_loop_run(job->loop);
    g_thread_join(t);

    g_source_remove(job->timer_id);
    job->timer_id = 0;
    g_main_loop_unref(job->loop);
    job->loop = NULL;
}

void job_free(Job *job)
{
    if (!job) return;
    if (job->timer_id) g_source_remove(job->timer_id);
    if (job->window) {
        gtk_window_destroy(GTK_WINDOW(job->window));
        fm_refocus_if_lost(job->fm);
    }
    fs_ctx_clear(&job->ctx);
    g_free(job->phase);
    g_free(job->file);
    g_free(job->from);
    g_free(job->to);
    g_mutex_clear(&job->lock);
    g_free(job);
}
