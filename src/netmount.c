/* Connect to a network server through GIO / gvfs (Ctrl+N): smb://,
 * ftp://, ftps://, dav://, davs://, sftp://, nfs://, afp:// …
 *
 * The location is mounted with g_file_mount_enclosing_volume() (password
 * dialog from GtkMountOperation, "remember password" via the keyring);
 * the panel then opens the mount's local FUSE path below
 * /run/user/<uid>/gvfs, so every panel operation (copy, viewer, editor,
 * search, archives …) works unchanged.  Unmount with ⏏ in the device
 * picker (Alt+F1 / Alt+F2), which lists gvfs mounts. */
#include "fm.h"
#include "job.h"
#include <string.h>

#define NET_GROUP  "network"
#define NET_RECENT "recent"
#define NET_MAX_RECENT 10

typedef struct {
    FM    *fm;
    int    panel;
    gchar *uri;
} NetMount;

static void net_mount_free(NetMount *m)
{
    g_free(m->uri);
    g_free(m);
}

static gchar **recent_load(void)
{
    gchar *s = settings_get_string(NET_GROUP, NET_RECENT);
    gchar **v = s && s[0] ? g_strsplit(s, "\n", -1) : g_new0(gchar *, 1);
    g_free(s);
    return v;
}

static void recent_add(const char *uri)
{
    gchar **old = recent_load();
    GString *s = g_string_new(uri);
    guint n = 1;
    for (gchar **o = old; *o && n < NET_MAX_RECENT; o++) {
        if (!**o || strcmp(*o, uri) == 0) continue;
        g_string_append_c(s, '\n');
        g_string_append(s, *o);
        n++;
    }
    settings_set_string(NET_GROUP, NET_RECENT, s->str);
    g_string_free(s, TRUE);
    g_strfreev(old);
}

static gchar *local_path_of(GFile *f)
{
    gchar *path = g_file_get_path(f);
    if (path) return path;
    /* some backends only map the mount root */
    GMount *m = g_file_find_enclosing_mount(f, NULL, NULL);
    if (!m) return NULL;
    GFile *root = g_mount_get_root(m);
    gchar *rp = g_file_get_path(root);
    gchar *rel = g_file_get_relative_path(root, f);
    if (rp) path = rel ? g_build_filename(rp, rel, NULL) : g_strdup(rp);
    g_free(rp); g_free(rel);
    g_object_unref(root);
    g_object_unref(m);
    return path;
}

static void open_in_panel(NetMount *nm, GFile *f)
{
    FM *fm = nm->fm;
    Panel *p = &fm->panels[nm->panel];
    gchar *path = local_path_of(f);
    if (!path) {
        fm_status(fm, "Connected, but %s has no local path – is gvfs-fuse "
                      "installed and running?", nm->uri);
        return;
    }
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        gchar *d = g_path_get_dirname(path);
        g_free(path);
        path = d;
    }
    recent_add(nm->uri);
    if (job_foreground_running()) {
        fm_status(fm, "Connected to %s (panel busy, not opened)", nm->uri);
        g_free(path);
        return;
    }
    if (p->ssh_conn) panel_tab_new(p, path);   /* keep the SFTP tab */
    else             panel_load(p, path);
    panel_focus(p);
    fm_status(fm, "Connected to %s", nm->uri);
    g_free(path);
}

static void on_mounted(GObject *src, GAsyncResult *res, gpointer d)
{
    NetMount *nm = d;
    GFile *f = G_FILE(src);
    GError *e = NULL;
    if (!g_file_mount_enclosing_volume_finish(f, res, &e) &&
        !g_error_matches(e, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED)) {
        if (!g_error_matches(e, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
            fm_status(nm->fm, "Cannot connect to %s: %s", nm->uri, e->message);
        else
            fm_status(nm->fm, "Connection cancelled");
        g_clear_error(&e);
        net_mount_free(nm);
        return;
    }
    g_clear_error(&e);
    open_in_panel(nm, f);
    net_mount_free(nm);
}

static void net_mount(FM *fm, const char *uri)
{
    NetMount *nm = g_new0(NetMount, 1);
    nm->fm = fm;
    nm->panel = fm->active;
    nm->uri = g_strdup(uri);
    GFile *f = g_file_new_for_uri(uri);
    GMountOperation *op = gtk_mount_operation_new(GTK_WINDOW(fm->window));
    fm_status(fm, "Connecting to %s…", uri);
    g_file_mount_enclosing_volume(f, G_MOUNT_MOUNT_NONE, op, NULL, on_mounted, nm);
    g_object_unref(op);
    g_object_unref(f);
}

/* ── dialog ───────────────────────────────────────────────────────── */

typedef struct {
    DlgCtx     ctx;
    GtkWidget *entry;
} NetDlg;

static void on_recent_activated(GtkListBox *lb, GtkListBoxRow *row, NetDlg *nd)
{
    (void)lb;
    GtkWidget *lbl = gtk_list_box_row_get_child(row);
    gtk_editable_set_text(GTK_EDITABLE(nd->entry), gtk_label_get_text(GTK_LABEL(lbl)));
    nd->ctx.response = 1;
    g_main_loop_quit(nd->ctx.loop);
}

static void on_recent_selected(GtkListBox *lb, GtkListBoxRow *row, NetDlg *nd)
{
    (void)lb;
    if (!row) return;
    GtkWidget *lbl = gtk_list_box_row_get_child(row);
    gtk_editable_set_text(GTK_EDITABLE(nd->entry), gtk_label_get_text(GTK_LABEL(lbl)));
}

static gboolean on_net_key(GtkEventControllerKey *k, guint keyval, guint code,
                           GdkModifierType state, NetDlg *nd)
{
    (void)k; (void)code; (void)state;
    if (keyval == GDK_KEY_Escape) { dlg_ctx_cancel(NULL, &nd->ctx); return TRUE; }
    return FALSE;
}

void net_connect_dialog(FM *fm)
{
    NetDlg nd = { 0 };
    dlg_ctx_init(&nd.ctx);
    gchar **recent = recent_load();

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Connect to server");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, -1);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(box, 14);
    gtk_widget_set_margin_end(box, 14);
    gtk_widget_set_margin_top(box, 12);
    gtk_widget_set_margin_bottom(box, 12);

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl),
        "Server address:\n<small>smb://server/share   ftp://host   davs://host/path   "
        "sftp://user@host   nfs://host/export\n"
        "Without a scheme smb:// is used. The password dialog can remember the "
        "password in the keyring.</small>");
    gtk_label_set_xalign(GTK_LABEL(lbl), 0);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    nd.entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(nd.entry), "smb://server/share");
    if (recent[0]) gtk_editable_set_text(GTK_EDITABLE(nd.entry), recent[0]);
    g_signal_connect(nd.entry, "activate", G_CALLBACK(dlg_ctx_entry_ok), &nd.ctx);
    gtk_box_append(GTK_BOX(box), lbl);
    gtk_box_append(GTK_BOX(box), nd.entry);

    if (recent[0]) {
        GtkWidget *rl = gtk_label_new("Recent:");
        gtk_label_set_xalign(GTK_LABEL(rl), 0);
        gtk_widget_add_css_class(rl, "dim-label");
        GtkWidget *lb = gtk_list_box_new();
        for (gchar **r = recent; *r; r++) {
            if (!**r) continue;
            GtkWidget *l = gtk_label_new(*r);
            gtk_label_set_xalign(GTK_LABEL(l), 0);
            gtk_widget_set_margin_start(l, 6);
            gtk_list_box_append(GTK_LIST_BOX(lb), l);
        }
        g_signal_connect(lb, "row-selected", G_CALLBACK(on_recent_selected), &nd);
        g_signal_connect(lb, "row-activated", G_CALLBACK(on_recent_activated), &nd);
        GtkWidget *sc = gtk_scrolled_window_new();
        gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), lb);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sc), TRUE);
        gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sc), 220);
        gtk_box_append(GTK_BOX(box), rl);
        gtk_box_append(GTK_BOX(box), sc);
    }

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *ok = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(ok, "suggested-action");
    g_signal_connect(cancel, "clicked", G_CALLBACK(dlg_ctx_cancel), &nd.ctx);
    g_signal_connect(ok, "clicked", G_CALLBACK(dlg_ctx_ok), &nd.ctx);
    gtk_box_append(GTK_BOX(row), cancel);
    gtk_box_append(GTK_BOX(row), ok);
    gtk_box_append(GTK_BOX(box), row);
    gtk_window_set_child(GTK_WINDOW(dlg), box);
    g_signal_connect(dlg, "close-request", G_CALLBACK(dlg_ctx_close), &nd.ctx);
    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_net_key), &nd);
    gtk_widget_add_controller(dlg, key);

    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(nd.entry);
    int resp = dlg_ctx_run(&nd.ctx);
    gchar *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(nd.entry)));
    gtk_window_destroy(GTK_WINDOW(dlg));
    fm_refocus_if_lost(fm);
    g_strfreev(recent);

    if (resp == 1) {
        gchar *uri = fs_net_normalize_uri(text);
        if (uri) net_mount(fm, uri);
        else     fm_status(fm, "Enter a server address such as smb://server/share");
        g_free(uri);
    }
    g_free(text);
}
