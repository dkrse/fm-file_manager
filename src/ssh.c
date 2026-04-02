#include "fm.h"
#include <string.h>
#include <stdlib.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  SSH / SFTP panel – full libssh2 implementation when available,
 *  informative stub otherwise.
 * ─────────────────────────────────────────────────────────────────── */

#ifdef HAVE_LIBSSH2
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

/* ── SshConn lifecycle ────────────────────────────────────────────── */

static SshConn *ssh_conn_new(const char *host, const char *user,
                              int port, const char *password,
                              const char *key_path,
                              gchar **out_error)
{
    *out_error = NULL;

    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *ai = NULL;
    gchar port_str[16];
    g_snprintf(port_str, sizeof(port_str), "%d", port);
    if (getaddrinfo(host, port_str, &hints, &ai) != 0 || !ai) {
        *out_error = g_strdup_printf("Failed to resolve host: %s", host);
        return NULL;
    }

    int sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(ai);
        *out_error = g_strdup("Error: cannot create socket");
        return NULL;
    }
    if (connect(sock, ai->ai_addr, ai->ai_addrlen) != 0) {
        freeaddrinfo(ai);
        close(sock);
        *out_error = g_strdup_printf("Failed to connect to %s:%d", host, port);
        return NULL;
    }
    freeaddrinfo(ai);

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        close(sock);
        *out_error = g_strdup("Error: libssh2_session_init failed");
        return NULL;
    }

    libssh2_session_set_blocking(session, 1);

    if (libssh2_session_handshake(session, sock) != 0) {
        libssh2_session_free(session);
        close(sock);
        *out_error = g_strdup("SSH handshake failed");
        return NULL;
    }

    int auth_ok = 0;
    if (password && password[0])
        auth_ok = (libssh2_userauth_password(session, user, password) == 0);

    if (!auth_ok) {
        gchar *priv = NULL;
        gchar *pub  = NULL;
        if (key_path && key_path[0]) {
            priv = g_strdup(key_path);
            pub  = g_strdup_printf("%s.pub", key_path);
        } else {
            const gchar *home = g_get_home_dir();
            priv = g_build_filename(home, ".ssh", "id_rsa",     NULL);
            pub  = g_build_filename(home, ".ssh", "id_rsa.pub", NULL);
        }
        auth_ok = (libssh2_userauth_publickey_fromfile(
                       session, user, pub, priv, "") == 0);
        g_free(pub); g_free(priv);
    }

    if (!auth_ok) {
        libssh2_session_disconnect(session, "Auth failed");
        libssh2_session_free(session);
        close(sock);
        *out_error = g_strdup("Authentication failed (password + key)");
        return NULL;
    }

    LIBSSH2_SFTP *sftp = libssh2_sftp_init(session);
    if (!sftp) {
        libssh2_session_disconnect(session, "SFTP init failed");
        libssh2_session_free(session);
        close(sock);
        *out_error = g_strdup("Failed to initialize SFTP subsystem");
        return NULL;
    }

    SshConn *conn     = g_new0(SshConn, 1);
    conn->sock        = sock;
    conn->session     = session;
    conn->sftp        = sftp;
    conn->host        = g_strdup(host);
    conn->user        = g_strdup(user);
    conn->remote_path = g_strdup("/");
    conn->port        = port;
    return conn;
}

void ssh_panel_close(Panel *p)
{
    panel_monitor_stop(p);

    SshConn *conn = p->ssh_conn;
    if (!conn) return;

    libssh2_sftp_shutdown(conn->sftp);
    libssh2_session_disconnect(conn->session, "Closing");
    libssh2_session_free(conn->session);
    close(conn->sock);

    g_free(conn->host);
    g_free(conn->user);
    g_free(conn->remote_path);
    g_free(conn);
    p->ssh_conn = NULL;
    gtk_widget_set_visible(p->disconnect_btn, FALSE);

    panel_load(p, g_get_home_dir());
}

/* ── ssh_read_file ────────────────────────────────────────────────── */

gchar *ssh_read_file(SshConn *conn, const char *path, gsize *out_len)
{
    *out_len = 0;
    if (!conn || !conn->sftp) return NULL;

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(conn->sftp, path,
        LIBSSH2_FXF_READ, 0);
    if (!fh) return NULL;

    GByteArray *ba = g_byte_array_new();
    char buf[32768];
    ssize_t n;
    while ((n = libssh2_sftp_read(fh, buf, sizeof(buf))) > 0)
        g_byte_array_append(ba, (guint8 *)buf, (guint)n);
    libssh2_sftp_close(fh);

    *out_len = ba->len;
    g_byte_array_append(ba, (guint8 *)"\0", 1);
    return (gchar *)g_byte_array_free(ba, FALSE);
}

/* ── ssh_write_file ───────────────────────────────────────────────── */

gboolean ssh_write_file(SshConn *conn, const char *path,
                        const gchar *data, gsize len)
{
    if (!conn || !conn->sftp) return FALSE;

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(conn->sftp, path,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
        LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
        LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH);
    if (!fh) return FALSE;

    gsize written = 0;
    while (written < len) {
        ssize_t r = libssh2_sftp_write(fh, data + written, len - written);
        if (r < 0) { libssh2_sftp_close(fh); return FALSE; }
        written += (gsize)r;
    }
    libssh2_sftp_close(fh);
    return TRUE;
}

/* ── panel_load_remote ────────────────────────────────────────────── */

/* remote_icon removed — using shared icon_for_entry() from main.c */

void panel_load_remote(Panel *p, const char *path)
{
    SshConn *conn = p->ssh_conn;
    if (!conn) return;

    /* Clear marks when entering a different directory */
    if (conn->remote_path && strcmp(conn->remote_path, path) != 0 && p->marks)
        g_hash_table_remove_all(p->marks);

    /* Suppress selection events during load */
    p->inhibit_sel = TRUE;

    gchar *new_path = g_strdup(path);   /* copy before free – path may alias remote_path */
    g_free(conn->remote_path);
    conn->remote_path = new_path;
    path = conn->remote_path;          /* use the safe copy from here on */

    gchar *entry_text = g_strdup_printf("sftp://%s@%s%s",
                                         conn->user, conn->host, path);
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), entry_text);
    g_free(entry_text);

    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(conn->sftp, path);
    if (!handle) {
        fm_status(p->fm, "SFTP: failed to open directory %s", path);
        g_list_store_remove_all(p->store);
        panel_update_status(p);
        p->inhibit_sel = FALSE;
        return;
    }

    /* Collect all items into array first, then splice once (O(n) instead of O(n²)) */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);

    /* ".." entry */
    g_ptr_array_add(items,
        file_item_new("go-up-symbolic", "..", "", "",
                      TRUE, p->fm->dir_color, p->fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                      (gint64)-2, (gint64)0));

    char name_buf[512];
    char longentry[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (libssh2_sftp_readdir_ex(handle, name_buf, sizeof(name_buf),
                                    longentry, sizeof(longentry), &attrs) > 0) {
        if (strcmp(name_buf, ".") == 0 || strcmp(name_buf, "..") == 0)
            continue;
        if (!p->fm->show_hidden && name_buf[0] == '.')
            continue;

        gboolean is_dir  = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                            LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        gboolean is_link = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                            LIBSSH2_SFTP_S_ISLNK(attrs.permissions);
        gboolean is_exec = !is_dir &&
                            (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                            (attrs.permissions & (S_IXUSR | S_IXGRP | S_IXOTH));

        const char *icon = icon_for_entry(name_buf, is_dir, is_link, is_exec);

        gint64 size_raw = (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE)
                          ? (gint64)attrs.filesize : 0;
        gint64 mtime    = (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                          ? (gint64)attrs.mtime : 0;

        gchar *vdc = fm_visible_color(p->fm, p->fm->dir_color);
        const gchar *fg = NULL;
        gint         wt = PANGO_WEIGHT_NORMAL;
        if      (is_dir)  { fg = vdc; wt = p->fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL; }
        else if (is_link) { fg = fm_link_color(p->fm); }
        else if (is_exec) { fg = fm_exec_color(p->fm); }

        /* file_item_new_take takes ownership of size/date strings */
        FileItem *fi = file_item_new_take(icon, name_buf,
                               is_dir ? g_strdup("<DIR>") : fmt_size((goffset)size_raw),
                               fmt_date(mtime),
                               is_dir, fg, wt, size_raw, mtime);
        g_free(vdc);
        if (p->marks && g_hash_table_contains(p->marks, name_buf))
            fi->marked = TRUE;
        g_ptr_array_add(items, fi);
    }
    libssh2_sftp_closedir(handle);

    /* Single splice: one items-changed signal → one sort pass */
    g_list_store_remove_all(p->store);
    g_list_store_splice(p->store, 0, 0,
                        (gpointer *)items->pdata, items->len);
    g_ptr_array_unref(items);

    panel_update_status(p);

    p->cursor_pos = 0;
    if (p->column_view && g_list_model_get_n_items(G_LIST_MODEL(p->sort_model)) > 0) {
        gboolean is_active = (p->fm->active == p->idx);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL,
                                  is_active ? GTK_LIST_SCROLL_SELECT
                                            : GTK_LIST_SCROLL_NONE,
                                  NULL);
    }

    p->inhibit_sel = FALSE;

    /* Start SFTP poll if auto-refresh enabled */
    panel_monitor_start(p);
}

/* ── File browse helper (for SSH key path) ───────────────────────── */

static void on_file_browse_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    GtkFileDialog *fd = GTK_FILE_DIALOG(source);
    GtkWidget *entry = GTK_WIDGET(user_data);
    GFile *file = gtk_file_dialog_open_finish(fd, res, NULL);
    if (file) {
        gchar *path = g_file_get_path(file);
        if (path) {
            gtk_editable_set_text(GTK_EDITABLE(entry), path);
            g_free(path);
        }
        g_object_unref(file);
    }
}

static void on_key_browse_clicked(GtkButton *btn, gpointer user_data)
{
    GtkWidget *entry = GTK_WIDGET(user_data);
    GtkFileDialog *fd = gtk_file_dialog_new();
    gtk_file_dialog_set_title(fd, "Select private key");

    const gchar *home = g_get_home_dir();
    gchar *ssh_dir = g_build_filename(home, ".ssh", NULL);
    GFile *f = g_file_new_for_path(ssh_dir);
    gtk_file_dialog_set_initial_folder(fd, f);
    g_object_unref(f);
    g_free(ssh_dir);

    GtkWidget *toplevel = GTK_WIDGET(gtk_widget_get_root(GTK_WIDGET(btn)));
    gtk_file_dialog_open(fd, GTK_WINDOW(toplevel), NULL,
                          on_file_browse_done, entry);
    g_object_unref(fd);
}

/* ── Connection dialog helpers ───────────────────────────────────── */

typedef struct {
    GtkDropDown   *dropdown;
    GtkStringList *str_list;
    GtkWidget     *name_entry;
    GtkWidget     *host_entry;
    GtkWidget     *user_entry;
    GtkWidget     *port_entry;
    GtkWidget     *key_entry;
    GList         *bookmarks;   /* GList<SshBookmark*>, owned */
    FM            *fm;
    gboolean       block_sel;   /* TRUE while rebuilding dropdown */
} SshDlgData;

static void ssh_dlg_fill_form(SshDlgData *d, SshBookmark *bm);

/* Rebuild GtkStringList items from d->bookmarks */
static void ssh_dlg_dropdown_fill(SshDlgData *d, int select_idx)
{
    d->block_sel = TRUE;

    guint n = g_list_model_get_n_items(G_LIST_MODEL(d->str_list));
    for (guint i = 0; i < n; i++)
        gtk_string_list_remove(d->str_list, 0);

    for (GList *l = d->bookmarks; l; l = l->next) {
        SshBookmark *bm = l->data;
        gchar *label = g_strdup_printf("%s  (%s@%s:%d)",
            bm->name && bm->name[0] ? bm->name : "–",
            bm->user ? bm->user : "",
            bm->host ? bm->host : "",
            bm->port > 0 ? bm->port : 22);
        gtk_string_list_append(d->str_list, label);
        g_free(label);
    }

    d->block_sel = FALSE;

    if (select_idx >= 0 && select_idx < (int)g_list_length(d->bookmarks)) {
        gtk_drop_down_set_selected(d->dropdown, (guint)select_idx);
        /* Explicitly fill form — signal may not fire if index unchanged */
        SshBookmark *bm = g_list_nth_data(d->bookmarks, select_idx);
        if (bm) ssh_dlg_fill_form(d, bm);
    } else {
        gtk_drop_down_set_selected(d->dropdown, GTK_INVALID_LIST_POSITION);
    }
}

static void ssh_dlg_fill_form(SshDlgData *d, SshBookmark *bm)
{
    gtk_editable_set_text(GTK_EDITABLE(d->name_entry), bm->name ? bm->name : "");
    gtk_editable_set_text(GTK_EDITABLE(d->host_entry), bm->host ? bm->host : "");
    gtk_editable_set_text(GTK_EDITABLE(d->user_entry), bm->user ? bm->user : "");
    gchar *ps = g_strdup_printf("%d", bm->port > 0 ? bm->port : 22);
    gtk_editable_set_text(GTK_EDITABLE(d->port_entry), ps);
    g_free(ps);
    gtk_editable_set_text(GTK_EDITABLE(d->key_entry), bm->key_path ? bm->key_path : "");
}

static void on_ssh_dropdown_selected(GObject *obj, GParamSpec *pspec, SshDlgData *d)
{
    (void)pspec;
    if (d->block_sel) return;
    guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (sel == GTK_INVALID_LIST_POSITION) return;
    SshBookmark *bm = g_list_nth_data(d->bookmarks, sel);
    if (bm) ssh_dlg_fill_form(d, bm);
}

static void on_ssh_bm_save(GtkButton *btn, SshDlgData *d)
{
    (void)btn;
    const gchar *name = gtk_editable_get_text(GTK_EDITABLE(d->name_entry));
    const gchar *host = gtk_editable_get_text(GTK_EDITABLE(d->host_entry));
    const gchar *user = gtk_editable_get_text(GTK_EDITABLE(d->user_entry));
    const gchar *ps   = gtk_editable_get_text(GTK_EDITABLE(d->port_entry));
    const gchar *kp   = gtk_editable_get_text(GTK_EDITABLE(d->key_entry));
    if (!host || !host[0]) return;

    int port = atoi(ps && ps[0] ? ps : "22");
    if (port <= 0 || port > 65535) port = 22;
    const gchar *bm_name = (name && name[0]) ? name : host;

    guint sel = gtk_drop_down_get_selected(d->dropdown);
    SshBookmark *bm = (sel != GTK_INVALID_LIST_POSITION)
                      ? g_list_nth_data(d->bookmarks, sel) : NULL;

    if (bm) {
        g_free(bm->name); bm->name = g_strdup(bm_name);
        g_free(bm->host); bm->host = g_strdup(host);
        g_free(bm->user); bm->user = g_strdup(user ? user : "");
        bm->port = port;
        g_free(bm->key_path); bm->key_path = (kp && kp[0]) ? g_strdup(kp) : NULL;
    } else {
        bm = g_new0(SshBookmark, 1);
        bm->name = g_strdup(bm_name);
        bm->host = g_strdup(host);
        bm->user = g_strdup(user ? user : "");
        bm->port = port;
        bm->key_path = (kp && kp[0]) ? g_strdup(kp) : NULL;
        d->bookmarks = g_list_append(d->bookmarks, bm);
    }
    settings_save_ssh_bookmarks(d->fm, d->bookmarks);
    ssh_dlg_dropdown_fill(d, g_list_index(d->bookmarks, bm));
}

static void on_ssh_bm_new(GtkButton *btn, SshDlgData *d)
{
    (void)btn;
    d->block_sel = TRUE;
    gtk_drop_down_set_selected(d->dropdown, GTK_INVALID_LIST_POSITION);
    d->block_sel = FALSE;
    gtk_editable_set_text(GTK_EDITABLE(d->name_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(d->host_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(d->user_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(d->port_entry), "22");
    gtk_editable_set_text(GTK_EDITABLE(d->key_entry), "");
    gtk_widget_grab_focus(d->name_entry);
}

static void on_ssh_bm_delete(GtkButton *btn, SshDlgData *d)
{
    (void)btn;
    guint sel = gtk_drop_down_get_selected(d->dropdown);
    if (sel == GTK_INVALID_LIST_POSITION) return;
    SshBookmark *bm = g_list_nth_data(d->bookmarks, sel);
    if (!bm) return;
    d->bookmarks = g_list_remove(d->bookmarks, bm);
    ssh_bookmark_free(bm);
    settings_save_ssh_bookmarks(d->fm, d->bookmarks);
    guint new_sel = (g_list_length(d->bookmarks) > 0) ? 0 : GTK_INVALID_LIST_POSITION;
    ssh_dlg_dropdown_fill(d, (int)new_sel == (int)GTK_INVALID_LIST_POSITION ? -1 : 0);
}

/* ── Async SSH connect via GTask ──────────────────────────────────── */

typedef struct {
    gchar *host;
    gchar *user;
    gchar *password;
    gchar *key_path;
    int    port;
} SshConnectArgs;

static void ssh_connect_args_free(gpointer p)
{
    SshConnectArgs *a = p;
    g_free(a->host);
    g_free(a->user);
    /* Securely clear password from memory before freeing */
    if (a->password) {
        explicit_bzero(a->password, strlen(a->password));
        g_free(a->password);
    }
    g_free(a->key_path);
    g_free(a);
}

static void ssh_connect_thread(GTask *task, gpointer source,
                                gpointer task_data, GCancellable *cancel)
{
    (void)source; (void)cancel;
    SshConnectArgs *args = task_data;
    gchar *errmsg = NULL;
    SshConn *conn = ssh_conn_new(args->host, args->user, args->port,
                                  args->password, args->key_path, &errmsg);
    if (conn) {
        g_task_return_pointer(task, conn, NULL);
    } else {
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                                "%s", errmsg ? errmsg : "unknown error");
        g_free(errmsg);
    }
}

typedef struct {
    FM    *fm;
    Panel *panel;
    gchar *user;
} SshConnectCtx;

static void ssh_connect_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    SshConnectCtx *ctx = user_data;
    GError *err = NULL;
    SshConn *conn = g_task_propagate_pointer(G_TASK(res), &err);

    if (conn) {
        ctx->panel->ssh_conn = conn;
        gchar *start = g_strdup_printf("/home/%s", ctx->user);
        LIBSSH2_SFTP_ATTRIBUTES a;
        if (libssh2_sftp_stat(conn->sftp, start, &a) != 0) {
            g_free(start);
            start = g_strdup("/");
        }
        panel_load_remote(ctx->panel, start);
        g_free(start);
        gtk_widget_set_visible(ctx->panel->disconnect_btn, TRUE);
        fm_status(ctx->fm, "Connected: %s@%s", conn->user, conn->host);
    } else {
        fm_status(ctx->fm, "SSH error: %s", err ? err->message : "unknown");
        g_clear_error(&err);
    }

    /* Restore focus */
    Panel *fp = &ctx->fm->panels[ctx->fm->active];
    fp->inhibit_sel = TRUE;
    gtk_widget_grab_focus(fp->column_view);

    g_free(ctx->user);
    g_free(ctx);
}

/* ── Connection dialog (full version) ────────────────────────────── */

void ssh_connect_dialog(FM *fm)
{
    SshDlgData *d = g_new0(SshDlgData, 1);
    d->fm        = fm;
    d->bookmarks = settings_load_ssh_bookmarks(fm);

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "SSH / SFTP connections");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 460, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(vbox, 14);
    gtk_widget_set_margin_end(vbox, 14);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    /* ── Form (dropdown is first row) ── */
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_widget_set_margin_bottom(grid, 10);
    gtk_box_append(GTK_BOX(vbox), grid);

    int row = 0;

#define FORM_ROW(lbl_txt, widget, gr, r) \
    { GtkWidget *_l = gtk_label_new(lbl_txt); \
      gtk_widget_set_halign(_l, GTK_ALIGN_END); \
      gtk_grid_attach(GTK_GRID(gr), _l,     0, r, 1, 1); \
      gtk_grid_attach(GTK_GRID(gr), widget, 1, r, 1, 1); }

    /* Saved connections dropdown */
    d->str_list = gtk_string_list_new(NULL);
    d->dropdown = GTK_DROP_DOWN(gtk_drop_down_new(G_LIST_MODEL(d->str_list), NULL));
    gtk_widget_set_hexpand(GTK_WIDGET(d->dropdown), TRUE);
    FORM_ROW("Saved:", GTK_WIDGET(d->dropdown), grid, row++)
    g_signal_connect(d->dropdown, "notify::selected",
                     G_CALLBACK(on_ssh_dropdown_selected), d);

    d->name_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->name_entry), "My connection");
    gtk_widget_set_hexpand(d->name_entry, TRUE);
    FORM_ROW("Name:", d->name_entry, grid, row++)

    d->host_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->host_entry), "192.168.1.10");
    gtk_widget_set_hexpand(d->host_entry, TRUE);
    FORM_ROW("Host:", d->host_entry, grid, row++)

    d->user_entry = gtk_entry_new();
    gtk_widget_set_hexpand(d->user_entry, TRUE);
    FORM_ROW("User:", d->user_entry, grid, row++)

    d->port_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(d->port_entry), "22");
    gtk_widget_set_size_request(d->port_entry, 80, -1);
    FORM_ROW("Port:", d->port_entry, grid, row++)

    GtkWidget *pw_entry = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(pw_entry), TRUE);
    gtk_widget_set_hexpand(pw_entry, TRUE);
    FORM_ROW("Password:", pw_entry, grid, row++)

    d->key_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->key_entry), "~/.ssh/id_rsa");
    gtk_widget_set_hexpand(d->key_entry, TRUE);
    GtkWidget *key_browse = gtk_button_new_with_label("...");
    g_signal_connect(key_browse, "clicked", G_CALLBACK(on_key_browse_clicked), d->key_entry);
    GtkWidget *key_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(key_box), d->key_entry);
    gtk_box_append(GTK_BOX(key_box), key_browse);
    gtk_widget_set_hexpand(key_box, TRUE);
    { GtkWidget *_l = gtk_label_new("Key:");
      gtk_widget_set_halign(_l, GTK_ALIGN_END);
      gtk_grid_attach(GTK_GRID(grid), _l,      0, row, 1, 1);
      gtk_grid_attach(GTK_GRID(grid), key_box, 1, row, 1, 1); }
    row++;

#undef FORM_ROW

    /* Fill dropdown after all form entries exist */
    ssh_dlg_dropdown_fill(d, d->bookmarks ? 0 : -1);

    GtkWidget *hint = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hint),
        "<small>Empty password = SSH key authentication (default: ~/.ssh/id_rsa)</small>");
    gtk_widget_set_halign(hint, GTK_ALIGN_START);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_grid_attach(GTK_GRID(grid), hint, 0, row++, 2, 1);

    /* Disconnect row if active panel is connected */
    Panel *ap = active_panel(fm);
    if (ap->ssh_conn) {
        GtkWidget *disc_btn = gtk_button_new_with_label("Disconnect active panel");
        gtk_widget_add_css_class(disc_btn, "destructive-action");
        gtk_widget_set_margin_top(disc_btn, 4);
        gtk_grid_attach(GTK_GRID(grid), disc_btn, 0, row++, 2, 1);
        g_signal_connect_swapped(disc_btn, "clicked",
                                  G_CALLBACK(ssh_panel_close), ap);
    }

    /* ── Button row ── */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_FILL);
    gtk_box_append(GTK_BOX(vbox), btn_box);

    GtkWidget *new_btn     = gtk_button_new_with_label("New");
    GtkWidget *del_btn     = gtk_button_new_with_label("Remove");
    GtkWidget *save_btn    = gtk_button_new_with_label("Save");
    GtkWidget *close_btn   = gtk_button_new_with_label("Close");
    GtkWidget *connect_btn = gtk_button_new_with_label("Connect");
    gtk_widget_add_css_class(connect_btn, "suggested-action");

    /* left-align new/delete/save, right-align close/connect */
    gtk_box_append(GTK_BOX(btn_box), new_btn);
    gtk_box_append(GTK_BOX(btn_box), del_btn);
    gtk_box_append(GTK_BOX(btn_box), save_btn);
    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(btn_box), spacer);
    gtk_box_append(GTK_BOX(btn_box), close_btn);
    gtk_box_append(GTK_BOX(btn_box), connect_btn);

    g_signal_connect(new_btn, "clicked", G_CALLBACK(on_ssh_bm_new), d);
    g_signal_connect(save_btn,    "clicked",       G_CALLBACK(on_ssh_bm_save),    d);
    g_signal_connect(del_btn,     "clicked",       G_CALLBACK(on_ssh_bm_delete),  d);
    g_signal_connect(connect_btn, "clicked",       G_CALLBACK(dlg_ctx_ok),        &ctx);
    g_signal_connect(close_btn,   "clicked",       G_CALLBACK(dlg_ctx_cancel),    &ctx);
    g_signal_connect(pw_entry,    "activate",      G_CALLBACK(dlg_ctx_entry_ok),  &ctx);
    g_signal_connect(dlg,         "close-request", G_CALLBACK(dlg_ctx_close),     &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    gint response = dlg_ctx_run(&ctx);

    if (response == 1) {
        const gchar *host   = gtk_editable_get_text(GTK_EDITABLE(d->host_entry));
        const gchar *user   = gtk_editable_get_text(GTK_EDITABLE(d->user_entry));
        const gchar *pw     = gtk_editable_get_text(GTK_EDITABLE(pw_entry));
        const gchar *port_s = gtk_editable_get_text(GTK_EDITABLE(d->port_entry));
        int port = atoi(port_s && port_s[0] ? port_s : "22");
        if (port <= 0 || port > 65535) port = 22;
        if (!user || !user[0]) user = "root";

        const gchar *kp = gtk_editable_get_text(GTK_EDITABLE(d->key_entry));
        if (host && host[0]) {
            Panel *p = active_panel(fm);
            if (p->ssh_conn) ssh_panel_close(p);

            fm_status(fm, "Connecting to %s@%s:%d …", user, host, port);

            /* Async connect — UI stays responsive during handshake */
            SshConnectArgs *args = g_new0(SshConnectArgs, 1);
            args->host     = g_strdup(host);
            args->user     = g_strdup(user);
            args->password = g_strdup(pw);
            args->key_path = (kp && kp[0]) ? g_strdup(kp) : NULL;
            args->port     = port;

            SshConnectCtx *cctx = g_new0(SshConnectCtx, 1);
            cctx->fm    = fm;
            cctx->panel = p;
            cctx->user  = g_strdup(user);

            GTask *task = g_task_new(NULL, NULL, ssh_connect_done, cctx);
            g_task_set_task_data(task, args, ssh_connect_args_free);
            g_task_run_in_thread(task, ssh_connect_thread);
            g_object_unref(task);
        }
    }

    gtk_window_destroy(GTK_WINDOW(dlg));
    g_list_free_full(d->bookmarks, ssh_bookmark_free);
    g_free(d);
}

#else  /* !HAVE_LIBSSH2 */

/* ── Stub implementations when libssh2-devel is not installed ─────── */

void panel_load_remote(Panel *p, const char *path)
{
    (void)p; (void)path;
    fm_status(p->fm, "SFTP is not available – install libssh2-devel");
}

void ssh_panel_close(Panel *p)
{
    g_free(p->ssh_conn);
    p->ssh_conn = NULL;
    panel_load(p, g_get_home_dir());
}

gchar *ssh_read_file(SshConn *conn, const char *path, gsize *out_len)
{
    (void)conn; (void)path;
    *out_len = 0;
    return NULL;
}

gboolean ssh_write_file(SshConn *conn, const char *path,
                        const gchar *data, gsize len)
{
    (void)conn; (void)path; (void)data; (void)len;
    return FALSE;
}

void ssh_connect_dialog(FM *fm)
{
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "SFTP is not available");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 400, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);

    GtkWidget *lbl = gtk_label_new(
        "Install libssh2-devel and recompile:\n"
        "  sudo dnf install libssh2-devel\n"
        "  make clean && make");
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);

    GtkWidget *ok_btn = gtk_button_new_with_label("OK");
    gtk_widget_set_halign(ok_btn, GTK_ALIGN_END);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), ok_btn);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn, "clicked",       G_CALLBACK(dlg_ctx_ok),    &ctx);
    g_signal_connect(dlg,    "close-request", G_CALLBACK(dlg_ctx_close), &ctx);

    gtk_window_present(GTK_WINDOW(dlg));
    dlg_ctx_run(&ctx);
    gtk_window_destroy(GTK_WINDOW(dlg));
}

#endif  /* HAVE_LIBSSH2 */
