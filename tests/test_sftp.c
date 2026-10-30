/* ─────────────────────────────────────────────────────────────────── *
 *  SFTP integration tests for the fsops engine.
 *
 *  Needs a running sshd – started by tests/sftp_test.sh, which passes
 *  FM_SFTP_PORT, FM_SFTP_KEY (private key, .pub next to it) and
 *  FM_SFTP_DIR (scratch directory, served on the same machine, so the
 *  "remote" side can be inspected with plain local calls).
 * ─────────────────────────────────────────────────────────────────── */

#include "fsops.h"
#include "util.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <glib/gstdio.h>

static LIBSSH2_SESSION *session;
static LIBSSH2_SFTP    *sftp;
static const char      *work;
static int              sock = -1;

static FsAnswer answer_overwrite(FsCtx *c, const char *src, const char *dst)
{
    (void)c; (void)src; (void)dst;
    return FS_ANS_OVERWRITE;
}

static FsAnswer answer_skip(FsCtx *c, const char *src, const char *dst)
{
    (void)c; (void)src; (void)dst;
    return FS_ANS_SKIP;
}

/* fresh local/ and remote/ dirs for each test */
static void fresh(gchar **l, gchar **r)
{
    static int n;
    n++;
    *l = g_strdup_printf("%s/t%d/local",  work, n);
    *r = g_strdup_printf("%s/t%d/remote", work, n);
    g_mkdir_with_parents(*l, 0755);
    g_mkdir_with_parents(*r, 0755);
}

static gchar *slurp(const char *path)
{
    gchar *s = NULL;
    g_file_get_contents(path, &s, NULL, NULL);
    return s;
}

static int count_temps(const char *dir)
{
    int n = 0;
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *e;
    while (d && (e = g_dir_read_name(d)))
        if (strstr(e, ".fm-")) n++;
    if (d) g_dir_close(d);
    return n;
}

static void put(const char *dir, const char *name, const char *text)
{
    gchar *p = g_build_filename(dir, name, NULL);
    g_assert_true(g_file_set_contents(p, text, -1, NULL));
    g_free(p);
}

static void assert_content(const char *dir, const char *name, const char *text)
{
    gchar *p = g_build_filename(dir, name, NULL);
    gchar *s = slurp(p);
    g_assert_cmpstr(s, ==, text);
    g_free(s);
    g_free(p);
}

/* ── tests ────────────────────────────────────────────────────────── */

static void test_upload_overwrite(void)
{
    gchar *l, *r; fresh(&l, &r);
    put(l, "a.txt", "NEW");
    put(r, "a.txt", "OLD");
    gchar *src = g_build_filename(l, "a.txt", NULL), *dst = g_build_filename(r, "a.txt", NULL);
    g_chmod(dst, 0750);

    FsCtx c = { .conflict = answer_overwrite };
    g_assert_cmpint(fs_sftp_upload(&c, sftp, src, dst), ==, FS_OK);
    assert_content(r, "a.txt", "NEW");
    g_assert_cmpint(count_temps(r), ==, 0);
    fs_ctx_clear(&c);
    g_free(src); g_free(dst); g_free(l); g_free(r);
}

static void test_upload_skip_keeps_old(void)
{
    gchar *l, *r; fresh(&l, &r);
    put(l, "a.txt", "NEW");
    put(r, "a.txt", "OLD");
    gchar *src = g_build_filename(l, "a.txt", NULL), *dst = g_build_filename(r, "a.txt", NULL);

    FsCtx c = { .conflict = answer_skip };
    g_assert_cmpint(fs_sftp_upload(&c, sftp, src, dst), ==, FS_SKIPPED);
    assert_content(r, "a.txt", "OLD");
    fs_ctx_clear(&c);
    g_free(src); g_free(dst); g_free(l); g_free(r);
}

static void test_download_overwrite(void)
{
    gchar *l, *r; fresh(&l, &r);
    put(r, "a.txt", "REMOTE");
    put(l, "a.txt", "LOCAL");
    gchar *src = g_build_filename(r, "a.txt", NULL), *dst = g_build_filename(l, "a.txt", NULL);

    FsCtx c = { .conflict = answer_overwrite };
    g_assert_cmpint(fs_sftp_download(&c, sftp, src, dst), ==, FS_OK);
    assert_content(l, "a.txt", "REMOTE");
    g_assert_cmpint(count_temps(l), ==, 0);
    fs_ctx_clear(&c);
    g_free(src); g_free(dst); g_free(l); g_free(r);
}

/* a symlink must be recreated, never followed (it could point anywhere
 * on the local machine) – also checks the SSH_FXP_SYMLINK arg order */
static void test_symlink_roundtrip(void)
{
    gchar *l, *r; fresh(&l, &r);
    gchar *ll = g_build_filename(l, "link", NULL), *rl = g_build_filename(r, "link", NULL);
    gchar *back = g_build_filename(l, "back", NULL);
    g_assert_cmpint(symlink("/nonexistent/target", ll), ==, 0);

    FsCtx c = { .conflict = answer_overwrite };
    g_assert_cmpint(fs_sftp_upload(&c, sftp, ll, rl), ==, FS_OK);
    gchar *t = g_file_read_link(rl, NULL);
    g_assert_cmpstr(t, ==, "/nonexistent/target");
    g_free(t);

    /* overwrite an existing link */
    g_assert_cmpint(fs_sftp_upload(&c, sftp, ll, rl), ==, FS_OK);
    g_assert_cmpint(count_temps(r), ==, 0);

    g_assert_cmpint(fs_sftp_download(&c, sftp, rl, back), ==, FS_OK);
    t = g_file_read_link(back, NULL);
    g_assert_cmpstr(t, ==, "/nonexistent/target");
    g_free(t);

    fs_ctx_clear(&c);
    g_free(ll); g_free(rl); g_free(back); g_free(l); g_free(r);
}

static void test_tree_download_and_delete(void)
{
    gchar *l, *r; fresh(&l, &r);
    gchar *sub = g_build_filename(r, "tree/sub", NULL);
    g_mkdir_with_parents(sub, 0755);
    put(sub, "x.txt", "X");
    gchar *rt = g_build_filename(r, "tree", NULL), *lt = g_build_filename(l, "tree", NULL);

    FsCtx c = { .conflict = answer_overwrite };
    g_assert_cmpint(fs_sftp_download(&c, sftp, rt, lt), ==, FS_OK);
    gchar *lsub = g_build_filename(lt, "sub", NULL);
    assert_content(lsub, "x.txt", "X");

    g_assert_cmpint(fs_sftp_calc_size(&c, sftp, rt), ==, 1);
    g_assert_cmpint(fs_sftp_delete(&c, sftp, rt), ==, FS_OK);
    g_assert_false(g_file_test(rt, G_FILE_TEST_EXISTS));

    fs_ctx_clear(&c);
    g_free(sub); g_free(rt); g_free(lt); g_free(lsub); g_free(l); g_free(r);
}

static void test_tree_upload(void)
{
    gchar *l, *r; fresh(&l, &r);
    gchar *sub = g_build_filename(l, "tree/sub", NULL);
    g_mkdir_with_parents(sub, 0755);
    put(sub, "y.txt", "Y");
    gchar *lt = g_build_filename(l, "tree", NULL), *rt = g_build_filename(r, "tree", NULL);

    FsCtx c = { .conflict = answer_overwrite };
    g_assert_cmpint(fs_sftp_upload(&c, sftp, lt, rt), ==, FS_OK);
    gchar *rsub = g_build_filename(rt, "sub", NULL);
    assert_content(rsub, "y.txt", "Y");

    gboolean is_dir = FALSE;
    g_assert_true(fs_sftp_exists(sftp, rt, &is_dir));
    g_assert_true(is_dir);

    fs_ctx_clear(&c);
    g_free(sub); g_free(lt); g_free(rt); g_free(rsub); g_free(l); g_free(r);
}

static void test_replace(void)
{
    gchar *l, *r; fresh(&l, &r);
    put(r, "one", "ONE");
    put(r, "two", "TWO");
    gchar *a = g_build_filename(r, "one", NULL), *b = g_build_filename(r, "two", NULL);
    g_assert_true(fs_sftp_replace(sftp, a, b));
    assert_content(r, "two", "ONE");
    g_assert_false(g_file_test(a, G_FILE_TEST_EXISTS));
    g_free(a); g_free(b); g_free(l); g_free(r);
}

/* ── setup ────────────────────────────────────────────────────────── */

static gboolean connect_sftp(void)
{
    const char *port = g_getenv("FM_SFTP_PORT");
    const char *key  = g_getenv("FM_SFTP_KEY");
    work = g_getenv("FM_SFTP_DIR");
    if (!port || !key || !work) return FALSE;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((guint16)atoi(port)) };
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) != 0) return FALSE;

    libssh2_init(0);
    session = libssh2_session_init();
    libssh2_session_set_blocking(session, 1);
    if (libssh2_session_handshake(session, sock) != 0) return FALSE;
    gchar *pub = g_strconcat(key, ".pub", NULL);
    int rc = libssh2_userauth_publickey_fromfile(session, g_get_user_name(), pub, key, NULL);
    g_free(pub);
    if (rc != 0) return FALSE;
    sftp = libssh2_sftp_init(session);
    return sftp != NULL;
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!connect_sftp()) {
        g_printerr("test_sftp: no SFTP server – run via tests/sftp_test.sh\n");
        return 77;   /* "skipped" for test harnesses */
    }

    g_test_add_func("/sftp/upload-overwrite",      test_upload_overwrite);
    g_test_add_func("/sftp/upload-skip-keeps-old", test_upload_skip_keeps_old);
    g_test_add_func("/sftp/download-overwrite",    test_download_overwrite);
    g_test_add_func("/sftp/symlink-roundtrip",     test_symlink_roundtrip);
    g_test_add_func("/sftp/tree-download-delete",  test_tree_download_and_delete);
    g_test_add_func("/sftp/tree-upload",           test_tree_upload);
    g_test_add_func("/sftp/replace",               test_replace);
    int rc = g_test_run();

    libssh2_sftp_shutdown(sftp);
    libssh2_session_disconnect(session, "done");
    libssh2_session_free(session);
    libssh2_exit();
    close(sock);
    return rc;
}
