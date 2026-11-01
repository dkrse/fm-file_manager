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

#include <sys/socket.h>
#include <signal.h>
#include <glib/gstdio.h>

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
    CHECK(g_strcmp0(tab_cur(p)->rpath, rdir) == 0);
    CHECK(strcmp(p->cwd, local) == 0);          /* local fallback kept */

    /* Ctrl+T shares the connection */
    panel_tab_new(p, NULL);
    CHECK(p->tabs->len == 2 && c->refs == 2 && tab_cur(p)->conn == c);

    /* Ctrl+Up equivalent: remote subdirectory in a new tab */
    gchar *sub = g_build_filename(rdir, "sub", NULL);
    panel_tab_new(p, sub);
    CHECK(p->tabs->len == 3 && c->refs == 3);
    CHECK(g_strcmp0(c->remote_path, sub) == 0);

    /* a local tab in between */
    panel_tab_new(p, NULL);
    panel_tab_set_conn(p, NULL);
    CHECK(p->ssh_conn == NULL && c->refs == 3 && tab_cur(p)->conn == NULL);
    CHECK(strcmp(p->cwd, local) == 0);

    /* switching: each tab its own directory, never blocks */
    double worst = 0;
    for (int i = 0; i < 40; i++) {
        double t0 = now_s();
        panel_tab_cycle(p, 1);
        worst = MAX(worst, now_s() - t0);
        PanelTab *t = tab_cur(p);
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

/* ── driver ───────────────────────────────────────────────────────── */

static gboolean run_tests(gpointer data)
{
    FM *fm = data;
    pump(200);
    test_local_tabs(fm);
    const char *work = g_getenv("FM_SFTP_DIR");
    if (work && g_getenv("FM_SFTP_PORT")) test_sftp_tabs(fm, work);
    else g_print("skip sftp tabs (no test sshd)\n");
    test_devices(fm);
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
