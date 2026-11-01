/* devices.c – drive / device picker (like the drive button of
 * Double Commander / Total Commander).
 *
 * Every panel has a button left of the path bar; Alt+F1 / Alt+F2 open it
 * for the left / right panel.  The list shows the file system root, home,
 * every mounted device and the volumes that are not mounted yet (USB
 * sticks, partitions, encrypted disks, phones, ...).  Activating an
 * unmounted volume mounts it (asking for a passphrase when needed) and
 * opens it; the button next to a mounted device unmounts / ejects it.
 *
 * Panels standing inside a device that goes away move to home; newly
 * plugged devices are announced in the status bar. */

#include "fm.h"
#include <string.h>

static GVolumeMonitor *vmon;

/* ── helpers ──────────────────────────────────────────────────────── */

static gchar *mount_path(GMount *m)
{
    GFile *root = g_mount_get_root(m);
    gchar *path = g_file_get_path(root);    /* NULL = not a local path */
    g_object_unref(root);
    return path;
}

static gboolean path_under(const char *path, const char *root)
{
    if (!path || !root || !path[0]) return FALSE;
    if (strcmp(root, "/") == 0) return FALSE;     /* never "leave" / */
    size_t n = strlen(root);
    return strncmp(path, root, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

/* Move every local panel (and tab) out of root, so the device can go */
static void panels_leave(FM *fm, const char *root)
{
    for (int i = 0; i < 2; i++) {
        Panel *p = &fm->panels[i];
        if (p->tabs) {
            for (guint t = 0; t < p->tabs->len; t++) {
                PanelTab *tab = g_ptr_array_index(p->tabs, t);
                if (path_under(tab->path, root)) {
                    g_free(tab->path);
                    tab->path = g_strdup(g_get_home_dir());
                    g_clear_pointer(&tab->cursor_name, g_free);
                }
            }
        }
        if (p->ssh_conn) continue;
        const char *dir = p->search_mode ? p->search_prev_cwd : p->cwd;
        if (path_under(dir, root) || (p->arc_file && path_under(p->arc_file, root))) {
            panel_monitor_stop(p);
            panel_load(p, g_get_home_dir());
        }
    }
}

static void open_in_panel(Panel *p, const char *path)
{
    if (p->ssh_conn) panel_tab_set_conn(p, NULL);   /* this tab goes local */
    panel_load(p, path);
    panel_focus(p);
}

/* ── mount / unmount ──────────────────────────────────────────────── */

typedef struct {
    Panel *p;
    gchar *name;
} DevOp;

static void devop_free(DevOp *op)
{
    g_free(op->name);
    g_free(op);
}

static void on_volume_mounted(GObject *src, GAsyncResult *res, gpointer data)
{
    DevOp *op = data;
    GVolume *v = G_VOLUME(src);
    GError *err = NULL;
    if (!g_volume_mount_finish(v, res, &err)) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
            fm_status(op->p->fm, "Cannot mount %s: %s", op->name, err->message);
        g_clear_error(&err);
        devop_free(op);
        return;
    }
    GMount *m = g_volume_get_mount(v);
    gchar *path = m ? mount_path(m) : NULL;
    if (path) {
        fm_status(op->p->fm, "Mounted %s at %s", op->name, path);
        open_in_panel(op->p, path);
    } else {
        fm_status(op->p->fm, "Mounted %s (no local path)", op->name);
    }
    g_free(path);
    if (m) g_object_unref(m);
    devop_free(op);
}

static void volume_mount(Panel *p, GVolume *v)
{
    GMount *m = g_volume_get_mount(v);
    if (m) {                            /* mounted meanwhile */
        gchar *path = mount_path(m);
        if (path) open_in_panel(p, path);
        g_free(path);
        g_object_unref(m);
        return;
    }
    DevOp *op = g_new0(DevOp, 1);
    op->p = p;
    op->name = g_volume_get_name(v);
    fm_status(p->fm, "Mounting %s …", op->name);
    GMountOperation *mo = gtk_mount_operation_new(GTK_WINDOW(p->fm->window));
    g_volume_mount(v, G_MOUNT_MOUNT_NONE, mo, NULL, on_volume_mounted, op);
    g_object_unref(mo);
}

static void on_unmounted(GObject *src, GAsyncResult *res, gpointer data)
{
    DevOp *op = data;
    GMount *m = G_MOUNT(src);
    GError *err = NULL;
    gboolean ok = g_object_get_data(G_OBJECT(m), "fm-eject")
        ? g_mount_eject_with_operation_finish(m, res, &err)
        : g_mount_unmount_with_operation_finish(m, res, &err);
    if (ok)
        fm_status(op->p->fm, "%s can be removed safely", op->name);
    else if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        fm_status(op->p->fm, "Cannot unmount %s: %s", op->name, err->message);
    g_clear_error(&err);
    devop_free(op);
}

static void mount_unmount(Panel *p, GMount *m)
{
    gchar *root = mount_path(m);
    if (root) panels_leave(p->fm, root);   /* our own handles keep it busy */
    g_free(root);

    DevOp *op = g_new0(DevOp, 1);
    op->p = p;
    op->name = g_mount_get_name(m);
    fm_status(p->fm, "Unmounting %s …", op->name);
    GMountOperation *mo = gtk_mount_operation_new(GTK_WINDOW(p->fm->window));
    if (g_mount_can_eject(m)) {
        g_object_set_data(G_OBJECT(m), "fm-eject", GINT_TO_POINTER(1));
        g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, mo, NULL,
                                     on_unmounted, op);
    } else {
        g_object_set_data(G_OBJECT(m), "fm-eject", NULL);
        g_mount_unmount_with_operation(m, G_MOUNT_UNMOUNT_NONE, mo, NULL,
                                       on_unmounted, op);
    }
    g_object_unref(mo);
}

/* ── the list ─────────────────────────────────────────────────────── */

static void popdown(Panel *p)
{
    gtk_menu_button_popdown(GTK_MENU_BUTTON(p->dev_btn));
}

static void on_row_activated(GtkListBox *lb, GtkListBoxRow *row, Panel *p)
{
    (void)lb;
    const char *path = g_object_get_data(G_OBJECT(row), "fm-path");
    GVolume *v = g_object_get_data(G_OBJECT(row), "fm-volume");
    popdown(p);
    if (path)   open_in_panel(p, path);
    else if (v) volume_mount(p, v);
}

static void on_unmount_clicked(GtkButton *b, Panel *p)
{
    GMount *m = g_object_get_data(G_OBJECT(b), "fm-mount");
    popdown(p);
    if (m) mount_unmount(p, m);
}

static void add_row(Panel *p, GIcon *icon, const char *icon_name,
                    const char *name, const char *detail,
                    const char *path, GVolume *vol, GMount *unmount)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(box, 4);
    gtk_widget_set_margin_end(box, 4);
    gtk_widget_set_margin_top(box, 3);
    gtk_widget_set_margin_bottom(box, 3);

    GtkWidget *img = icon ? gtk_image_new_from_gicon(icon)
                          : gtk_image_new_from_icon_name(icon_name);
    gtk_box_append(GTK_BOX(box), img);

    GtkWidget *txt = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(txt, TRUE);
    GtkWidget *l1 = gtk_label_new(name);
    gtk_widget_set_halign(l1, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(txt), l1);
    if (detail) {
        GtkWidget *l2 = gtk_label_new(detail);
        gtk_widget_set_halign(l2, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(l2), PANGO_ELLIPSIZE_MIDDLE);
        gtk_label_set_max_width_chars(GTK_LABEL(l2), 40);
        gtk_widget_add_css_class(l2, "dim-label");
        gtk_widget_add_css_class(l2, "caption");
        gtk_box_append(GTK_BOX(txt), l2);
    }
    gtk_box_append(GTK_BOX(box), txt);

    if (unmount) {
        gboolean ej = g_mount_can_eject(unmount);
        GtkWidget *b = gtk_button_new_from_icon_name("media-eject-symbolic");
        gtk_widget_set_tooltip_text(b, ej ? "Eject" : "Unmount");
        gtk_widget_add_css_class(b, "flat");
        gtk_widget_set_valign(b, GTK_ALIGN_CENTER);
        gtk_widget_set_focusable(b, FALSE);
        g_object_set_data_full(G_OBJECT(b), "fm-mount",
                               g_object_ref(unmount), g_object_unref);
        g_signal_connect(b, "clicked", G_CALLBACK(on_unmount_clicked), p);
        gtk_box_append(GTK_BOX(box), b);
    }

    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    if (path)
        g_object_set_data_full(G_OBJECT(row), "fm-path", g_strdup(path), g_free);
    if (vol)
        g_object_set_data_full(G_OBJECT(row), "fm-volume",
                               g_object_ref(vol), g_object_unref);
    if (unmount)    /* Delete / Shift+Delete in the list = unmount */
        g_object_set_data_full(G_OBJECT(row), "fm-mount",
                               g_object_ref(unmount), g_object_unref);
    gtk_list_box_append(GTK_LIST_BOX(p->dev_list), row);
}

static void add_separator(Panel *p)
{
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row),
                               gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_set_focusable(row, FALSE);
    gtk_list_box_append(GTK_LIST_BOX(p->dev_list), row);
}

static void devices_fill(Panel *p)
{
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(p->dev_list)))
        gtk_list_box_remove(GTK_LIST_BOX(p->dev_list), c);

    add_row(p, NULL, "drive-harddisk-symbolic", "File System", "/", "/", NULL, NULL);
    add_row(p, NULL, "user-home-symbolic", "Home", g_get_home_dir(),
            g_get_home_dir(), NULL, NULL);

    gboolean sep = FALSE;

    /* mounted devices */
    GList *mounts = g_volume_monitor_get_mounts(vmon);
    for (GList *l = mounts; l; l = l->next) {
        GMount *m = l->data;
        if (g_mount_is_shadowed(m)) continue;
        gchar *path = mount_path(m);
        if (!path || strcmp(path, "/") == 0) { g_free(path); continue; }
        if (!sep) { add_separator(p); sep = TRUE; }
        gchar *name = g_mount_get_name(m);
        GIcon *icon = g_mount_get_symbolic_icon(m);
        gboolean can = g_mount_can_unmount(m) || g_mount_can_eject(m);
        add_row(p, icon, NULL, name, path, path, NULL, can ? m : NULL);
        g_object_unref(icon);
        g_free(name);
        g_free(path);
    }
    g_list_free_full(mounts, g_object_unref);

    /* volumes not mounted yet */
    GList *vols = g_volume_monitor_get_volumes(vmon);
    for (GList *l = vols; l; l = l->next) {
        GVolume *v = l->data;
        GMount *m = g_volume_get_mount(v);
        if (m) { g_object_unref(m); continue; }
        if (!g_volume_can_mount(v)) continue;
        if (!sep) { add_separator(p); sep = TRUE; }
        gchar *name = g_volume_get_name(v);
        GIcon *icon = g_volume_get_symbolic_icon(v);
        add_row(p, icon, NULL, name, "Not mounted – click to mount",
                NULL, v, NULL);
        g_object_unref(icon);
        g_free(name);
    }
    g_list_free_full(vols, g_object_unref);

    /* preselect the device the panel is on */
    const char *cwd = p->ssh_conn ? NULL : p->cwd;
    GtkListBoxRow *best = NULL;
    size_t best_len = 0;
    for (int i = 0; ; i++) {
        GtkListBoxRow *r = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->dev_list), i);
        if (!r) break;
        const char *rp = g_object_get_data(G_OBJECT(r), "fm-path");
        if (!rp || !cwd) continue;
        size_t n = strlen(rp);
        if ((strcmp(rp, "/") == 0 || path_under(cwd, rp)) && n >= best_len) {
            best = r;
            best_len = n;
        }
    }
    if (!best) best = gtk_list_box_get_row_at_index(GTK_LIST_BOX(p->dev_list), 0);
    gtk_list_box_select_row(GTK_LIST_BOX(p->dev_list), best);
}

static void on_popover_show(GtkWidget *pop, Panel *p)
{
    (void)pop;
    devices_fill(p);
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->dev_list));
    if (r) gtk_widget_grab_focus(GTK_WIDGET(r));
}

static void on_popover_closed(GtkPopover *pop, Panel *p)
{
    (void)pop;
    panel_focus(p);
}

/* Delete in the list unmounts the selected device */
static gboolean on_list_key(GtkEventControllerKey *k, guint keyval, guint code,
                            GdkModifierType state, Panel *p)
{
    (void)k; (void)code; (void)state;
    if (keyval != GDK_KEY_Delete && keyval != GDK_KEY_KP_Delete) return FALSE;
    GtkListBoxRow *r = gtk_list_box_get_selected_row(GTK_LIST_BOX(p->dev_list));
    GMount *m = r ? g_object_get_data(G_OBJECT(r), "fm-mount") : NULL;
    if (!m) return TRUE;
    g_object_ref(m);
    popdown(p);
    mount_unmount(p, m);
    g_object_unref(m);
    return TRUE;
}

GtkWidget *devices_button_new(Panel *p)
{
    p->dev_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(p->dev_btn),
                                  "drive-harddisk-symbolic");
    gtk_widget_set_tooltip_text(p->dev_btn,
                                p->idx == 0 ? "Devices (Alt+F1)" : "Devices (Alt+F2)");
    gtk_widget_set_focusable(p->dev_btn, FALSE);

    p->dev_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(p->dev_list), GTK_SELECTION_BROWSE);
    gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(p->dev_list), TRUE);
    g_signal_connect(p->dev_list, "row-activated", G_CALLBACK(on_row_activated), p);
    GtkEventController *kc = gtk_event_controller_key_new();
    g_signal_connect(kc, "key-pressed", G_CALLBACK(on_list_key), p);
    gtk_widget_add_controller(p->dev_list, kc);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sw), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw), 500);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), p->dev_list);

    GtkWidget *pop = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(pop), sw);
    g_signal_connect(pop, "show", G_CALLBACK(on_popover_show), p);
    g_signal_connect(pop, "closed", G_CALLBACK(on_popover_closed), p);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(p->dev_btn), pop);
    return p->dev_btn;
}

/* Alt+F1 / Alt+F2 */
void devices_popup(Panel *p)
{
    if (p->dev_btn) gtk_menu_button_popup(GTK_MENU_BUTTON(p->dev_btn));
}

/* ── hot-plug ─────────────────────────────────────────────────────── */

static void on_volume_added(GVolumeMonitor *m, GVolume *v, FM *fm)
{
    (void)m;
    gchar *name = g_volume_get_name(v);
    fm_status(fm, "New device: %s (Alt+F1 / Alt+F2 to open)", name);
    g_free(name);
}

static void on_mount_added(GVolumeMonitor *mon, GMount *m, FM *fm)
{
    (void)mon;
    if (g_mount_is_shadowed(m)) return;
    gchar *name = g_mount_get_name(m);
    gchar *path = mount_path(m);
    if (path) fm_status(fm, "Mounted: %s at %s", name, path);
    g_free(path);
    g_free(name);
}

static void on_mount_gone(GVolumeMonitor *mon, GMount *m, FM *fm)
{
    (void)mon;
    gchar *path = mount_path(m);
    if (path) panels_leave(fm, path);
    g_free(path);
}

void devices_init(FM *fm)
{
    if (vmon) return;
    vmon = g_volume_monitor_get();
    g_signal_connect(vmon, "volume-added", G_CALLBACK(on_volume_added), fm);
    g_signal_connect(vmon, "mount-added", G_CALLBACK(on_mount_added), fm);
    g_signal_connect(vmon, "mount-pre-unmount", G_CALLBACK(on_mount_gone), fm);
    g_signal_connect(vmon, "mount-removed", G_CALLBACK(on_mount_gone), fm);
}
