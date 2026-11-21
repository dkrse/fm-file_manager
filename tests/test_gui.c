/* ─────────────────────────────────────────────────────────────────── *
 *  GUI integration tests: panel tabs (local + SFTP), connection
 *  ref-counting, auto-refresh polling, disconnect / dead connection,
 *  device picker.
 *
 *  The real app sources are compiled in (statics reachable).  Runs
 *  headless on broadway, started by `make test-gui`; SFTP parts need
 *  the throwaway sshd of tests/sftp_test.sh (FM_SFTP_PORT, FM_SFTP_KEY,
 *  FM_SFTP_DIR, FM_SFTP_HOSTKEY) and are skipped without it.
 *  Build with SAN=1 to catch leaks / use-after-free.
 * ─────────────────────────────────────────────────────────────────── */

#define main fm_app_main
#include "../src/main.c"
#undef main
#include "../src/ssh.c"
#include "../src/devices.c"
#include "../src/quickview.c"
#include "../src/tabs.c"
#include "../src/monitor.c"
#include "../src/cmdline.c"
#include "../src/editor.c"
#include "../src/compare.c"
#include "../src/multirename.c"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <signal.h>
#include <glib/gstdio.h>
#include <utime.h>
#include <math.h>
#include "../include/job.h"

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    g_printerr("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static gchar *tmp;              /* local scratch (also $HOME) */

/* ── helpers ──────────────────────────────────────────────────────── */

static void pump(int ms)
{
    gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
    while (g_get_monotonic_time() < end)
        if (!g_main_context_iteration(NULL, FALSE)) g_usleep(5000);
}

static gboolean store_has(Panel *p, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
        gboolean eq = strcmp(it->name, name) == 0;
        g_object_unref(it);
        if (eq) return TRUE;
    }
    return FALSE;
}

static gboolean wait_has(Panel *p, const char *name, int sec)
{
    gint64 end = g_get_monotonic_time() + (gint64)sec * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end) {
        if (store_has(p, name)) return TRUE;
        pump(50);
    }
    return FALSE;
}

static PanelTab *tab_at(Panel *p, guint i) { return g_ptr_array_index(p->tabs, i); }

static double now_s(void) { return g_get_monotonic_time() / 1e6; }

static void mk(const char *dir, const char *name)
{
    gchar *path = g_build_filename(dir, name, NULL);
    g_mkdir_with_parents(path, 0755);
    g_free(path);
}

static void touch(const char *dir, const char *name)
{
    gchar *path = g_build_filename(dir, name, NULL);
    g_file_set_contents(path, "x", 1, NULL);
    g_free(path);
}

static SshConn *connect_test(void)
{
    gchar *err = NULL;
    SshConn *c = ssh_conn_new("127.0.0.1", g_get_user_name(),
                              atoi(g_getenv("FM_SFTP_PORT")), "",
                              g_getenv("FM_SFTP_KEY"), &err);
    if (!c) g_printerr("connect: %s\n", err ? err : "?");
    g_free(err);
    return c;
}

/* ── local tabs ───────────────────────────────────────────────────── */

static void test_local_tabs(FM *fm)
{
    Panel *p = &fm->panels[0];
    gchar *a = g_build_filename(tmp, "a", NULL);
    mk(tmp, "a"); mk(tmp, "b");
    panel_load(p, a);
    CHECK(strcmp(p->cwd, a) == 0);

    for (int i = 0; i < 50; i++) panel_tab_new(p, NULL);
    CHECK(p->tabs->len == 51);
    for (int i = 0; i < 120; i++) panel_tab_cycle(p, i % 3 ? 1 : -2);
    while (p->tabs->len > 1) panel_tab_close(p);
    CHECK(p->tabs->len == 1);
    CHECK(strcmp(p->cwd, a) == 0);
    CHECK(p->ssh_conn == NULL);
    g_free(a);
    g_print("ok local tabs\n");
}

/* ── SFTP tabs ────────────────────────────────────────────────────── */

static void test_sftp_tabs(FM *fm, const char *work)
{
    Panel *p = &fm->panels[0];
    gchar *local = g_build_filename(tmp, "a", NULL);
    panel_load(p, local);

    gchar *rdir = g_build_filename(work, "gui", NULL);
    mk(work, "gui"); mk(rdir, "sub"); touch(rdir, "f1");

    SshConn *c = connect_test();
    CHECK(c != NULL);
    if (!c) { g_free(rdir); g_free(local); return; }
    CHECK(c->refs == 0);

    /* connect into the active tab */
    panel_tab_set_conn(p, c);
    panel_load_remote(p, rdir);
    CHECK(p->ssh_conn == c && c->refs == 1);
    CHECK(store_has(p, "f1") && store_has(p, "sub"));
    CHECK(g_str_has_prefix(gtk_editable_get_text(GTK_EDITABLE(p->path_entry)), "sftp://"));
    CHECK(g_strcmp0(panel_tab_cur(p)->rpath, rdir) == 0);
    CHECK(strcmp(p->cwd, local) == 0);          /* local fallback kept */

    /* Ctrl+T shares the connection */
    panel_tab_new(p, NULL);
    CHECK(p->tabs->len == 2 && c->refs == 2 && panel_tab_cur(p)->conn == c);

    /* Ctrl+Up equivalent: remote subdirectory in a new tab */
    gchar *sub = g_build_filename(rdir, "sub", NULL);
    panel_tab_new(p, sub);
    CHECK(p->tabs->len == 3 && c->refs == 3);
    CHECK(g_strcmp0(c->remote_path, sub) == 0);

    /* a local tab in between */
    panel_tab_new(p, NULL);
    panel_tab_set_conn(p, NULL);
    CHECK(p->ssh_conn == NULL && c->refs == 3 && panel_tab_cur(p)->conn == NULL);
    CHECK(strcmp(p->cwd, local) == 0);

    /* switching: each tab its own directory, never blocks */
    double worst = 0;
    for (int i = 0; i < 40; i++) {
        double t0 = now_s();
        panel_tab_cycle(p, 1);
        worst = MAX(worst, now_s() - t0);
        PanelTab *t = panel_tab_cur(p);
        CHECK(p->ssh_conn == t->conn);
        if (t->conn) CHECK(g_strcmp0(c->remote_path, t->rpath) == 0);
        else         CHECK(strcmp(p->cwd, local) == 0);
        CHECK(gtk_widget_get_visible(p->disconnect_btn) == (t->conn != NULL));
    }
    CHECK(worst < 2.0);
    g_print("  tab switch worst %.3f s\n", worst);
    CHECK(tab_at(p, 0)->conn == c && g_strcmp0(tab_at(p, 0)->rpath, rdir) == 0);
    CHECK(tab_at(p, 2)->conn == c && g_strcmp0(tab_at(p, 2)->rpath, sub) == 0);

    /* auto-refresh with a 1 s interval on the sub tab */
    fm->auto_refresh = TRUE;
    fm->sftp_poll_interval = 1;
    panel_tab_cycle(p, 2 - (int)p->tab_active);
    CHECK(p->tab_active == 2 && p->poll_id != 0);
    double t0 = now_s();
    touch(sub, "appeared");
    CHECK(wait_has(p, "appeared", 5));
    g_print("  new remote file shown after %.2f s\n", now_s() - t0);
    gchar *gone = g_build_filename(sub, "appeared", NULL);
    g_unlink(gone);
    g_free(gone);
    pump(2500);
    CHECK(!store_has(p, "appeared"));

    /* closing a shared tab only drops a reference */
    panel_tab_close(p);
    CHECK(p->tabs->len == 3 && c->refs == 2);

    /* Disconnect: all tabs of the connection go local */
    ssh_conn_ref(c);                        /* keep it to inspect */
    panel_tab_cycle(p, -(int)p->tab_active);
    CHECK(p->ssh_conn == c);
    ssh_panel_close(p);
    CHECK(c->refs == 1);
    CHECK(p->ssh_conn == NULL);
    for (guint i = 0; i < p->tabs->len; i++)
        CHECK(tab_at(p, i)->conn == NULL && tab_at(p, i)->rpath == NULL);
    CHECK(!gtk_widget_get_visible(p->disconnect_btn));
    CHECK(strcmp(p->cwd, local) == 0);
    ssh_conn_unref(c);                      /* frees it (ASan checks) */

    /* dead connection: switching to its tab must not hang */
    c = connect_test();
    CHECK(c != NULL);
    if (c) {
        panel_tab_set_conn(p, c);
        panel_load_remote(p, rdir);
        panel_tab_new(p, NULL);
        panel_tab_set_conn(p, NULL);         /* local tab */
        shutdown(c->sock, SHUT_RDWR);        /* server "gone" */
        t0 = now_s();
        panel_tab_cycle(p, -1);
        double dt = now_s() - t0;
        g_print("  dead connection detected in %.3f s\n", dt);
        CHECK(dt < 5.0);
        CHECK(p->ssh_conn == NULL);
        for (guint i = 0; i < p->tabs->len; i++) CHECK(tab_at(p, i)->conn == NULL);
    }

    /* stress: connect / share / close / disconnect cycles */
    fm->auto_refresh = FALSE;
    for (int round = 0; round < 15; round++) {
        SshConn *s = connect_test();
        if (!s) { CHECK(s != NULL); break; }
        panel_tab_set_conn(p, s);
        panel_load_remote(p, rdir);
        for (int i = 0; i < 4; i++) panel_tab_new(p, NULL);
        panel_tab_cycle(p, 2);
        panel_tab_close(p);
        if (round % 2) ssh_panel_close(p);
        else while (p->tabs->len > 1) panel_tab_close(p);
        if (p->ssh_conn) ssh_panel_close(p);
        CHECK(p->ssh_conn == NULL);
    }
    while (p->tabs->len > 1) panel_tab_close(p);

    /* a connection left in a tab is released at exit (panel_tabs_free) */
    c = connect_test();
    if (c) { panel_tab_set_conn(p, c); panel_load_remote(p, rdir); }

    g_free(sub);
    g_free(rdir);
    g_free(local);
    g_print("ok sftp tabs\n");
}

/* ── devices ──────────────────────────────────────────────────────── */

static void test_devices(FM *fm)
{
    CHECK(path_under("/media/x/y", "/media/x"));
    CHECK(path_under("/media/x", "/media/x"));
    CHECK(!path_under("/media/xy", "/media/x"));
    CHECK(!path_under("/home", "/"));
    CHECK(!path_under(NULL, "/a"));

    Panel *p = &fm->panels[1];
    for (int i = 0; i < 20; i++) devices_fill(p);
    int rows = 0;
    while (gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->dev_list), rows)) rows++;
    CHECK(rows >= 2);
    GtkListBoxRow *r0 = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->dev_list), 0);
    CHECK(g_strcmp0(g_object_get_data(G_OBJECT(r0), "fm-path"), "/") == 0);

    /* activating "Home" opens it */
    GtkListBoxRow *r1 = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->dev_list), 1);
    on_row_activated(GTK_LIST_BOX(p->dev_list), r1, p);
    CHECK(strcmp(p->cwd, g_get_home_dir()) == 0);

    /* a vanishing device moves panels and tabs out of it */
    gchar *b = g_build_filename(tmp, "b", NULL);
    panel_load(p, b);
    panel_tab_new(p, NULL);
    panels_leave(fm, b);
    CHECK(strcmp(p->cwd, g_get_home_dir()) == 0);
    for (guint i = 0; i < p->tabs->len; i++)
        CHECK(strcmp(tab_at(p, i)->path, g_get_home_dir()) == 0);
    while (p->tabs->len > 1) panel_tab_close(p);

    /* popup / popdown cycles */
    for (int i = 0; i < 5; i++) { devices_popup(p); pump(30); popdown(p); pump(30); }
    g_free(b);
    g_print("ok devices (%d rows)\n", rows);
}


/* ── compare directories, quick view, background queue ───────────── */

static void set_mtime(const char *dir, const char *name, gint64 t)
{
    gchar *path = g_build_filename(dir, name, NULL);
    struct utimbuf ub = { (time_t)t, (time_t)t };
    utime(path, &ub);
    g_free(path);
}

static void write_in(const char *dir, const char *name, const char *data)
{
    gchar *path = g_build_filename(dir, name, NULL);
    g_file_set_contents(path, data, -1, NULL);
    g_free(path);
}

static gboolean marked(Panel *p, const char *name)
{
    return p->marks && g_hash_table_contains(p->marks, name);
}

/* the SFTP test may leave a panel connected – these tests are local */
static void local_panels(FM *fm)
{
    for (int i = 0; i < 2; i++)
        while (fm->panels[i].ssh_conn) { ssh_panel_close(&fm->panels[i]); pump(50); }
}

static void test_compare_dirs(FM *fm)
{
    local_panels(fm);
    gchar *a = g_build_filename(tmp, "cmpA", NULL), *b = g_build_filename(tmp, "cmpB", NULL);
    g_mkdir_with_parents(a, 0755);
    g_mkdir_with_parents(b, 0755);
    write_in(a, "same.txt", "1");   write_in(b, "same.txt", "1");
    write_in(a, "newer.txt", "2");  write_in(b, "newer.txt", "2");
    write_in(a, "only_a.txt", "3");
    write_in(b, "only_b.txt", "4");
    write_in(a, "size.txt", "55");  write_in(b, "size.txt", "5");
    mk(a, "subdir");                         /* directories are not compared */
    set_mtime(a, "same.txt", 1000000);  set_mtime(b, "same.txt", 1000001);
    set_mtime(a, "newer.txt", 2000000); set_mtime(b, "newer.txt", 1000000);
    set_mtime(a, "size.txt", 1000000);  set_mtime(b, "size.txt", 1000000);

    panel_load(&fm->panels[0], a);
    panel_load(&fm->panels[1], b);
    pump(100);
    guint l = 0, r = 0, d = 0;
    CHECK(cmp_mark_dirs(fm, &l, &r, &d));
    CHECK(l == 2 && r == 1 && d == 1);
    Panel *pl = &fm->panels[0], *pr = &fm->panels[1];
    CHECK(marked(pl, "newer.txt") && marked(pl, "only_a.txt"));
    CHECK(!marked(pl, "same.txt") && !marked(pl, "size.txt") && !marked(pl, "subdir"));
    CHECK(marked(pr, "only_b.txt") && !marked(pr, "newer.txt"));

    /* synchronize left → right: the marked files are copied */
    GList *paths = panel_selection_paths(pl);
    CHECK(g_list_length(paths) == 2);
    gchar *dst = g_build_filename(b, "only_a.txt", NULL);
    /* newer.txt exists on the right → the conflict prompt would block;
     * copy only the missing file here */
    GList *only = NULL;
    for (GList *x = paths; x; x = x->next)
        if (g_str_has_suffix(x->data, "only_a.txt")) only = g_list_append(only, x->data);
    CHECK(fo_transfer(fm, only, NULL, b, NULL, FALSE) == FS_OK);
    CHECK(g_file_test(dst, G_FILE_TEST_IS_REGULAR));
    g_list_free(only);
    g_list_free_full(paths, g_free);
    g_free(dst);
    sel_mark_all(pl, FALSE);
    sel_mark_all(pr, FALSE);
    g_print("ok compare dirs\n");
    g_free(a);
    g_free(b);
}

static void test_quickview(FM *fm)
{
    local_panels(fm);
    gchar *d = g_build_filename(tmp, "qv", NULL);
    g_mkdir_with_parents(d, 0755);
    write_in(d, "a.txt", "hello quick view\n");
    Panel *p = &fm->panels[0];
    panel_load(p, d);
    pump(100);
    panel_focus(p);
    panel_select_name(p, "a.txt");
    qv_toggle(fm);
    pump(300);
    Panel *q = &fm->panels[1];
    CHECK(qv_active(fm) && fm->qv_panel == 1);
    CHECK(q->qv_box && gtk_widget_get_visible(q->qv_box));
    CHECK(!gtk_widget_get_visible(q->list_scroll));
    GtkTextBuffer *buf = gtk_text_view_get_buffer(
        GTK_TEXT_VIEW(g_object_get_data(G_OBJECT(q->qv_box), "text")));
    GtkTextIter s, e;
    gtk_text_buffer_get_bounds(buf, &s, &e);
    gchar *txt = gtk_text_buffer_get_text(buf, &s, &e, FALSE);
    CHECK(strstr(txt, "hello quick view") != NULL);
    g_free(txt);
    qv_toggle(fm);
    pump(50);
    CHECK(!qv_active(fm) && gtk_widget_get_visible(q->list_scroll));
    g_print("ok quick view\n");
    g_free(d);
}

static gint64 job1_end, job2_start;

static void slow_worker(Job *job, gpointer d)
{
    (void)job; (void)d;
    g_usleep(400000);
    job1_end = g_get_monotonic_time();
}

static void quick_worker(Job *job, gpointer d)
{
    (void)job; (void)d;
    job2_start = g_get_monotonic_time();
}

static gboolean start_second(gpointer fm)
{
    CHECK(job_running_count() == 1);
    Job *j2 = job_new(fm, "Second");
    job_run(j2, quick_worker, NULL);       /* queued behind job 1 */
    job_free(j2);
    return G_SOURCE_REMOVE;
}

static gboolean press_background(gpointer job)
{
    Job *j = job;
    CHECK(gtk_widget_get_visible(j->bg_btn));
    g_signal_emit_by_name(j->bg_btn, "clicked");
    CHECK(j->background && !gtk_widget_get_visible(j->window));
    g_timeout_add(50, start_second, j->fm);
    return G_SOURCE_REMOVE;
}

static void test_background_queue(FM *fm)
{
    Job *j1 = job_new(fm, "First");
    j1->allow_background = TRUE;
    g_timeout_add(50, press_background, j1);
    job_run(j1, slow_worker, NULL);
    job_free(j1);
    CHECK(job1_end > 0 && job2_start >= job1_end);
    CHECK(job_running_count() == 0);
    g_print("ok background queue\n");
}


/* ── SFTP: editor save in a worker, ssh_exec with a flood of stderr ── */

static void test_sftp_save_exec(FM *fm, const char *work)
{
    SshConn *c = connect_test();
    CHECK(c != NULL);
    if (!c) return;
    ssh_conn_ref(c);            /* a tab owns it in the app (refs ≥ 1) */
    gchar *rdir = g_build_filename(work, "save", NULL);
    mk(work, "save");
    gchar *rf = g_build_filename(rdir, "s.txt", NULL);

    CHECK(fm_write_remote(fm, c, rf, "saved remotely\n", 15) == 1);
    gchar *got = NULL;
    CHECK(g_file_get_contents(rf, &got, NULL, NULL) && g_strcmp0(got, "saved remotely\n") == 0);
    g_free(got);
    gchar *bad = g_build_filename(rdir, "no", "such", "dir", "x", NULL);
    CHECK(fm_write_remote(fm, c, bad, "x", 1) == 0);      /* error, no hang */
    g_free(bad);

    /* 3 MB on stderr: more than the channel window – reading stdout to
     * EOF first used to block until the 60 s timeout */
    gchar *out = NULL, *err = NULL;
    double t0 = now_s();
    int code = ssh_exec(c, rdir,
        "head -c 3000000 /dev/zero | tr '\\0' e >&2; echo done", &out, &err);
    CHECK(now_s() - t0 < 20.0);
    CHECK(code == 0);
    CHECK(g_strcmp0(out, "done\n") == 0);
    CHECK(err && strlen(err) >= 1024 * 1024 && err[0] == 'e');   /* capped at 1 MB */
    g_free(out); g_free(err);

    /* plain stdout + exit code still come through */
    CHECK(ssh_exec(c, rdir, "printf abc; echo oops >&2; exit 3", &out, &err) == 3);
    CHECK(g_strcmp0(out, "abc") == 0 && g_strcmp0(err, "oops\n") == 0);
    g_free(out); g_free(err);

    ssh_conn_unref(c);
    g_free(rf); g_free(rdir);
    g_print("ok sftp save + exec\n");
}

/* ── directory monitor batches ─────────────────────────────────────── */

static gboolean has_marked_item(Panel *p, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
        gboolean hit = strcmp(it->name, name) == 0 && it->marked;
        g_object_unref(it);
        if (hit) return TRUE;
    }
    return FALSE;
}

static gint64 item_size(Panel *p, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
        gint64 sz = strcmp(it->name, name) == 0 ? it->size_raw : -1;
        g_object_unref(it);
        if (sz >= 0) return sz;
    }
    return -1;
}

static guint store_count(Panel *p)
{
    return g_list_model_get_n_items(G_LIST_MODEL(p->store));
}

static void test_monitor(FM *fm)
{
    local_panels(fm);
    gchar *d = g_build_filename(tmp, "mon", NULL);
    g_mkdir_with_parents(d, 0755);
    write_in(d, "keep.txt", "1");
    write_in(d, "gone.txt", "1");
    write_in(d, "grow.txt", "1");
    gboolean prev_ar = fm->auto_refresh;
    fm->auto_refresh = TRUE;            /* off in the default test settings */
    Panel *p = &fm->panels[0];
    panel_load(p, d);
    pump(100);
    guint base = store_count(p);
    sel_mark_all(p, FALSE);
    panel_select_name(p, "grow.txt");
    sel_toggle_cursor(p, FALSE);
    CHECK(has_marked_item(p, "grow.txt"));

    /* one small batch: add, delete, modify together */
    write_in(d, "new1.txt", "1");
    write_in(d, "new2.txt", "1");
    gchar *g = g_build_filename(d, "gone.txt", NULL);
    g_unlink(g);
    g_free(g);
    write_in(d, "grow.txt", "123456");
    gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end &&
           !(store_has(p, "new2.txt") && !store_has(p, "gone.txt") &&
             item_size(p, "grow.txt") == 6))
        pump(50);
    CHECK(store_has(p, "new1.txt") && store_has(p, "new2.txt"));
    CHECK(!store_has(p, "gone.txt") && store_has(p, "keep.txt"));
    CHECK(item_size(p, "grow.txt") == 6);
    CHECK(has_marked_item(p, "grow.txt"));          /* mark kept across refresh */
    CHECK(store_count(p) == base + 1);              /* +2 −1, no duplicates */

    /* overflow (> MON_RELOAD_LIMIT names) → one reload, then backoff */
    for (int i = 0; i < 100; i++) {
        gchar n[32];
        g_snprintf(n, sizeof n, "bulk%03d", i);
        write_in(d, n, "x");
    }
    end = g_get_monotonic_time() + 8 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end && !store_has(p, "bulk099")) pump(50);
    CHECK(store_has(p, "bulk000") && store_has(p, "bulk099"));
    CHECK(store_count(p) == base + 1 + 100);
    CHECK(p->mon_reload_at != 0);
    write_in(d, "after.txt", "x");                  /* still picked up later */
    CHECK(wait_has(p, "after.txt", 5));
    sel_mark_all(p, FALSE);
    fm->auto_refresh = prev_ar;
    panel_monitor_start(p);
    g_print("ok monitor batches\n");
    g_free(d);
}

/* ── checksum verify: the stat of every listed file runs in the job ── */

static void test_cks_verify(FM *fm)
{
    local_panels(fm);
    gchar *d = g_build_filename(tmp, "cks", NULL);
    g_mkdir_with_parents(d, 0755);
    write_in(d, "a.txt", "abc");
    /* sha256("abc") */
    write_in(d, "sums.sha256",
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  a.txt\n"
             "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  missing.txt\n");
    Panel *p = &fm->panels[0];
    panel_load(p, d);
    pump(100);
    panel_focus(p);
    panel_select_name(p, "sums.sha256");
    cks_verify(fm);
    pump(100);
    const char *st = gtk_label_get_text(GTK_LABEL(fm->statusbar));
    CHECK(g_strcmp0(st, "Verify: 1 OK, 0 failed, 1 missing") == 0);
    if (g_strcmp0(st, "Verify: 1 OK, 0 failed, 1 missing")) g_printerr("status: %s\n", st);
    /* close the result window */
    GListModel *tl = gtk_window_get_toplevels();
    for (guint i = g_list_model_get_n_items(tl); i > 0; i--) {
        GtkWindow *w = g_list_model_get_item(tl, i - 1);
        if (w != GTK_WINDOW(fm->window) &&
            g_strcmp0(gtk_window_get_title(w), "Verify checksums") == 0)
            gtk_window_destroy(w);
        g_object_unref(w);
    }
    pump(50);
    g_print("ok checksum verify\n");
    g_free(d);
}

/* ── quick view images: decoded in a worker, stale results dropped ── */

static void write_png(const char *path, int w, int h)
{
    GdkPixbuf *pb = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    gdk_pixbuf_fill(pb, 0x3366ccff);
    gdk_pixbuf_save(pb, path, "png", NULL, NULL);
    g_object_unref(pb);
}

static void test_quickview_image(FM *fm)
{
    local_panels(fm);
    gchar *d = g_build_filename(tmp, "qvimg", NULL);
    g_mkdir_with_parents(d, 0755);
    gchar *small = g_build_filename(d, "a_small.png", NULL);
    gchar *big   = g_build_filename(d, "b_big.png", NULL);
    write_png(small, 40, 30);
    write_png(big, 4000, 3000);
    write_in(d, "c.txt", "text after the image\n");
    gboolean prev = fm->viewer_image_preview;
    fm->viewer_image_preview = TRUE;

    Panel *p = &fm->panels[0];
    panel_load(p, d);
    pump(100);
    panel_focus(p);
    panel_select_name(p, "a_small.png");
    qv_toggle(fm);
    Panel *q = &fm->panels[1];
    GtkPicture *pic = g_object_get_data(G_OBJECT(q->qv_box), "pic");
    gint64 end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end && !gtk_picture_get_paintable(pic)) pump(30);
    GdkPaintable *pt = gtk_picture_get_paintable(pic);
    CHECK(pt != NULL);
    if (pt) CHECK(gdk_paintable_get_intrinsic_width(pt) == 40);

    /* big image is downscaled to QV_IMG_MAX_PX */
    panel_select_name(p, "b_big.png");
    end = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (g_get_monotonic_time() < end &&
           !((pt = gtk_picture_get_paintable(pic)) &&
             gdk_paintable_get_intrinsic_width(pt) != 40))
        pump(30);
    CHECK(pt && gdk_paintable_get_intrinsic_width(pt) == QV_IMG_MAX_PX);
    if (pt) CHECK(gdk_paintable_get_intrinsic_height(pt) == 1536);

    /* leave an image while it is still decoding: the late result must
     * not replace the text of the file now under the cursor */
    panel_select_name(p, "a_small.png");
    pump(QV_POLL_MS + 50);
    panel_select_name(p, "c.txt");
    pump(QV_POLL_MS * 3);
    pump(500);
    CHECK(!gtk_widget_get_visible(GTK_WIDGET(pic)));
    CHECK(gtk_picture_get_paintable(pic) == NULL);
    CHECK(!qv_img_busy && qv_img_next == NULL);

    qv_toggle(fm);
    pump(50);
    fm->viewer_image_preview = prev;
    g_print("ok quick view images\n");
    g_free(small); g_free(big); g_free(d);
}


/* ── closing F3/F4 must not scroll the list when the cursor row is
 * already on screen.  Headless broadway has no frame clock, so
 * scroll_to itself never runs here: the visibility test that decides
 * whether panel_focus may scroll is checked on a real adjustment. ── */

static void test_focus_keeps_scroll(FM *fm)
{
    local_panels(fm);
    gchar *d = g_build_filename(tmp, "many", NULL);
    g_mkdir_with_parents(d, 0755);
    for (int i = 0; i < 400; i++) {
        gchar n[32];
        g_snprintf(n, sizeof n, "f%03d.txt", i);
        write_in(d, n, "x");
    }
    Panel *p = &fm->panels[0];
    panel_load(p, d);
    panel_focus(p);
    pump(200);
    GtkAdjustment *va = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(p->column_view));
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (int i = 0; i < 100 && gtk_adjustment_get_upper(va) <= gtk_adjustment_get_page_size(va); i++)
        pump(50);                           /* wait for the first layout */
    if (gtk_adjustment_get_upper(va) <= gtk_adjustment_get_page_size(va))
        g_printerr("layout: n=%u upper=%.0f page=%.0f\n", n,
                   gtk_adjustment_get_upper(va), gtk_adjustment_get_page_size(va));
    double upper = gtk_adjustment_get_upper(va), page = gtk_adjustment_get_page_size(va);
    CHECK(n == 401 && upper > page);           /* 400 files + ".." */
    double h = upper / n;
    guint rows = (guint)(page / h);
    gtk_adjustment_set_value(va, 200 * h);     /* top row = 200 */
    double v0 = gtk_adjustment_get_value(va);
    guint top = (guint)(v0 / h + 0.5);
    CHECK(panel_row_visible(p, top, n));                 /* first shown row */
    CHECK(panel_row_visible(p, top + rows / 2, n));      /* middle */
    CHECK(panel_row_visible(p, top + rows - 1, n));      /* last full row */
    CHECK(!panel_row_visible(p, top - 1, n));            /* just above */
    CHECK(!panel_row_visible(p, top + rows + 1, n));     /* just below */
    CHECK(!panel_row_visible(p, 5, n));

    /* focus with a visible cursor row: selected, view untouched */
    p->cursor_pos = top + rows / 2;
    panel_focus(p);
    pump(200);
    CHECK(fabs(gtk_adjustment_get_value(va) - v0) < 1.0);
    GtkBitset *bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->selection));
    CHECK(gtk_bitset_get_size(bs) == 1 && gtk_bitset_contains(bs, top + rows / 2));
    gtk_bitset_unref(bs);

    /* the window regaining focus after F3/F4 also runs these two:
     * same row stays selected, list stays put */
    restore_panel_cursor(fm);
    p->inhibit_sel = TRUE;
    on_focus_enter(NULL, p);
    pump(200);
    CHECK(!p->inhibit_sel);
    CHECK(fabs(gtk_adjustment_get_value(va) - v0) < 1.0);
    bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->selection));
    CHECK(gtk_bitset_get_size(bs) == 1 && gtk_bitset_contains(bs, top + rows / 2));
    gtk_bitset_unref(bs);
    CHECK(p->cursor_pos == top + rows / 2);

    /* F3/F4 round trip: offset saved before, something moves the list
     * meanwhile (focus handlers), closing puts it back exactly with the
     * cursor row selected – also after the delayed second pass */
    double saved = panel_view_save(p);
    CHECK(fabs(saved - v0) < 1.0);
    gtk_adjustment_set_value(va, 0);
    p->inhibit_sel = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->selection));
    p->inhibit_sel = FALSE;
    panel_view_restore(p, saved);
    CHECK(fabs(gtk_adjustment_get_value(va) - saved) < 1.0);
    gtk_adjustment_set_value(va, 0);        /* a late handler scrolls again */
    pump(300);                              /* > the 60 ms second pass */
    CHECK(fabs(gtk_adjustment_get_value(va) - saved) < 1.0);
    bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->selection));
    CHECK(gtk_bitset_get_size(bs) == 1 && gtk_bitset_contains(bs, top + rows / 2));
    gtk_bitset_unref(bs);

    /* the panel moved on meanwhile (other cursor): no forced old view */
    panel_view_restore(p, saved);
    p->cursor_pos = 3;
    gtk_adjustment_set_value(va, 0);
    pump(300);
    CHECK(gtk_adjustment_get_value(va) < 1.0);

    /* Enter / Open with → external app: fm active again restores the
     * view once, cursor row selected */
    p->cursor_pos = top + rows / 2;
    panel_view_restore(p, saved);
    pump(300);
    ext_view_remember(p);
    CHECK(ext_view.set && fabs(ext_view.value - saved) < 1.0);
    gtk_adjustment_set_value(va, 0);        /* focus return moved the list */
    p->inhibit_sel = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->selection));
    p->inhibit_sel = FALSE;
    ext_view_apply(fm);
    CHECK(!ext_view.set && ext_view.dir == NULL);
    CHECK(fabs(gtk_adjustment_get_value(va) - saved) < 1.0);
    gtk_adjustment_set_value(va, 0);
    pump(300);                              /* delayed second pass */
    CHECK(fabs(gtk_adjustment_get_value(va) - saved) < 1.0);
    bs = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(p->selection));
    CHECK(gtk_bitset_get_size(bs) == 1 && gtk_bitset_contains(bs, top + rows / 2));
    gtk_bitset_unref(bs);

    /* only once: a later activation (user scrolled meanwhile) leaves it */
    gtk_adjustment_set_value(va, 0);
    ext_view_apply(fm);
    pump(100);
    CHECK(gtk_adjustment_get_value(va) < 1.0);

    /* cursor moved while the app was open: no forced old view */
    ext_view_remember(p);
    p->cursor_pos = 3;
    gtk_adjustment_set_value(va, 0);
    ext_view_apply(fm);
    pump(300);
    CHECK(!ext_view.set);
    CHECK(gtk_adjustment_get_value(va) < 1.0);

    /* another directory meanwhile: likewise */
    p->cursor_pos = top + rows / 2;
    ext_view_remember(p);
    g_free(ext_view.dir);
    ext_view.dir = g_strdup("/elsewhere");
    ext_view_apply(fm);
    pump(300);
    CHECK(gtk_adjustment_get_value(va) < 1.0);

    /* app never took focus (launch failed / focus stealing prevented):
     * fm stays active, a later dialog's re-activation must not force
     * the old view – the first "active" without a "left" disarms it */
    p->cursor_pos = top + rows / 2;
    gtk_adjustment_set_value(va, saved);
    ext_view_remember(p);
    gtk_adjustment_set_value(va, 0);        /* user wheel-scrolled */
    ext_view_on_active(fm, TRUE);
    pump(300);
    CHECK(!ext_view.set);
    CHECK(gtk_adjustment_get_value(va) < 1.0);
    ext_view_on_active(fm, FALSE);          /* a dialog later … */
    ext_view_on_active(fm, TRUE);
    pump(300);
    CHECK(gtk_adjustment_get_value(va) < 1.0);

    /* the real round trip through the handler: left, then back */
    gtk_adjustment_set_value(va, saved);
    ext_view_remember(p);
    ext_view_on_active(fm, FALSE);
    gtk_adjustment_set_value(va, 0);
    ext_view_on_active(fm, TRUE);
    pump(300);
    CHECK(!ext_view.set);
    CHECK(fabs(gtk_adjustment_get_value(va) - saved) < 1.0);

    /* failed "Open with" launch forgets it */
    ext_view_remember(p);
    ext_view_forget();
    CHECK(!ext_view.set && ext_view.dir == NULL);

    g_print("ok focus keeps scroll\n");
    g_free(d);
}


/* ── middle click on a tab that is not shown: only that tab goes, the
 * visible one stays (it used to switch there, then close it) ───────── */

static void middle_click_tab(Panel *p, guint i)
{
    GtkWidget *b = gtk_button_new();
    g_object_ref_sink(b);
    g_object_set_data(G_OBJECT(b), "fm-tab-idx", GUINT_TO_POINTER(i));
    GtkGesture *g = gtk_gesture_click_new();
    gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(g));
    on_tab_middle(GTK_GESTURE_CLICK(g), 1, 0, 0, p);
    g_object_unref(b);
}

static void test_tab_middle_close(FM *fm)
{
    local_panels(fm);
    Panel *p = &fm->panels[0];
    gchar *a = g_build_filename(tmp, "ta", NULL);
    gchar *b = g_build_filename(tmp, "tb", NULL);
    gchar *c = g_build_filename(tmp, "tc", NULL);
    mk(tmp, "ta"); mk(tmp, "tb"); mk(tmp, "tc");
    while (p->tabs->len > 1) panel_tab_close(p);
    panel_load(p, a);
    panel_tab_new(p, b);
    panel_tab_new(p, c);
    CHECK(p->tabs->len == 3);
    panel_tab_cycle(p, -(int)p->tab_active);          /* show A */
    CHECK(p->tab_active == 0 && strcmp(p->cwd, a) == 0);

    middle_click_tab(p, 2);                           /* close C */
    CHECK(p->tabs->len == 2);
    CHECK(p->tab_active == 0 && strcmp(p->cwd, a) == 0);

    panel_tab_cycle(p, 1);                            /* show B */
    CHECK(p->tab_active == 1 && strcmp(p->cwd, b) == 0);
    middle_click_tab(p, 0);                           /* close A, left of it */
    CHECK(p->tabs->len == 1);
    CHECK(p->tab_active == 0 && strcmp(p->cwd, b) == 0);

    middle_click_tab(p, 0);                           /* the last one stays */
    CHECK(p->tabs->len == 1);
    g_free(a); g_free(b); g_free(c);
    g_print("ok tab middle close\n");
}


/* ── a job queued behind a background job can be cancelled while it
 * waits (its window is modal; it used to block until the first ended) ── */

static gboolean queued_ran;
static gint64 queued_back;

static void long_worker(Job *job, gpointer d)
{
    (void)d;
    for (int i = 0; i < 40 && !job_cancelled(job); i++) g_usleep(50000);  /* 2 s */
    job1_end = g_get_monotonic_time();
}

static void mark_worker(Job *job, gpointer d) { (void)job; (void)d; queued_ran = TRUE; }

static gboolean cancel_job(gpointer job) { fs_cancel(&((Job *)job)->ctx); return G_SOURCE_REMOVE; }

static gboolean start_queued(gpointer fm)
{
    Job *j2 = job_new(fm, "Queued");
    g_timeout_add(150, cancel_job, j2);
    job_run(j2, mark_worker, NULL);
    queued_back = g_get_monotonic_time();
    job_free(j2);
    return G_SOURCE_REMOVE;
}

static gboolean bg_then_queue(gpointer job)
{
    Job *j = job;
    g_signal_emit_by_name(j->bg_btn, "clicked");
    g_timeout_add(50, start_queued, j->fm);
    return G_SOURCE_REMOVE;
}

static void test_queued_cancel(FM *fm)
{
    job1_end = 0; queued_ran = FALSE; queued_back = 0;
    Job *j1 = job_new(fm, "Long");
    j1->allow_background = TRUE;
    g_timeout_add(50, bg_then_queue, j1);
    job_run(j1, long_worker, NULL);
    job_free(j1);
    CHECK(!queued_ran);                         /* cancelled before its turn */
    CHECK(queued_back > 0 && queued_back < job1_end);   /* did not wait */
    CHECK(job_running_count() == 0);
    g_print("ok queued job cancel\n");
}


/* ── known_hosts: a file without a final newline (hand edited) must not
 * get the new entry glued onto its last line ─────────────────────── */

static void test_known_hosts_append(void)
{
    gchar *path = known_hosts_path();
    gchar *orig = NULL; gsize olen = 0;
    gboolean had = g_file_get_contents(path, &orig, &olen, NULL);
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    LIBSSH2_SESSION *s = libssh2_session_init();
    LIBSSH2_KNOWNHOSTS *kh = s ? libssh2_knownhost_init(s) : NULL;
    CHECK(kh != NULL);
    struct libssh2_knownhost *e = NULL;
    const char *key = "AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7dGVzdGtleQ==";
    if (kh)
        libssh2_knownhost_addc(kh, "edited.invalid", NULL, key, strlen(key), NULL, 0,
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_BASE64 |
            LIBSSH2_KNOWNHOST_KEY_SSHRSA, &e);
    CHECK(e != NULL);

    const char *old = "old.invalid ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAAAgQC7b2xk";
    for (int round = 0; round < 2 && e; round++) {
        /* round 0: no final newline; round 1: normal file */
        gchar *init = g_strconcat(old, round ? "\n" : "", NULL);
        CHECK(g_file_set_contents(path, init, -1, NULL));
        CHECK(known_hosts_append(kh, e));
        gchar *got = NULL;
        CHECK(g_file_get_contents(path, &got, NULL, NULL));
        gchar **lines = got ? g_strsplit(got, "\n", -1) : NULL;
        CHECK(lines && g_strv_length(lines) == 3);        /* 2 lines + "" */
        CHECK(lines && g_strcmp0(lines[0], old) == 0);
        CHECK(lines && lines[1] && g_str_has_prefix(lines[1], "edited.invalid "));
        g_strfreev(lines);
        g_free(got); g_free(init);
    }

    if (kh) libssh2_knownhost_free(kh);
    if (s) libssh2_session_free(s);
    if (had) g_file_set_contents(path, orig, (gssize)olen, NULL);
    else g_unlink(path);
    g_free(orig); g_free(path);
    g_print("ok known_hosts append\n");
}


/* ── command line: a watched panel is not reloaded after every command
 * (its monitor brings the changes); an unwatched one still is ──────── */

static void count_changes(GListModel *m, guint pos, guint rem, guint add, int *n)
{
    (void)m; (void)pos; (void)rem; (void)add;
    (*n)++;
}

static void test_cmd_no_reload(FM *fm)
{
    local_panels(fm);
    gboolean prev_ar = fm->auto_refresh;
    gchar *d = g_build_filename(tmp, "cmdr", NULL);
    g_mkdir_with_parents(d, 0755);
    write_in(d, "one.txt", "1");
    Panel *p = &fm->panels[0], *o = &fm->panels[1];

    fm->auto_refresh = TRUE;
    panel_load(p, d);
    CHECK(p->dir_monitor != NULL);
    int changes = 0;
    gulong h = g_signal_connect(p->store, "items-changed", G_CALLBACK(count_changes), &changes);
    cmd_report(fm, "true", 0, NULL, NULL);
    pump(50);
    CHECK(changes == 0);
    g_signal_handler_disconnect(p->store, h);

    fm->auto_refresh = FALSE;
    panel_monitor_stop(o);
    panel_load(o, d);
    CHECK(o->dir_monitor == NULL);
    write_in(d, "two.txt", "2");
    cmd_report(fm, "touch two.txt", 0, NULL, NULL);
    pump(50);
    CHECK(store_has(o, "two.txt"));                  /* unwatched: reloaded */

    fm->auto_refresh = prev_ar;
    local_panels(fm);
    g_free(d);
    g_print("ok cmd line reload\n");
}


/* ── SFTP auto-refresh: many remote changes go in as one splice, marks
 * survive; a small change stays incremental ────────────────────────── */

static LIBSSH2_SFTP_ATTRIBUTES *fake_attr(gint64 size, gint64 mtime)
{
    LIBSSH2_SFTP_ATTRIBUTES *a = g_new0(LIBSSH2_SFTP_ATTRIBUTES, 1);
    a->flags = LIBSSH2_SFTP_ATTR_PERMISSIONS | LIBSSH2_SFTP_ATTR_SIZE |
               LIBSSH2_SFTP_ATTR_ACMODTIME;
    a->permissions = LIBSSH2_SFTP_S_IFREG | 0644;
    a->filesize = (libssh2_uint64_t)size;
    a->mtime = (unsigned long)mtime;
    return a;
}

static gint64 item_mtime(Panel *p, const char *name)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    for (guint i = 0; i < n; i++) {
        FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
        gint64 m = strcmp(it->name, name) == 0 ? it->date_raw : -1;
        g_object_unref(it);
        if (m >= 0) return m;
    }
    return -1;
}

static void test_sftp_poll_bulk(FM *fm)
{
    local_panels(fm);
    gchar *d = g_build_filename(tmp, "pollb", NULL);
    g_mkdir_with_parents(d, 0755);
    write_in(d, "a.txt", "1");
    write_in(d, "b.txt", "1");
    write_in(d, "c.txt", "1");
    Panel *p = &fm->panels[0];
    panel_load(p, d);
    sel_mark_all(p, FALSE);
    panel_select_name(p, "b.txt");
    sel_toggle_cursor(p, FALSE);
    CHECK(has_marked_item(p, "b.txt"));
    gint64 bm = item_mtime(p, "b.txt");

    GHashTable *remote = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(remote, g_strdup("b.txt"), fake_attr(1, bm));   /* unchanged */
    g_hash_table_insert(remote, g_strdup("c.txt"), fake_attr(1, bm));
    g_hash_table_insert(remote, g_strdup("a.txt"), fake_attr(1, bm));
    g_hash_table_insert(remote, g_strdup("new.txt"), fake_attr(5, bm));
    /* one difference: incremental path, store untouched here */
    CHECK(!sftp_poll_rebuild(p, remote));

    g_hash_table_remove(remote, "a.txt");
    for (int i = 0; i < 100; i++) {
        gchar n[32];
        g_snprintf(n, sizeof n, "r%03d", i);
        g_hash_table_insert(remote, g_strdup(n), fake_attr(i, bm));
    }
    int changes = 0;
    gulong h = g_signal_connect(p->store, "items-changed", G_CALLBACK(count_changes), &changes);
    CHECK(sftp_poll_rebuild(p, remote));
    g_signal_handler_disconnect(p->store, h);
    CHECK(changes == 1);                              /* one splice */
    CHECK(!store_has(p, "a.txt"));
    CHECK(store_has(p, "b.txt") && store_has(p, "c.txt") && store_has(p, "new.txt"));
    CHECK(store_has(p, "r000") && store_has(p, "r099"));
    CHECK(has_marked_item(p, "b.txt"));               /* mark kept */
    CHECK(store_has(p, ".."));
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->store));
    CHECK(n == 1 + 3 + 100);                          /* "..", b, c, new, r* */
    g_hash_table_destroy(remote);
    sel_mark_all(p, FALSE);
    local_panels(fm);
    g_free(d);
    g_print("ok sftp poll bulk\n");
}


/* ── a mount / connection finishing while a copy runs must not switch
 * the panel (it would drop the connection the job is using) ───────── */

static gboolean guard_seen, guard_kept;
static gchar *guard_cwd;

static gboolean guard_probe(gpointer fm_)
{
    FM *fm = fm_;
    Panel *p = &fm->panels[0];
    guard_seen = job_foreground_running();
    open_in_panel(p, "/");                            /* devices.c */
    guard_kept = g_strcmp0(p->cwd, guard_cwd) == 0;
    return G_SOURCE_REMOVE;
}

static void sleep_worker(Job *job, gpointer d) { (void)job; (void)d; g_usleep(400000); }

static void test_job_guard(FM *fm)
{
    local_panels(fm);
    Panel *p = &fm->panels[0];
    guard_cwd = g_strdup(p->cwd);
    guard_seen = guard_kept = FALSE;
    CHECK(!job_foreground_running());
    Job *j = job_new(fm, "Guard");
    g_timeout_add(100, guard_probe, fm);
    job_run(j, sleep_worker, NULL);
    job_free(j);
    CHECK(guard_seen && guard_kept);
    CHECK(!job_foreground_running());
    open_in_panel(p, tmp);                            /* no job: opens */
    CHECK(g_strcmp0(p->cwd, tmp) == 0);
    g_clear_pointer(&guard_cwd, g_free);
    local_panels(fm);
    g_print("ok job guard\n");
}


/* ── helpers for the review tests ─────────────────────────────────── */

static int count_hidden_aside(const char *dir)
{
    int n = 0;
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d)))
        if (strstr(e, ".fm-")) n++;
    if (d) g_dir_close(d);
    return n;
}

static gchar *slurp(const char *path)
{
    gchar *d = NULL;
    return g_file_get_contents(path, &d, NULL, NULL) ? d : NULL;
}

/* ── SFTP transfers: remote → remote one file at a time, a connection
 * dropped by its tab during the job stays alive, a failed replace keeps
 * the old target (upload and download) ────────────────────────────── */

static Panel *drop_panel;
static gboolean drop_conn_idle(gpointer d)
{
    (void)d;
    panel_tab_set_conn(drop_panel, NULL);         /* the tab lets go */
    return G_SOURCE_REMOVE;
}

static void test_sftp_xfer(FM *fm, const char *work)
{
    local_panels(fm);
    Panel *p = &fm->panels[0];
    gchar *x = g_build_filename(work, "xfer", NULL);
    mk(work, "xfer");
    gchar *src = g_build_filename(x, "src", NULL);
    mk(x, "src");
    gchar *d1 = g_build_filename(src, "d1", NULL);
    mk(src, "d1");
    write_in(d1, "f1", "one");
    write_in(src, "f2", "two");

    SshConn *c = connect_test();
    CHECK(c != NULL);
    if (!c) goto done;
    panel_tab_set_conn(p, c);
    panel_load_remote(p, x);
    CHECK(c->refs == 1);

    /* remote → remote copy of a tree */
    gchar *cdst = g_build_filename(x, "copy", NULL);
    mk(x, "copy");
    GList *l = g_list_append(NULL, src);
    CHECK(fo_transfer(fm, l, c, cdst, c, FALSE) == FS_OK);
    gchar *cf1 = g_build_filename(cdst, "src", "d1", "f1", NULL);
    gchar *cf2 = g_build_filename(cdst, "src", "f2", NULL);
    gchar *g1 = slurp(cf1), *g2 = slurp(cf2);
    CHECK(g_strcmp0(g1, "one") == 0 && g_strcmp0(g2, "two") == 0);
    CHECK(g_file_test(src, G_FILE_TEST_IS_DIR));     /* copy keeps source */
    g_free(g1); g_free(g2);

    /* remote → remote move: source tree gone, target complete */
    gchar *mdst = g_build_filename(x, "moved", NULL);
    mk(x, "moved");
    CHECK(fo_transfer(fm, l, c, mdst, c, TRUE) == FS_OK);
    gchar *mf1 = g_build_filename(mdst, "src", "d1", "f1", NULL);
    g1 = slurp(mf1);
    CHECK(g_strcmp0(g1, "one") == 0);
    CHECK(!g_file_test(src, G_FILE_TEST_EXISTS));
    g_free(g1);
    g_list_free(l);

    /* failed replace keeps the old target – upload: a local directory
     * with an unreadable file over a remote file */
    if (getuid() != 0) {
        gchar *up = g_build_filename(tmp, "up", NULL);
        mk(tmp, "up");
        write_in(up, "ok.txt", "ok");
        write_in(up, "bad.txt", "bad");
        gchar *bad = g_build_filename(up, "bad.txt", NULL);
        chmod(bad, 0);
        gchar *rt = g_build_filename(x, "up", NULL);
        CHECK(g_file_set_contents(rt, "OLD", -1, NULL));
        FsCtx fc = { .policy = FS_OVERWRITE_ALL };
        ssh_conn_lock(c);
        int r = fs_sftp_upload(&fc, c->sftp, up, rt);
        ssh_conn_unlock(c);
        CHECK(r != FS_OK);
        gchar *got = slurp(rt);
        CHECK(g_strcmp0(got, "OLD") == 0);
        CHECK(count_hidden_aside(x) == 0);
        g_free(got);
        fs_ctx_clear(&fc);

        /* download: a remote directory with an unreadable file over a
         * local file */
        gchar *rd = g_build_filename(x, "down", NULL);
        mk(x, "down");
        write_in(rd, "ok.txt", "ok");
        write_in(rd, "bad.txt", "bad");
        gchar *rbad = g_build_filename(rd, "bad.txt", NULL);
        chmod(rbad, 0);
        gchar *lt = g_build_filename(tmp, "down", NULL);
        CHECK(g_file_set_contents(lt, "OLDL", -1, NULL));
        FsCtx dc = { .policy = FS_OVERWRITE_ALL };
        ssh_conn_lock(c);
        r = fs_sftp_download(&dc, c->sftp, rd, lt);
        ssh_conn_unlock(c);
        CHECK(r != FS_OK);
        got = slurp(lt);
        CHECK(g_strcmp0(got, "OLDL") == 0);
        CHECK(count_hidden_aside(tmp) == 0);
        g_free(got);
        fs_ctx_clear(&dc);
        chmod(bad, 0644); chmod(rbad, 0644);
        g_unlink(lt);
        g_free(bad); g_free(rbad); g_free(rt); g_free(rd); g_free(lt); g_free(up);
    }

    /* the panel drops the connection while the job runs: the job's own
     * reference keeps it alive, it goes away only after the job */
    gchar *big = g_build_filename(x, "big.bin", NULL);
    {
        gchar *data = g_malloc0(4 << 20);
        CHECK(g_file_set_contents(big, data, 4 << 20, NULL));
        g_free(data);
    }
    gchar *ldst = g_build_filename(tmp, "dl", NULL);
    mk(tmp, "dl");
    drop_panel = p;
    g_idle_add(drop_conn_idle, NULL);              /* runs in the job's loop */
    l = g_list_append(NULL, big);
    CHECK(fo_transfer(fm, l, c, ldst, NULL, FALSE) == FS_OK);   /* c freed after */
    g_list_free(l);
    CHECK(p->ssh_conn == NULL);
    gchar *lbig = g_build_filename(ldst, "big.bin", NULL);
    struct stat st;
    CHECK(stat(lbig, &st) == 0 && st.st_size == (4 << 20));
    g_free(lbig); g_free(ldst); g_free(big);

    g_free(cdst); g_free(cf1); g_free(cf2); g_free(mdst); g_free(mf1);
done:
    local_panels(fm);
    g_free(d1); g_free(src); g_free(x);
    g_print("ok sftp transfers\n");
}

/* ── TCP connect gives up after its timeout (a black-holed host used to
 * block ~2 min); a listening port still connects ─────────────────── */

static void test_connect_timeout(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(22) };
    inet_pton(AF_INET, "10.255.255.1", &a.sin_addr);     /* unroutable */
    int s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    double t0 = now_s();
    int r = connect_timeout(s, (struct sockaddr *)&a, sizeof a, 300);
    double dt = now_s() - t0;
    close(s);
    CHECK(r == -1);
    CHECK(dt < 2.0);

    /* a local listener: connects, socket blocking again */
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in la = { .sin_family = AF_INET };
    inet_pton(AF_INET, "127.0.0.1", &la.sin_addr);
    socklen_t ll = sizeof la;
    CHECK(bind(ls, (struct sockaddr *)&la, sizeof la) == 0 && listen(ls, 1) == 0 &&
          getsockname(ls, (struct sockaddr *)&la, &ll) == 0);
    s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    CHECK(connect_timeout(s, (struct sockaddr *)&la, sizeof la, 2000) == 0);
    CHECK((fcntl(s, F_GETFL) & O_NONBLOCK) == 0);
    int ka = 0; socklen_t kl = sizeof ka;
    CHECK(getsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &ka, &kl) == 0 && ka);
    close(s);

    /* closed port: refused at once, not a timeout */
    s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    close(ls);                                   /* nobody listens now */
    t0 = now_s();
    CHECK(connect_timeout(s, (struct sockaddr *)&la, sizeof la, 2000) == -1);
    CHECK(now_s() - t0 < 1.0);
    close(s);
    g_print("ok connect timeout\n");
}

/* ── editor search: capped match count, F3 starts at the cached offset,
 * offsets dropped (and still right) after an edit ─────────────────── */

static void test_editor_search(FM *fm)
{
    EditorCtx *ctx = g_new0(EditorCtx, 1);
    ctx->fm = fm;
    ctx->buffer = gtk_text_buffer_new(NULL);
    ctx->text_view = g_object_ref_sink(gtk_text_view_new_with_buffer(ctx->buffer));
    ctx->search_entry = g_object_ref_sink(gtk_entry_new());
    ctx->match_lbl = g_object_ref_sink(gtk_label_new(""));
    ctx->match_tag = gtk_text_buffer_create_tag(ctx->buffer, NULL, "background", "yellow", NULL);
    ctx->match_lines = g_array_new(FALSE, FALSE, sizeof(int));
    g_signal_connect(ctx->buffer, "changed", G_CALLBACK(on_buffer_changed), ctx);
    ctx->window = g_object_ref_sink(gtk_window_new());

    /* 30 000 lines with "e": far over the cap */
    GString *txt = g_string_new(NULL);
    for (int i = 0; i < 30000; i++) g_string_append_printf(txt, "line %d e\n", i);
    gtk_text_buffer_set_text(ctx->buffer, txt->str, -1);
    g_string_free(txt, TRUE);

    double t0 = now_s();
    search_highlight_all(ctx, "e");
    double dt = now_s() - t0;
    CHECK(ctx->total_matches == SEARCH_MAX_MATCHES);
    CHECK(ctx->match_offs && ctx->match_offs->len == SEARCH_MAX_MATCHES);
    CHECK(g_str_has_suffix(gtk_label_get_text(GTK_LABEL(ctx->match_lbl)), "+"));
    CHECK(dt < 5.0);

    /* jump far in: lands on the right match (line of the 9000th "e") */
    gtk_editable_set_text(GTK_EDITABLE(ctx->search_entry), "e");
    if (ctx->search_idle) { g_source_remove(ctx->search_idle); ctx->search_idle = 0; }
    search_goto_match(ctx, 9000);
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &it, gtk_text_buffer_get_insert(ctx->buffer));
    CHECK(gtk_text_iter_get_offset(&it) == g_array_index(ctx->match_offs, int, 8999));
    CHECK(ctx->current_match == 9000);
    /* a thousand steps from the cache stay fast */
    t0 = now_s();
    for (int i = 0; i < 1000; i++) search_goto_match(ctx, 9000 + (i % 1000));
    CHECK(now_s() - t0 < 10.0);        /* rescanning from the top: minutes */

    /* an edit drops the offsets; F3 still finds the right place */
    GtkTextIter s0;
    gtk_text_buffer_get_start_iter(ctx->buffer, &s0);
    gtk_text_buffer_insert(ctx->buffer, &s0, "xx\n", -1);
    CHECK(ctx->match_offs == NULL);
    search_goto_match(ctx, 2);
    gtk_text_buffer_get_iter_at_mark(ctx->buffer, &it, gtk_text_buffer_get_insert(ctx->buffer));
    /* "xx\nline 0 e\n…": the 2nd "e" is the one after "line 0 " */
    CHECK(gtk_text_iter_get_line(&it) == 1);
    CHECK(gtk_text_iter_get_offset(&it) == 10);

    /* a short needle list below the cap: no "+" */
    search_highlight_all(ctx, "line 2999 ");
    CHECK(ctx->total_matches == 1);
    CHECK(!g_str_has_suffix(gtk_label_get_text(GTK_LABEL(ctx->match_lbl)), "+"));

    if (ctx->search_idle) g_source_remove(ctx->search_idle);
    if (ctx->hl_idle) g_source_remove(ctx->hl_idle);     /* as editor close does */
    g_signal_handlers_disconnect_by_data(ctx->buffer, ctx);
    g_array_free(ctx->match_lines, TRUE);
    if (ctx->match_offs) g_array_free(ctx->match_offs, TRUE);
    gtk_window_destroy(GTK_WINDOW(ctx->window));
    g_object_unref(ctx->window);
    g_object_unref(ctx->text_view);
    g_object_unref(ctx->search_entry);
    g_object_unref(ctx->match_lbl);
    g_object_unref(ctx->buffer);
    g_free(ctx);
    g_print("ok editor search\n");
}

/* ── compare: diff runs in a worker and keeps only what is shown;
 * without diff, sizes first and streamed comparison ──────────────── */

static gboolean diff_done_flag;
static void diff_wait_cb(GObject *s, GAsyncResult *r, gpointer d)
{
    (void)s; (void)r; (void)d;
    diff_done_flag = TRUE;
}

static void run_diff_thread(DiffCtx *dc)
{
    diff_done_flag = FALSE;
    GTask *t = g_task_new(NULL, NULL, diff_wait_cb, NULL);
    g_task_set_task_data(t, dc, NULL);
    g_task_run_in_thread(t, diff_thread);
    g_object_unref(t);
    gint64 end = g_get_monotonic_time() + 60 * G_USEC_PER_SEC;
    while (!diff_done_flag && g_get_monotonic_time() < end) pump(20);
    CHECK(diff_done_flag);
}

static void test_compare_worker(void)
{
    gchar *d = g_build_filename(tmp, "cmpw", NULL);
    g_mkdir_with_parents(d, 0755);
    gchar *a = g_build_filename(d, "a.txt", NULL), *b = g_build_filename(d, "b.txt", NULL);
    gchar *c = g_build_filename(d, "c.txt", NULL);

    /* two large, completely different files: diff output » 8 MB */
    GString *sa = g_string_new(NULL), *sb = g_string_new(NULL);
    for (int i = 0; i < 400000; i++) {
        g_string_append_printf(sa, "left line %d\n", i);
        g_string_append_printf(sb, "right line %d\n", i);
    }
    CHECK(g_file_set_contents(a, sa->str, (gssize)sa->len, NULL));
    CHECK(g_file_set_contents(b, sb->str, (gssize)sb->len, NULL));
    CHECK(g_file_set_contents(c, sa->str, (gssize)sa->len, NULL));

    gchar *diff_bin = g_find_program_in_path("diff");
    if (diff_bin) {
        DiffCtx dc = { .f1 = a, .f2 = b };
        run_diff_thread(&dc);
        CHECK(dc.code == 1);
        CHECK(dc.out && strlen(dc.out) > DIFF_MAX_SHOW);
        CHECK(dc.out && strlen(dc.out) <= DIFF_MAX_SHOW + 65536);   /* not all of it */
        g_free(dc.out); g_free(dc.err);

        DiffCtx de = { .f1 = a, .f2 = c };
        run_diff_thread(&de);
        CHECK(de.code == 0);
        g_free(de.out); g_free(de.err);
    }

    g_free(diff_bin);

    /* no diff installed: byte comparison */
    CHECK(same_bytes(a, c) == 1);
    CHECK(same_bytes(a, b) == 0);               /* sizes differ: at once */
    gchar *e = g_build_filename(d, "e.txt", NULL);
    sa->str[sa->len - 2] ^= 1;                   /* same size, last byte differs */
    CHECK(g_file_set_contents(e, sa->str, (gssize)sa->len, NULL));
    CHECK(same_bytes(a, e) == 0);
    gchar *none = g_build_filename(d, "none", NULL);
    CHECK(same_bytes(a, none) == -1);

    g_string_free(sa, TRUE); g_string_free(sb, TRUE);
    g_free(a); g_free(b); g_free(c); g_free(e); g_free(none); g_free(d);
    g_print("ok compare worker\n");
}


/* ── multi-rename: typing waits for a pause before recomputing (each
 * pass lstat()s every item and rebuilds the preview); Enter flushes ── */

static void test_mrn_debounce(void)
{
    gchar *d = g_build_filename(tmp, "mrn", NULL);
    g_mkdir_with_parents(d, 0755);
    write_in(d, "a.txt", "1");
    MrnDlg m = { .items = g_ptr_array_new_with_free_func(mrn_item_free) };
    MrnItem *it = g_new0(MrnItem, 1);
    it->path = g_build_filename(d, "a.txt", NULL);
    it->dir = g_strdup(d);
    it->old_name = g_strdup("a.txt");
    g_ptr_array_add(m.items, it);
    GtkWidget *keep[] = {
        m.mask = gtk_entry_new(), m.find = gtk_entry_new(), m.repl = gtk_entry_new(),
        m.start = mrn_spin(0, 1000, 1), m.step = mrn_spin(1, 100, 1),
        m.digits = mrn_spin(1, 9, 1),
        m.casedd = gtk_drop_down_new_from_strings((const char *[]){ "As is", "lower", "UPPER", NULL }),
        m.scroll = gtk_scrolled_window_new(), m.status = gtk_label_new(""),
        m.ok_btn = gtk_button_new(),
    };
    for (gsize i = 0; i < G_N_ELEMENTS(keep); i++) g_object_ref_sink(keep[i]);
    gtk_editable_set_text(GTK_EDITABLE(m.mask), "[N]");

    /* five quick keystrokes: nothing computed yet, OK disabled */
    const char *steps[] = { "[", "[N", "[N]", "[N]_", "[N]_x.[E]" };
    for (int i = 0; i < 5; i++) {
        gtk_editable_set_text(GTK_EDITABLE(m.mask), steps[i]);
        on_mrn_changed(NULL, &m);
    }
    CHECK(m.update_id != 0);
    CHECK(it->new_name == NULL);
    CHECK(!gtk_widget_get_sensitive(m.ok_btn));
    pump(MRN_DEBOUNCE_MS + 150);                  /* the pause: computed once */
    CHECK(m.update_id == 0);
    CHECK(g_strcmp0(it->new_name, "a_x.txt") == 0);
    CHECK(gtk_widget_get_sensitive(m.ok_btn));

    /* Enter right after typing: flushed at once */
    gtk_editable_set_text(GTK_EDITABLE(m.mask), "[N]_y.[E]");
    on_mrn_changed(NULL, &m);
    mrn_flush(&m);
    CHECK(m.update_id == 0);
    CHECK(g_strcmp0(it->new_name, "a_y.txt") == 0);
    CHECK(gtk_widget_get_sensitive(m.ok_btn));

    /* invalid name: OK stays off after the recheck */
    gtk_editable_set_text(GTK_EDITABLE(m.mask), "");
    on_mrn_changed(NULL, &m);
    mrn_flush(&m);
    CHECK(!gtk_widget_get_sensitive(m.ok_btn));

    for (gsize i = 0; i < G_N_ELEMENTS(keep); i++) g_object_unref(keep[i]);
    g_ptr_array_unref(m.items);
    g_free(d);
    g_print("ok multi-rename debounce\n");
}


/* ── benchmark (make bench): timings on a 50 000 file directory ───── */

static double bench_ms(void) { return g_get_monotonic_time() / 1000.0; }

static void bench(FM *fm)
{
    const int N = 50000;
    gchar *d = g_build_filename(tmp, "bench", NULL);
    g_mkdir_with_parents(d, 0755);
    double t = bench_ms();
    for (int i = 0; i < N; i++) {
        gchar n[64];
        g_snprintf(n, sizeof n, "file_%05d_%s.%s", i, i % 3 ? "data" : "Report",
                   i % 5 ? "txt" : "c");
        gchar *f = g_build_filename(d, n, NULL);
        int fd = open(f, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        g_free(f);
    }
    for (int i = 0; i < 500; i++) {
        gchar n[32];
        g_snprintf(n, sizeof n, "dir_%03d", i);
        mk(d, n);
    }
    g_print("bench: created %d entries in %.0f ms\n", N + 500, bench_ms() - t);

    Panel *p = &fm->panels[0];
    local_panels(fm);
    gboolean ar = fm->auto_refresh;
    fm->auto_refresh = TRUE;

    t = bench_ms(); panel_load(p, d);         double load = bench_ms() - t;
    pump(50);
    t = bench_ms(); panel_load(p, tmp);       double small = bench_ms() - t;
    t = bench_ms(); panel_load(p, d);         double load2 = bench_ms() - t;
    t = bench_ms(); panel_reload(p);          double reload = bench_ms() - t;
    CHECK(g_list_model_get_n_items(G_LIST_MODEL(p->store)) >= (guint)N);

    p->cursor_pos = N / 2;
    t = bench_ms(); panel_focus(p);           double focus = bench_ms() - t;

    t = bench_ms(); sel_mark_all(p, TRUE);    double markall = bench_ms() - t;
    t = bench_ms(); panel_update_status(p);   double status = bench_ms() - t;
    t = bench_ms(); sel_invert(p);            double invert = bench_ms() - t;
    sel_mark_all(p, FALSE);

    /* quick filter: typing three characters */
    double filt = 0;
    const char *steps[] = { "R", "Re", "Rep", "" };
    for (int i = 0; i < 4; i++) {
        t = bench_ms();
        gtk_editable_set_text(GTK_EDITABLE(p->filter_entry), steps[i]);
        pump(0);
        filt += bench_ms() - t;
    }

    /* 60 changed files: one monitor batch (below the reload limit) */
    t = bench_ms();
    for (int i = 0; i < 60; i++) {
        gchar n[32];
        g_snprintf(n, sizeof n, "zz_new_%02d", i);
        write_in(d, n, "x");
    }
    while (!store_has(p, "zz_new_59") && bench_ms() - t < 10000) pump(5);
    double batch = bench_ms() - t;   /* includes the 150 ms batch delay + rate limit */

    /* main-loop stall while the batch is applied: longest gap between ticks */
    t = bench_ms();
    for (int i = 0; i < 60; i++) {
        gchar *f = g_strdup_printf("%s/zz_new_%02d", d, i);
        g_unlink(f);
        g_free(f);
    }
    double worst = 0, last = bench_ms();
    while (store_has(p, "zz_new_00") && bench_ms() - t < 10000) {
        g_main_context_iteration(NULL, FALSE);
        double now = bench_ms();
        if (now - last > worst) worst = now - last;
        last = now;
    }

    /* memory across repeated loads of the big directory: steady = no leak */
    long rss[11];
    for (int i = 0; i <= 10; i++) {
        panel_load(p, tmp);
        panel_load(p, d);
        pump(0);
        FILE *f = fopen("/proc/self/statm", "r");
        long sz = 0, res = 0;
        if (f) { if (fscanf(f, "%ld %ld", &sz, &res) != 2) res = 0; fclose(f); }
        rss[i] = res * (sysconf(_SC_PAGESIZE) / 1024);
    }
    g_print("bench: RSS 50k dir     %ld MB after 1 load, %ld MB after 11 (%+ld MB)\n",
            rss[0] / 1024, rss[10] / 1024, (rss[10] - rss[0]) / 1024);
    g_print("bench: RSS per 5 loads %ld, %ld, %ld MB\n", rss[0] / 1024, rss[5] / 1024, rss[10] / 1024);

    g_print("bench: load 50k        %7.1f ms (again %.1f, small dir %.1f)\n", load, load2, small);
    g_print("bench: reload          %7.1f ms\n", reload);
    g_print("bench: focus mid-list  %7.1f ms\n", focus);
    g_print("bench: mark all        %7.1f ms, status %.1f ms, invert %.1f ms\n",
            markall, status, invert);
    g_print("bench: filter 4 keys   %7.1f ms\n", filt);
    g_print("bench: 60-file batch   %7.1f ms until shown (incl. delays)\n", batch);
    g_print("bench: worst UI stall  %7.1f ms during a 60-file batch\n", worst);

    fm->auto_refresh = ar;
    panel_load(p, tmp);
    gchar *cmd = g_strdup_printf("rm -rf '%s'", d);
    if (system(cmd) != 0) { /* ignore */ }
    g_free(cmd);
    g_free(d);
}

/* ── driver ───────────────────────────────────────────────────────── */

static gboolean run_tests(gpointer data)
{
    FM *fm = data;
    pump(200);
    if (g_getenv("FM_BENCH")) {
        bench(fm);
        g_application_quit(G_APPLICATION(fm->app));
        return G_SOURCE_REMOVE;
    }
    test_local_tabs(fm);
    const char *work = g_getenv("FM_SFTP_DIR");
    if (work && g_getenv("FM_SFTP_PORT")) test_sftp_tabs(fm, work);
    else g_print("skip sftp tabs (no test sshd)\n");
    if (work && g_getenv("FM_SFTP_PORT")) test_sftp_save_exec(fm, work);
    if (work && g_getenv("FM_SFTP_PORT")) test_sftp_xfer(fm, work);
    test_devices(fm);
    test_compare_dirs(fm);
    test_quickview(fm);
    test_background_queue(fm);
    test_monitor(fm);
    test_cks_verify(fm);
    test_quickview_image(fm);
    test_focus_keeps_scroll(fm);
    test_tab_middle_close(fm);
    test_queued_cancel(fm);
    test_known_hosts_append();
    test_cmd_no_reload(fm);
    test_sftp_poll_bulk(fm);
    test_job_guard(fm);
    test_connect_timeout();
    test_editor_search(fm);
    test_compare_worker();
    test_mrn_debounce();
    g_application_quit(G_APPLICATION(fm->app));
    return G_SOURCE_REMOVE;
}

static void on_test_activate(GtkApplication *app, gpointer data)
{
    (void)app;
    g_idle_add(run_tests, data);
}

static void watchdog(int sig)
{
    (void)sig;
    static const char m[] = "FAIL: test hung (90 s watchdog)\n";
    if (write(2, m, sizeof m - 1) < 0) { /* nothing */ }
    _exit(2);
}

int main(int argc, char *argv[])
{
    (void)argc;
    setlocale(LC_ALL, "");
    tmp = g_dir_make_tmp("fm-gui-XXXXXX", NULL);
    g_setenv("HOME", tmp, TRUE);            /* settings + known_hosts isolated */
    g_setenv("XDG_CONFIG_HOME", tmp, TRUE);
    g_setenv("GSETTINGS_BACKEND", "memory", TRUE);

    /* trust the test sshd's host key */
    const char *hk = g_getenv("FM_SFTP_HOSTKEY");
    if (hk) {
        gchar *pub = NULL;
        if (g_file_get_contents(hk, &pub, NULL, NULL)) {
            gchar **f = g_strsplit(g_strstrip(pub), " ", 3);
            gchar *dir = g_build_filename(tmp, ".ssh", NULL);
            g_mkdir_with_parents(dir, 0700);
            gchar *kh = g_build_filename(dir, "known_hosts", NULL);
            gchar *line = g_strdup_printf("[127.0.0.1]:%s %s %s\n",
                                          g_getenv("FM_SFTP_PORT"), f[0], f[1]);
            g_file_set_contents(kh, line, -1, NULL);
            g_free(line); g_free(kh); g_free(dir); g_strfreev(f);
        }
        g_free(pub);
    }

    memset(&g_fm, 0, sizeof(g_fm));
#ifdef HAVE_LIBADWAITA
    adw_init();
    GtkApplication *app = GTK_APPLICATION(
        adw_application_new("sk.km.fm.test", G_APPLICATION_NON_UNIQUE));
#else
    GtkApplication *app = gtk_application_new("sk.km.fm.test", G_APPLICATION_NON_UNIQUE);
#endif
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &g_fm);
    g_signal_connect(app, "activate", G_CALLBACK(on_test_activate), &g_fm);
    g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), &g_fm);
    g_object_set_data(G_OBJECT(app), "fm", &g_fm);
    signal(SIGALRM, watchdog);
    alarm(90);

    char *args[] = { argv[0], NULL };
    g_application_run(G_APPLICATION(app), 1, args);
    view_tmpdirs_cleanup();
    arc_panel_cleanup();
    g_object_unref(app);

    gchar *cmd = g_strdup_printf("rm -rf '%s'", tmp);
    if (system(cmd) != 0) { /* ignore */ }
    g_free(cmd);
    g_free(tmp);

    g_print("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
