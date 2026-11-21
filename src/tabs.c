/* Panel tabs (Ctrl+T / Ctrl+W / Ctrl+Tab), split out of main.c */
#include "fm.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Tabs – each tab remembers a local directory and the cursor item;
 *  the panel's widgets are shared, switching simply reloads the panel.
 * ─────────────────────────────────────────────────────────────────── */


void panel_tab_free(gpointer data)
{
    PanelTab *t = data;
    g_free(t->path);
    g_free(t->cursor_name);
    g_free(t->rpath);
    ssh_conn_unref(t->conn);
    g_free(t);
}

PanelTab *panel_tab_item_new(const char *path)
{
    PanelTab *t = g_new0(PanelTab, 1);
    t->path = g_strdup(path);
    return t;
}

PanelTab *panel_tab_cur(Panel *p)
{
    return g_ptr_array_index(p->tabs, p->tab_active);
}

/* The directory the panel is "in": search results count as the
 * directory the search started from (results stay on Alt+Left). */
static const char *tab_dir(Panel *p)
{
    return p->search_mode ? p->search_prev_cwd : p->cwd;
}

static void tabs_rebuild(Panel *p);
static gboolean tabs_refused(Panel *p);

static void on_tab_clicked(GtkButton *btn, Panel *p)
{
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "fm-tab-idx"));
    set_active_panel(p->fm, p->idx);
    panel_tab_cycle(p, (int)i - (int)p->tab_active);
    panel_focus(p);
}

static void on_tab_middle(GtkGestureClick *g, int n_press, double x, double y,
                          Panel *p)
{
    (void)n_press; (void)x; (void)y;
    GtkWidget *btn = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(btn), "fm-tab-idx"));
    set_active_panel(p->fm, p->idx);
    if (i == p->tab_active) { panel_tab_close(p); return; }
    /* another tab: drop it without loading it first (the visible tab
     * stays; switching there and closing landed on a neighbour) */
    if (tabs_refused(p) || i >= p->tabs->len) return;
    g_ptr_array_remove_index(p->tabs, i);
    if (i < p->tab_active) p->tab_active--;
    tabs_rebuild(p);
}

/* Tabs are written to settings shortly after every change (not only
 * on exit), so they survive a crash or a killed session too. */
static guint tabs_save_id;

/* drop a pending delayed save (the caller saves right away) */
void panel_tabs_cancel_save(void)
{
    if (tabs_save_id) { g_source_remove(tabs_save_id); tabs_save_id = 0; }
}

static gboolean tabs_save_cb(gpointer fm)
{
    tabs_save_id = 0;
    settings_save_tabs(fm);
    return G_SOURCE_REMOVE;
}

/* Recreate the tab buttons; the strip is hidden with a single tab */
static void tabs_rebuild(Panel *p)
{
    if (!p->tab_box) return;
    if (tabs_save_id) g_source_remove(tabs_save_id);
    tabs_save_id = g_timeout_add_seconds(1, tabs_save_cb, p->fm);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(p->tab_box)))
        gtk_box_remove(GTK_BOX(p->tab_box), c);

    for (guint i = 0; i < p->tabs->len; i++) {
        PanelTab *t = g_ptr_array_index(p->tabs, i);
        const char *path = t->path ? t->path : "/";
        GtkWidget *b;
        if (t->conn) {
            const char *rp = t->rpath ? t->rpath : "/";
            gchar *base = g_path_get_basename(rp);
            gchar *lbl = g_strdup_printf("%s:%s", t->conn->host, base);
            gchar *tip = g_strdup_printf("sftp://%s@%s%s",
                                         t->conn->user, t->conn->host, rp);
            b = gtk_button_new_with_label(lbl);
            gtk_widget_set_tooltip_text(b, tip);
            g_free(base); g_free(lbl); g_free(tip);
        } else {
            gchar *base = g_path_get_basename(path);
            b = gtk_button_new_with_label(base);
            g_free(base);
            gtk_widget_set_tooltip_text(b, path);
        }
        gtk_widget_set_focusable(b, FALSE);
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_add_css_class(b, "fm-tab");
        if (i == p->tab_active) gtk_widget_add_css_class(b, "fm-tab-active");
        g_object_set_data(G_OBJECT(b), "fm-tab-idx", GUINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(on_tab_clicked), p);

        GtkGesture *mc = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mc), GDK_BUTTON_MIDDLE);
        g_signal_connect(mc, "pressed", G_CALLBACK(on_tab_middle), p);
        gtk_widget_add_controller(b, GTK_EVENT_CONTROLLER(mc));

        gtk_box_append(GTK_BOX(p->tab_box), b);
    }
    gtk_widget_set_visible(p->tab_scroll, p->tabs->len > 1);
}

/* Called by panel_load on success: the active tab follows navigation */
static void tab_track(Panel *p)
{
    PanelTab *t = panel_tab_cur(p);
    if (p->ssh_conn) {
        if (g_strcmp0(t->rpath, p->ssh_conn->remote_path) == 0) return;
        g_free(t->rpath);
        t->rpath = g_strdup(p->ssh_conn->remote_path);
        g_clear_pointer(&t->cursor_name, g_free);
        tabs_rebuild(p);
        return;
    }
    if (g_strcmp0(t->path, p->cwd) == 0) return;
    g_free(t->path);
    t->path = g_strdup(p->cwd);
    g_clear_pointer(&t->cursor_name, g_free);
    tabs_rebuild(p);
}

/* Remember the cursor item of the active tab before leaving it */
static void tab_remember(Panel *p)
{
    PanelTab *t = panel_tab_cur(p);
    g_free(t->cursor_name);
    t->cursor_name = p->search_mode ? g_strdup(p->search_prev_name)
                   : p->arc_file    ? g_path_get_basename(p->arc_file)
                                    : panel_cursor_name(p);
    if (p->ssh_conn) {
        g_free(t->rpath);
        t->rpath = g_strdup(p->ssh_conn->remote_path);
        return;                 /* t->path keeps the local fallback */
    }
    g_free(t->path);
    t->path = g_strdup(tab_dir(p));
}

void panel_tab_track(Panel *p)
{
    if (p->tabs && p->tabs->len) tab_track(p);
}

/* Make conn (may be NULL) the connection of the active tab; the panel
 * follows.  NULL puts the tab back on its local directory. */
void panel_tab_set_conn(Panel *p, SshConn *conn)
{
    PanelTab *t = panel_tab_cur(p);
    panel_monitor_stop(p);
    SshConn *old = t->conn;
    t->conn = ssh_conn_ref(conn);
    p->ssh_conn = conn;
    ssh_conn_unref(old);
    g_clear_pointer(&t->rpath, g_free);
    gtk_widget_set_visible(p->disconnect_btn, conn != NULL);
    if (!conn) panel_load(p, t->path ? t->path : g_get_home_dir());
    tabs_rebuild(p);
}

/* Disconnect: all tabs using conn become local again */
void panel_tabs_drop_conn(Panel *p, SshConn *conn)
{
    panel_monitor_stop(p);
    gboolean active = (p->ssh_conn == conn);
    if (active) p->ssh_conn = NULL;
    ssh_conn_ref(conn);             /* keep it alive while unhooking */
    for (guint i = 0; i < p->tabs->len; i++) {
        PanelTab *t = g_ptr_array_index(p->tabs, i);
        if (t->conn != conn) continue;
        ssh_conn_unref(t->conn);
        t->conn = NULL;
        g_clear_pointer(&t->rpath, g_free);
        g_clear_pointer(&t->cursor_name, g_free);
    }
    ssh_conn_unref(conn);           /* closes the connection */
    if (active) {
        gtk_widget_set_visible(p->disconnect_btn, FALSE);
        PanelTab *t = panel_tab_cur(p);
        panel_load(p, t->path ? t->path : g_get_home_dir());
    }
    tabs_rebuild(p);
}

/* Show tab i: load its directory and put the cursor back */
static void tab_show(Panel *p, guint i)
{
    p->tab_active = i;
    PanelTab *t = panel_tab_cur(p);
    gchar *name = g_strdup(t->cursor_name);

    /* the panel takes the connection of the tab (or none) */
    ssh_connect_cancel(p);
    panel_monitor_stop(p);
    if (p->ssh_conn != t->conn && p->marks)
        g_hash_table_remove_all(p->marks);
    p->ssh_conn = t->conn;
    gtk_widget_set_visible(p->disconnect_btn, t->conn != NULL);

    if (t->conn) {
        gchar *rpath = g_strdup(t->rpath ? t->rpath : "/");
        g_clear_pointer(&t->conn->remote_path, g_free);
        panel_load_remote(p, rpath);
        if (!t->conn->remote_path) {
            /* connection gone or directory unreadable */
            fm_status(p->fm, "SFTP: %s is not reachable – tab disconnected",
                      t->conn->host);
            g_free(rpath);
            g_free(name);
            panel_tabs_drop_conn(p, t->conn);
            return;
        }
        if (name && strcmp(t->conn->remote_path, rpath) == 0)
            panel_select_name(p, name);
        g_free(rpath);
        g_free(name);
        tabs_rebuild(p);
        return;
    }

    gchar *path = g_strdup(t->path ? t->path : g_get_home_dir());
    panel_load(p, path);
    if (strcmp(p->cwd, path) != 0 || p->search_mode) {
        /* directory gone (or load cancelled): the tab takes what is shown */
        g_free(t->path);
        t->path = g_strdup(tab_dir(p));
    } else if (name) {
        panel_select_name(p, name);
    }
    g_free(path);
    g_free(name);
    tabs_rebuild(p);
}

static gboolean tabs_refused(Panel *p)
{
    return p->busy;
}

/* Ctrl+T – new tab after the active one (path NULL = current directory).
 * On an SFTP panel the new tab shares the connection and path is remote. */
void panel_tab_new(Panel *p, const char *path)
{
    if (tabs_refused(p)) return;
    PanelTab *nt;
    if (p->ssh_conn) {
        nt = panel_tab_item_new(panel_tab_cur(p)->path ? panel_tab_cur(p)->path : g_get_home_dir());
        nt->conn  = ssh_conn_ref(p->ssh_conn);
        nt->rpath = g_strdup(path ? path : p->ssh_conn->remote_path);
        tab_remember(p);
    } else {
        gchar *dir = g_strdup(path ? path : tab_dir(p));
        tab_remember(p);
        nt = panel_tab_item_new(dir);
        g_free(dir);
    }
    g_ptr_array_insert(p->tabs, (int)p->tab_active + 1, nt);
    tab_show(p, p->tab_active + 1);
}

/* Ctrl+W – close the active tab (never the last one) */
void panel_tab_close(Panel *p)
{
    if (tabs_refused(p)) return;
    if (p->tabs->len < 2) { fm_status(p->fm, "Cannot close the last tab"); return; }
    /* the panel must not keep using a connection the tab may close */
    panel_monitor_stop(p);
    p->ssh_conn = NULL;
    g_ptr_array_remove_index(p->tabs, p->tab_active);
    tab_show(p, MIN(p->tab_active, p->tabs->len - 1));
}

/* Ctrl+Tab / Ctrl+Shift+Tab – move delta tabs, wrapping around */
void panel_tab_cycle(Panel *p, int delta)
{
    if (p->tabs->len < 2 || delta == 0) return;
    if (tabs_refused(p)) return;
    int n = (int)p->tabs->len;
    int i = (((int)p->tab_active + delta) % n + n) % n;
    tab_remember(p);
    tab_show(p, (guint)i);
}

/* Startup: tabs from settings (missing directories skipped), then the
 * active one is loaded.  Always leaves at least one tab. */
void panel_tabs_restore(Panel *p, gchar **paths, int active)
{
    g_ptr_array_set_size(p->tabs, 0);
    int skipped_before = 0;
    for (int i = 0; paths && paths[i]; i++) {
        if (paths[i][0] == '/' && g_file_test(paths[i], G_FILE_TEST_IS_DIR))
            g_ptr_array_add(p->tabs, panel_tab_item_new(paths[i]));
        else if (i < active)
            skipped_before++;
    }
    if (p->tabs->len == 0) {
        g_ptr_array_add(p->tabs, panel_tab_item_new(g_get_home_dir()));
        active = 0;
    }
    active -= skipped_before;
    if (active < 0 || active >= (int)p->tabs->len) active = 0;
    tab_show(p, (guint)active);
}

void panel_tabs_free(Panel *p)
{
    panel_monitor_stop(p);
    p->ssh_conn = NULL;
    g_clear_pointer(&p->tabs, g_ptr_array_unref);
}
