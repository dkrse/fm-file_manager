#include "fm.h"
#include <string.h>
#include <stdlib.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  SSH / SFTP panel – full libssh2 implementation when available,
 *  informative stub otherwise.
 * ─────────────────────────────────────────────────────────────────── */

#ifdef HAVE_LIBSSH2
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

/* ── Host key verification (~/.ssh/known_hosts) ──────────────────── */

static int knownhost_key_bits(int hostkey_type)
{
    switch (hostkey_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:                             return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

static const char *hostkey_type_name(int t)
{
    switch (t) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:       return "RSA";
    case LIBSSH2_HOSTKEY_TYPE_DSS:       return "DSA";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return "ECDSA-256";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return "ECDSA-384";
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return "ECDSA-521";
    case LIBSSH2_HOSTKEY_TYPE_ED25519:   return "ED25519";
    default:                             return "unknown";
    }
}

static gchar *known_hosts_path(void)
{
    return g_build_filename(g_get_home_dir(), ".ssh", "known_hosts", NULL);
}

/* Load line by line: libssh2_knownhost_readfile() stops at the first
 * line it cannot parse (e.g. @cert-authority, sk-* keys), which would
 * hide every later host.  libssh2 knows no markers: @revoked entries go
 * into a separate list, @cert-authority lines are skipped. */
static void known_hosts_load(LIBSSH2_KNOWNHOSTS *kh, LIBSSH2_KNOWNHOSTS *revoked)
{
    gchar *path = known_hosts_path();
    gchar *data = NULL;
    if (g_file_get_contents(path, &data, NULL, NULL)) {
        gchar **lines = g_strsplit(data, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            const char *l = lines[i];
            while (*l == ' ' || *l == '\t') l++;
            if (!*l || *l == '#') continue;
            LIBSSH2_KNOWNHOSTS *dst = kh;
            if (*l == '@') {
                if (strncmp(l, "@revoked", 8) != 0 || (l[8] != ' ' && l[8] != '\t'))
                    continue;
                l += 8;
                while (*l == ' ' || *l == '\t') l++;
                dst = revoked;
            }
            libssh2_knownhost_readline(dst, l, strlen(l),
                                       LIBSSH2_KNOWNHOST_FILE_OPENSSH);
        }
        g_strfreev(lines);
    }
    g_free(data);
    g_free(path);
}

static gboolean known_hosts_append(LIBSSH2_KNOWNHOSTS *kh,
                                   struct libssh2_knownhost *entry)
{
    char line[8192];
    size_t len = 0;
    if (libssh2_knownhost_writeline(kh, entry, line, sizeof(line), &len,
                                    LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0)
        return FALSE;

    gchar *dir = g_build_filename(g_get_home_dir(), ".ssh", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    gchar *path = known_hosts_path();
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    g_free(path);
    if (fd < 0) return FALSE;
    gboolean ok = write(fd, line, len) == (ssize_t)len;
    close(fd);
    return ok;
}

typedef struct {
    const char *host;
    int         port;
    const char *type;
    const char *fingerprint;
} HostKeyPrompt;

/* Main thread: ask whether to trust an unknown host key */
static gpointer hostkey_prompt_main(gpointer d)
{
    HostKeyPrompt *hp = d;
    FM *fm = g_object_get_data(G_OBJECT(g_application_get_default()), "fm");
    GtkWindow *parent = fm ? GTK_WINDOW(fm->window) : NULL;

    DlgCtx ctx;
    dlg_ctx_init(&ctx);
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Unknown host");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    if (parent) gtk_window_set_transient_for(GTK_WINDOW(dlg), parent);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 480, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 14);
    gtk_widget_set_margin_bottom(vbox, 14);

    gchar *mk = g_markup_printf_escaped(
        "The authenticity of host <b>%s</b> (port %d) can't be established.\n\n"
        "%s key fingerprint:\n<tt>%s</tt>\n\n"
        "Compare it with the server's fingerprint "
        "(<tt>ssh-keygen -lf /etc/ssh/ssh_host_*_key.pub</tt>). "
        "If you trust it, it will be added to ~/.ssh/known_hosts.",
        hp->host, hp->port, hp->type, hp->fingerprint);
    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), mk);
    gtk_label_set_wrap(GTK_LABEL(lbl), TRUE);
    gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    g_free(mk);

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bb, GTK_ALIGN_END);
    GtkWidget *no  = gtk_button_new_with_label("Cancel");
    GtkWidget *yes = gtk_button_new_with_label("Trust and connect");
    gtk_widget_add_css_class(yes, "suggested-action");
    gtk_box_append(GTK_BOX(bb), no);
    gtk_box_append(GTK_BOX(bb), yes);
    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), bb);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(yes, "clicked",       G_CALLBACK(dlg_ctx_ok),     &ctx);
    g_signal_connect(no,  "clicked",       G_CALLBACK(dlg_ctx_cancel), &ctx);
    g_signal_connect(dlg, "close-request", G_CALLBACK(dlg_ctx_close),  &ctx);
    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(no);
    int r = dlg_ctx_run(&ctx);
    gtk_window_destroy(GTK_WINDOW(dlg));
    return GINT_TO_POINTER(r);
}

/* Worker thread.  Returns FALSE (with *err) if the host must not be used. */
static gboolean verify_host_key(LIBSSH2_SESSION *session, const char *host,
                                int port, gchar **err)
{
    size_t klen = 0;
    int ktype = 0;
    const char *key = libssh2_session_hostkey(session, &klen, &ktype);
    if (!key) { *err = g_strdup("Server sent no host key"); return FALSE; }

    const unsigned char *sha = (const unsigned char *)
        libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA256);
    gchar *b64 = sha ? g_base64_encode(sha, 32) : g_strdup("?");
    for (char *e = b64 + strlen(b64); e > b64 && e[-1] == '='; ) *--e = '\0';
    gchar *fp = g_strdup_printf("SHA256:%s", b64);
    g_free(b64);

    LIBSSH2_KNOWNHOSTS *kh  = libssh2_knownhost_init(session);
    LIBSSH2_KNOWNHOSTS *rev = libssh2_knownhost_init(session);
    if (!kh || !rev) {
        if (kh)  libssh2_knownhost_free(kh);
        if (rev) libssh2_knownhost_free(rev);
        g_free(fp); *err = g_strdup("known_hosts init failed"); return FALSE;
    }
    known_hosts_load(kh, rev);

    int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                   knownhost_key_bits(ktype);
    struct libssh2_knownhost *found = NULL;
    int check = libssh2_knownhost_checkp(kh, host, port, key, klen, typemask, &found);

    /* @revoked: refuse even if the key is also listed as trusted */
    if (libssh2_knownhost_checkp(rev, host, port, key, klen, typemask, NULL)
            == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        *err = g_strdup_printf("Host key %s for %s is marked @revoked in "
                               "~/.ssh/known_hosts – refusing to connect", fp, host);
        libssh2_knownhost_free(rev);
        libssh2_knownhost_free(kh);
        g_free(fp);
        return FALSE;
    }
    libssh2_knownhost_free(rev);

    /* NOTFOUND only means "no key of this type": a host known with a key of
     * another type has changed its key type – treat that like a mismatch. */
    gboolean other_type = FALSE;
    if (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        static const int key_types[] = {
            LIBSSH2_KNOWNHOST_KEY_RSA1, LIBSSH2_KNOWNHOST_KEY_SSHRSA,
            LIBSSH2_KNOWNHOST_KEY_SSHDSS, LIBSSH2_KNOWNHOST_KEY_ECDSA_256,
            LIBSSH2_KNOWNHOST_KEY_ECDSA_384, LIBSSH2_KNOWNHOST_KEY_ECDSA_521,
            LIBSSH2_KNOWNHOST_KEY_ED25519 };
        for (gsize i = 0; i < G_N_ELEMENTS(key_types) && !other_type; i++) {
            if (key_types[i] == knownhost_key_bits(ktype)) continue;
            int c = libssh2_knownhost_checkp(kh, host, port, key, klen,
                        LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW |
                        key_types[i], NULL);
            other_type = (c == LIBSSH2_KNOWNHOST_CHECK_MISMATCH ||
                          c == LIBSSH2_KNOWNHOST_CHECK_MATCH);
        }
    }

    gboolean ok = FALSE;
    switch (check) {
    case LIBSSH2_KNOWNHOST_CHECK_MATCH:
        ok = TRUE;
        break;
    case LIBSSH2_KNOWNHOST_CHECK_MISMATCH:
        *err = g_strdup_printf(
            "HOST KEY HAS CHANGED for %s! Someone could be intercepting the "
            "connection (man-in-the-middle). New %s key %s. If the change is "
            "expected, run: ssh-keygen -R %s",
            host, hostkey_type_name(ktype), fp, host);
        break;
    case LIBSSH2_KNOWNHOST_CHECK_NOTFOUND: {
        if (other_type) {
            *err = g_strdup_printf(
                "HOST KEY TYPE HAS CHANGED for %s! known_hosts has a key of a "
                "different type, the server now offers %s key %s. Someone could "
                "be intercepting the connection. If the change is legitimate, "
                "remove the old entry from ~/.ssh/known_hosts "
                "(ssh-keygen -R %s) and connect again.",
                host, hostkey_type_name(ktype), fp, host);
            break;
        }
        HostKeyPrompt hp = { host, port, hostkey_type_name(ktype), fp };
        if (GPOINTER_TO_INT(fm_call_on_main(hostkey_prompt_main, &hp))) {
            gchar *name = (port == 22) ? g_strdup(host)
                                       : g_strdup_printf("[%s]:%d", host, port);
            struct libssh2_knownhost *entry = NULL;
            if (libssh2_knownhost_addc(kh, name, NULL, key, klen, NULL, 0,
                                       typemask, &entry) == 0 && entry)
                known_hosts_append(kh, entry);
            g_free(name);
            ok = TRUE;
        } else {
            *err = g_strdup("Host key not accepted");
        }
        break;
    }
    default:                            /* CHECK_FAILURE → refuse */
        *err = g_strdup_printf("Host key check failed for %s", host);
        break;
    }
    libssh2_knownhost_free(kh);
    g_free(fp);
    return ok;
}

/* ── Authentication ───────────────────────────────────────────────── */

/* keyboard-interactive: answer every prompt with the password */
static __thread const char *kbd_password;

static void kbd_callback(const char *name, int name_len,
                         const char *instr, int instr_len, int num_prompts,
                         const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                         LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                         void **abstract)
{
    (void)name; (void)name_len; (void)instr; (void)instr_len;
    (void)prompts; (void)abstract;
    for (int i = 0; i < num_prompts; i++) {
        /* freed by libssh2 with its default free() */
        responses[i].text   = strdup(kbd_password ? kbd_password : "");
        responses[i].length = (unsigned int)strlen(responses[i].text);
    }
}

static gboolean auth_agent(LIBSSH2_SESSION *session, const char *user)
{
    LIBSSH2_AGENT *agent = libssh2_agent_init(session);
    if (!agent) return FALSE;
    gboolean ok = FALSE;
    if (libssh2_agent_connect(agent) == 0 &&
        libssh2_agent_list_identities(agent) == 0) {
        struct libssh2_agent_publickey *id = NULL, *prev = NULL;
        while (libssh2_agent_get_identity(agent, &id, prev) == 0) {
            if (libssh2_agent_userauth(agent, user, id) == 0) { ok = TRUE; break; }
            prev = id;
        }
        libssh2_agent_disconnect(agent);
    }
    libssh2_agent_free(agent);
    return ok;
}

static gboolean auth_keyfile(LIBSSH2_SESSION *session, const char *user,
                             const char *priv, const char *passphrase)
{
    if (!g_file_test(priv, G_FILE_TEST_IS_REGULAR)) return FALSE;
    gchar *pub = g_strdup_printf("%s.pub", priv);
    const char *pubp = g_file_test(pub, G_FILE_TEST_IS_REGULAR) ? pub : NULL;
    gboolean ok = libssh2_userauth_publickey_fromfile(session, user, pubp, priv,
                                                      "") == 0;
    /* encrypted key: the password field doubles as passphrase */
    if (!ok && passphrase && passphrase[0])
        ok = libssh2_userauth_publickey_fromfile(session, user, pubp, priv,
                                                 passphrase) == 0;
    g_free(pub);
    return ok;
}

/* Order like OpenSSH: agent, key files, password, keyboard-interactive */
static gboolean authenticate(LIBSSH2_SESSION *session, const char *user,
                             const char *password, const char *key_path)
{
    const char *methods = libssh2_userauth_list(session, user, (unsigned)strlen(user));
    if (!methods) return libssh2_userauth_authenticated(session);   /* "none" */

    if (strstr(methods, "publickey")) {
        if (key_path && key_path[0]) {
            gchar *kp = (key_path[0] == '~')
                ? g_build_filename(g_get_home_dir(), key_path + 1, NULL)
                : g_strdup(key_path);
            gboolean ok = auth_keyfile(session, user, kp, password);
            g_free(kp);
            if (ok) return TRUE;
        } else {
            if (auth_agent(session, user)) return TRUE;
            static const char *defaults[] = { "id_ed25519", "id_ecdsa", "id_rsa", NULL };
            for (int i = 0; defaults[i]; i++) {
                gchar *kp = g_build_filename(g_get_home_dir(), ".ssh", defaults[i], NULL);
                gboolean ok = auth_keyfile(session, user, kp, password);
                g_free(kp);
                if (ok) return TRUE;
            }
        }
    }
    if (password && password[0]) {
        if (strstr(methods, "password") &&
            libssh2_userauth_password(session, user, password) == 0)
            return TRUE;
        if (strstr(methods, "keyboard-interactive")) {
            kbd_password = password;
            int r = libssh2_userauth_keyboard_interactive(session, user, kbd_callback);
            kbd_password = NULL;
            if (r == 0) return TRUE;
        }
    }
    return FALSE;
}

/* ── SshConn lifecycle ────────────────────────────────────────────── */

/* Try every address (IPv6 and IPv4) like ssh(1) does */
static int tcp_connect(const char *host, int port, gchar **err)
{
    struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    gchar port_str[16];
    g_snprintf(port_str, sizeof(port_str), "%d", port);
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        *err = g_strdup_printf("Failed to resolve host %s: %s", host, gai_strerror(gai));
        return -1;
    }
    int sock = -1, last_errno = 0;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        sock = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (sock < 0) { last_errno = errno; continue; }
        if (connect(sock, ai->ai_addr, ai->ai_addrlen) == 0) break;
        last_errno = errno;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    if (sock < 0)
        *err = g_strdup_printf("Failed to connect to %s:%d: %s", host, port,
                               g_strerror(last_errno));
    return sock;
}

static SshConn *ssh_conn_new(const char *host, const char *user,
                              int port, const char *password,
                              const char *key_path,
                              gchar **out_error)
{
    *out_error = NULL;

    int sock = tcp_connect(host, port, out_error);
    if (sock < 0) return NULL;

    LIBSSH2_SESSION *session = libssh2_session_init();
    if (!session) {
        close(sock);
        *out_error = g_strdup("Error: libssh2_session_init failed");
        return NULL;
    }

    libssh2_session_set_blocking(session, 1);
    /* never hang forever on a dead link (ms) */
    libssh2_session_set_timeout(session, 60000);

    if (libssh2_session_handshake(session, sock) != 0) {
        char *msg = NULL;
        libssh2_session_last_error(session, &msg, NULL, 0);
        *out_error = g_strdup_printf("SSH handshake failed: %s", msg ? msg : "?");
        libssh2_session_free(session);
        close(sock);
        return NULL;
    }

    if (!verify_host_key(session, host, port, out_error)) {
        libssh2_session_disconnect(session, "Host key rejected");
        libssh2_session_free(session);
        close(sock);
        return NULL;
    }

    if (!authenticate(session, user, password, key_path)) {
        libssh2_session_disconnect(session, "Auth failed");
        libssh2_session_free(session);
        close(sock);
        *out_error = g_strdup("Authentication failed (agent, keys, password)");
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
    g_rec_mutex_init(&conn->lock);
    conn->sock        = sock;
    conn->session     = session;
    conn->sftp        = sftp;
    conn->host        = g_strdup(host);
    conn->user        = g_strdup(user);
    conn->remote_path = g_strdup("/");
    conn->port        = port;
    return conn;
}

/* Jobs hold the lock for their whole run, the background SFTP poll only
 * for one directory listing – so wait up to 500 ms before giving up. */
gboolean ssh_conn_trylock(SshConn *conn)
{
    if (!conn) return FALSE;
    for (int i = 0; i < 50; i++) {
        if (g_rec_mutex_trylock(&conn->lock)) return TRUE;
        g_usleep(10 * 1000);
    }
    return FALSE;
}

void ssh_conn_lock(SshConn *conn)
{
    if (conn) g_rec_mutex_lock(&conn->lock);
}

void ssh_conn_unlock(SshConn *conn)
{
    if (conn) g_rec_mutex_unlock(&conn->lock);
}

/* Per-panel async connect state (idx 0/1): a connect result is attached
 * only if its generation is still current, i.e. no disconnect happened
 * while it was in flight. */
static guint    connect_gen[2];
static gboolean connect_pending[2];

static void ssh_conn_free(SshConn *conn)
{
    /* wait for a running job (cannot normally happen: jobs are modal) */
    g_rec_mutex_lock(&conn->lock);
    libssh2_sftp_shutdown(conn->sftp);
    libssh2_session_disconnect(conn->session, "Closing");
    libssh2_session_free(conn->session);
    close(conn->sock);
    g_rec_mutex_unlock(&conn->lock);
    g_rec_mutex_clear(&conn->lock);

    g_free(conn->host);
    g_free(conn->user);
    g_free(conn->remote_path);
    g_free(conn);
}

void ssh_panel_close(Panel *p)
{
    panel_monitor_stop(p);

    /* invalidate a connect still in flight for this panel */
    connect_gen[p->idx]++;
    connect_pending[p->idx] = FALSE;

    SshConn *conn = p->ssh_conn;
    if (!conn) return;
    ssh_conn_free(conn);
    p->ssh_conn = NULL;
    gtk_widget_set_visible(p->disconnect_btn, FALSE);

    panel_load(p, g_get_home_dir());
}

/* ── ssh_read_file ────────────────────────────────────────────────── */

gchar *ssh_read_file(SshConn *conn, const char *path, gsize *out_len)
{
    *out_len = 0;
    if (!conn || !conn->sftp) return NULL;
    if (!ssh_conn_trylock(conn)) return NULL;       /* a job is using it */

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(conn->sftp, path,
        LIBSSH2_FXF_READ, 0);
    if (!fh) { ssh_conn_unlock(conn); return NULL; }

    GByteArray *ba = g_byte_array_new();
    char buf[32768];
    ssize_t n;
    /* session is blocking: no EAGAIN, any negative value is an error */
    while ((n = libssh2_sftp_read(fh, buf, sizeof(buf))) > 0)
        g_byte_array_append(ba, (guint8 *)buf, (guint)n);
    libssh2_sftp_close(fh);
    ssh_conn_unlock(conn);
    if (n < 0) {                        /* never return a truncated file */
        g_byte_array_free(ba, TRUE);
        return NULL;
    }

    *out_len = ba->len;
    g_byte_array_append(ba, (guint8 *)"\0", 1);
    return (gchar *)g_byte_array_free(ba, FALSE);
}

/* ── ssh_write_file ───────────────────────────────────────────────── */

/* Write to a temp file in the same directory, then rename it over the
 * target: a failed upload never leaves a truncated remote file. */
gboolean ssh_write_file(SshConn *conn, const char *path,
                        const gchar *data, gsize len)
{
    if (!conn || !conn->sftp) return FALSE;
    if (!ssh_conn_trylock(conn)) return FALSE;      /* a job is using it */

    /* follow a symlink so the link itself survives the rename */
    char real[4096];
    int rl = libssh2_sftp_realpath(conn->sftp, path, real, sizeof(real) - 1);
    gchar *target = (rl > 0) ? g_strndup(real, (gsize)rl) : g_strdup(path);

    /* keep the original permissions */
    LIBSSH2_SFTP_ATTRIBUTES st;
    long mode = LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR |
                LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IROTH;
    gboolean have_mode = libssh2_sftp_stat(conn->sftp, target, &st) == 0 &&
                         (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS);
    if (have_mode) mode = (long)(st.permissions & 07777);

    gchar *dir  = g_path_get_dirname(target);
    gchar *base = g_path_get_basename(target);
    gchar *tmp  = g_strdup_printf("%s/.%s.fm-%08x", strcmp(dir, "/") ? dir : "",
                                  base, g_random_int());
    g_free(dir);
    g_free(base);

    LIBSSH2_SFTP_HANDLE *fh = libssh2_sftp_open(conn->sftp, tmp,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL,
        (mode & 0777) | LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR);
    gboolean ok = fh != NULL;

    gsize written = 0;
    while (ok && written < len) {
        ssize_t r = libssh2_sftp_write(fh, data + written, len - written);
        if (r <= 0) ok = FALSE;
        else written += (gsize)r;
    }
    if (fh && libssh2_sftp_close(fh) != 0) ok = FALSE;

    if (ok && have_mode) {
        LIBSSH2_SFTP_ATTRIBUTES a = { 0 };
        a.flags       = LIBSSH2_SFTP_ATTR_PERMISSIONS;
        a.permissions = (unsigned long)mode;
        libssh2_sftp_setstat(conn->sftp, tmp, &a);  /* best effort (umask) */
    }
    if (ok && libssh2_sftp_rename_ex(conn->sftp, tmp, (unsigned)strlen(tmp),
                                     target, (unsigned)strlen(target),
                                     LIBSSH2_SFTP_RENAME_OVERWRITE |
                                     LIBSSH2_SFTP_RENAME_ATOMIC |
                                     LIBSSH2_SFTP_RENAME_NATIVE) != 0) {
        /* SFTPv3 servers refuse to rename over an existing file */
        libssh2_sftp_unlink(conn->sftp, target);
        ok = libssh2_sftp_rename(conn->sftp, tmp, target) == 0;
        if (!ok) fh = NULL;     /* original gone: keep the temp copy */
    }
    if (!ok && fh) libssh2_sftp_unlink(conn->sftp, tmp);

    g_free(tmp);
    g_free(target);
    ssh_conn_unlock(conn);
    return ok;
}

/* ── panel_load_remote ────────────────────────────────────────────── */

/* remote_icon removed — using shared icon_for_entry() from main.c */

void panel_load_remote(Panel *p, const char *path)
{
    SshConn *conn = p->ssh_conn;
    if (!conn) return;

    arc_panel_leave(p);                 /* SFTP opened over an archive view */

    /* Leaving local search results (SFTP opened in that panel) */
    if (p->search_mode) {
        search_detach(p);
        gtk_sort_list_model_set_sorter(p->sort_model, p->default_sorter);
        search_update_buttons(p);
    }

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

    /* the background poll may be using the session */
    ssh_conn_lock(conn);
    LIBSSH2_SFTP_HANDLE *handle = libssh2_sftp_opendir(conn->sftp, path);
    if (!handle) {
        ssh_conn_unlock(conn);
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

    char name_buf[4096];
    char longentry[4096];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc;

    while ((rc = libssh2_sftp_readdir_ex(handle, name_buf, sizeof(name_buf),
                                         longentry, sizeof(longentry), &attrs)) > 0) {
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
    ssh_conn_unlock(conn);
    if (rc < 0)                         /* 0 = end of listing */
        fm_status(p->fm, "SFTP: listing of %s incomplete (error %d)", path, rc);

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
    g_object_unref(entry);   /* ref taken when the dialog was opened */
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
                          on_file_browse_done, g_object_ref(entry));
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
    guint  gen;     /* connect_gen[panel->idx] when started */
} SshConnectCtx;

static void ssh_connect_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    (void)source;
    SshConnectCtx *ctx = user_data;
    GError *err = NULL;
    SshConn *conn = g_task_propagate_pointer(G_TASK(res), &err);
    int idx = ctx->panel->idx;

    if (ctx->gen != connect_gen[idx]) {
        /* panel was disconnected while connecting: drop the result */
        if (conn) ssh_conn_free(conn);
        g_clear_error(&err);
        g_free(ctx->user);
        g_free(ctx);
        return;
    }
    connect_pending[idx] = FALSE;

    if (conn) {
        ctx->panel->ssh_conn = conn;
        /* start in the remote home directory (realpath of ".") */
        char home[4096];
        int hl = libssh2_sftp_realpath(conn->sftp, ".", home, sizeof(home) - 1);
        gchar *start = (hl > 0) ? g_strndup(home, (gsize)hl) : g_strdup("/");
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
    panel_focus(fp);

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
    gtk_entry_set_placeholder_text(GTK_ENTRY(d->key_entry), "default: ssh-agent, ~/.ssh/id_ed25519, id_ecdsa, id_rsa");
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
        "<small>Keys are tried first (ssh-agent, then key files); the password is also used as the key passphrase. "
        "Unknown hosts are verified against ~/.ssh/known_hosts.</small>");
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
        if (host && host[0] && connect_pending[active_panel(fm)->idx]) {
            fm_status(fm, "SSH: a connection is already in progress for this panel");
        } else if (host && host[0]) {
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
            cctx->gen   = ++connect_gen[p->idx];
            connect_pending[p->idx] = TRUE;

            GTask *task = g_task_new(NULL, NULL, ssh_connect_done, cctx);
            g_task_set_task_data(task, args, ssh_connect_args_free);
            g_task_run_in_thread(task, ssh_connect_thread);
            g_object_unref(task);
        }
    }
    /* drop the password from the entry buffer */
    gtk_editable_set_text(GTK_EDITABLE(pw_entry), "");

    gtk_window_destroy(GTK_WINDOW(dlg));
    g_list_free_full(d->bookmarks, ssh_bookmark_free);
    g_free(d);
}

#else  /* !HAVE_LIBSSH2 */

gboolean ssh_conn_trylock(SshConn *conn) { (void)conn; return TRUE; }
void     ssh_conn_lock   (SshConn *conn) { (void)conn; }
void     ssh_conn_unlock (SshConn *conn) { (void)conn; }

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
