/* Unit tests for the GLib-only core: util.c + fsops.c
 * Run: make test   (or make test-asan for ASan/UBSan) */

#include "util.h"
#include "fsops.h"
#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ── helpers ─────────────────────────────────────────────────────── */

static gchar *tmp_root(void)
{
    gchar *d = g_dir_make_tmp("fm-test-XXXXXX", NULL);
    g_assert_nonnull(d);
    return d;
}

static void write_file(const char *path, const char *data)
{
    g_assert_true(g_file_set_contents(path, data, -1, NULL));
}

static gchar *read_file(const char *path)
{
    gchar *d = NULL;
    g_assert_true(g_file_get_contents(path, &d, NULL, NULL));
    return d;
}

static void rm_rf(const char *path)
{
    FsCtx c = { 0 };
    fs_delete(&c, path);
    fs_ctx_clear(&c);
}

static gchar *J(const char *a, const char *b) { return g_build_filename(a, b, NULL); }

/* ── util ────────────────────────────────────────────────────────── */

static void test_name_safety(void)
{
    g_assert_true(fs_name_is_safe("a.txt"));
    g_assert_true(fs_name_is_safe(".hidden"));
    g_assert_true(fs_name_is_safe("..."));
    g_assert_false(fs_name_is_safe(""));
    g_assert_false(fs_name_is_safe(NULL));
    g_assert_false(fs_name_is_safe("."));
    g_assert_false(fs_name_is_safe(".."));
    g_assert_false(fs_name_is_safe("a/b"));

    g_assert_true(fs_relpath_is_safe("a"));
    g_assert_true(fs_relpath_is_safe("a/b/c"));
    g_assert_true(fs_relpath_is_safe("a/b/"));
    g_assert_false(fs_relpath_is_safe("/etc"));
    g_assert_false(fs_relpath_is_safe("a/../b"));
    g_assert_false(fs_relpath_is_safe(".."));
    g_assert_false(fs_relpath_is_safe(""));
    g_assert_false(fs_relpath_is_safe("//"));
}

static void test_path_inside(void)
{
    g_assert_true(fs_path_is_inside("/a/b", "/a"));
    g_assert_true(fs_path_is_inside("/a", "/a"));
    g_assert_true(fs_path_is_inside("/a/b", "/a/"));
    g_assert_true(fs_path_is_inside("/x", "/"));
    g_assert_false(fs_path_is_inside("/ab", "/a"));
    g_assert_false(fs_path_is_inside("/b", "/a"));
}

static void test_remote_join(void)
{
    gchar *a = fs_remote_join("/", "x");
    gchar *b = fs_remote_join("/home/u", "x");
    gchar *c = fs_remote_join("/home/u/", "x");
    g_assert_cmpstr(a, ==, "/x");
    g_assert_cmpstr(b, ==, "/home/u/x");
    g_assert_cmpstr(c, ==, "/home/u/x");
    g_free(a); g_free(b); g_free(c);
}

static void test_fmt_size(void)
{
    gchar *s;
    s = fmt_size(0);              g_assert_cmpstr(s, ==, "0 B");    g_free(s);
    s = fmt_size(1023);           g_assert_cmpstr(s, ==, "1023 B"); g_free(s);
    s = fmt_size(1024);           g_assert_cmpstr(s, ==, "1.0 KB"); g_free(s);
    s = fmt_size(5 * 1024 * 1024);g_assert_cmpstr(s, ==, "5.0 MB"); g_free(s);
    s = fmt_size(-1);             g_assert_cmpstr(s, ==, "");       g_free(s);
}

static void test_arc(void)
{
    g_assert_cmpint(arc_detect("a.tar.gz"),  ==, ARC_TAR_GZ);
    g_assert_cmpint(arc_detect("A.TAR.GZ"),  ==, ARC_TAR_GZ);
    g_assert_cmpint(arc_detect("a.tgz"),     ==, ARC_TAR_GZ);
    g_assert_cmpint(arc_detect("a.tar.zst"), ==, ARC_TAR_ZST);
    g_assert_cmpint(arc_detect("a.zip"),     ==, ARC_ZIP);
    g_assert_cmpint(arc_detect("a.7z"),      ==, ARC_7Z);
    g_assert_cmpint(arc_detect("a.rar"),     ==, ARC_RAR);
    g_assert_cmpint(arc_detect("a.txt"),     ==, ARC_UNKNOWN);
    g_assert_cmpint(arc_detect("noext"),     ==, ARC_UNKNOWN);
    g_assert_cmpint(arc_detect(NULL),        ==, ARC_UNKNOWN);

    gchar *n = arc_with_extension("proj.tar.gz", ARC_ZIP);
    g_assert_cmpstr(n, ==, "proj.zip");
    g_free(n);
    n = arc_with_extension("proj", ARC_TAR_XZ);
    g_assert_cmpstr(n, ==, "proj.tar.xz");
    g_free(n);
}

static void test_unique_name(void)
{
    gchar *root = tmp_root();
    gchar *p;
    gchar *u = fs_unique_name(root, "a.txt", fs_local_exists, NULL);
    g_assert_cmpstr(u, ==, "a.txt");
    g_free(u);

    p = J(root, "a.txt");        write_file(p, "x"); g_free(p);
    u = fs_unique_name(root, "a.txt", fs_local_exists, NULL);
    g_assert_cmpstr(u, ==, "a (copy).txt");
    g_free(u);

    p = J(root, "a (copy).txt"); write_file(p, "x"); g_free(p);
    u = fs_unique_name(root, "a.txt", fs_local_exists, NULL);
    g_assert_cmpstr(u, ==, "a (copy 2).txt");
    g_free(u);

    p = J(root, "b.tar.gz");     write_file(p, "x"); g_free(p);
    u = fs_unique_name(root, "b.tar.gz", fs_local_exists, NULL);
    g_assert_cmpstr(u, ==, "b (copy).tar.gz");
    g_free(u);

    p = J(root, ".bashrc");      write_file(p, "x"); g_free(p);
    u = fs_unique_name(root, ".bashrc", fs_local_exists, NULL);
    g_assert_cmpstr(u, ==, ".bashrc (copy)");
    g_free(u);

    rm_rf(root);
    g_free(root);
}

static void test_uri_list(void)
{
    gboolean cut = FALSE;
    const char *gnome = "cut\nfile:///tmp/a%20b\nfile:///etc/passwd\n";
    GList *l = fs_parse_uri_list(gnome, strlen(gnome), &cut);
    g_assert_true(cut);
    g_assert_cmpuint(g_list_length(l), ==, 2);
    g_assert_cmpstr(l->data, ==, "/tmp/a b");
    g_list_free_full(l, g_free);

    const char *uris = "# comment\r\nfile:///x\r\nhttp://example.com/y\r\n";
    l = fs_parse_uri_list(uris, strlen(uris), &cut);
    g_assert_false(cut);
    g_assert_cmpuint(g_list_length(l), ==, 1);
    g_assert_cmpstr(l->data, ==, "/x");
    g_list_free_full(l, g_free);
}

static void test_patterns(void)
{
    g_assert_true(fs_match_patterns("*.c", "main.c"));
    g_assert_true(fs_match_patterns("*.C", "main.c"));      /* case-insensitive */
    g_assert_true(fs_match_patterns("*.h; *.c", "main.c"));
    g_assert_true(fs_match_patterns("*.*", "Makefile"));    /* TC semantics */
    g_assert_false(fs_match_patterns("*.h", "main.c"));
    g_assert_false(fs_match_patterns("", "main.c"));
}

/* ── fsops: copy / move / delete ─────────────────────────────────── */

static void test_copy_tree(void)
{
    gchar *root = tmp_root();
    gchar *src = J(root, "src"), *dst = J(root, "dst");
    g_mkdir(src, 0755);
    gchar *sub = J(src, "sub");
    g_mkdir(sub, 0750);
    gchar *f1 = J(src, "f1.txt"), *f2 = J(sub, "f2.txt");
    write_file(f1, "hello");
    write_file(f2, "world");
    chmod(f1, 0640);
    struct timespec ts[2] = { { 1000000000, 0 }, { 1000000000, 0 } };
    utimensat(AT_FDCWD, f1, ts, 0);
    gchar *ln = J(src, "link");
    g_assert_cmpint(symlink("f1.txt", ln), ==, 0);

    FsCtx c = { 0 };
    g_assert_cmpint(fs_calc_size(&c, src), >=, 10);
    g_assert_cmpint(fs_copy(&c, src, dst), ==, FS_OK);
    g_assert_null(c.error);
    g_assert_cmpint(c.files_done, ==, 3);

    gchar *d1 = J(dst, "f1.txt");
    gchar *d2 = g_build_filename(dst, "sub", "f2.txt", NULL);
    gchar *dl = J(dst, "link");
    gchar *t = read_file(d1); g_assert_cmpstr(t, ==, "hello"); g_free(t);
    t = read_file(d2);        g_assert_cmpstr(t, ==, "world"); g_free(t);

    struct stat st;
    g_assert_cmpint(stat(d1, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0640);          /* mode kept  */
    g_assert_cmpint(st.st_mtime, ==, 1000000000);          /* mtime kept */
    g_assert_cmpint(lstat(dl, &st), ==, 0);
    g_assert_true(S_ISLNK(st.st_mode));                     /* link kept  */
    gchar *tgt = g_file_read_link(dl, NULL);
    g_assert_cmpstr(tgt, ==, "f1.txt");
    g_free(tgt);

    fs_ctx_clear(&c);
    rm_rf(root);
    g_free(d1); g_free(d2); g_free(dl); g_free(ln);
    g_free(f1); g_free(f2); g_free(sub); g_free(src); g_free(dst); g_free(root);
}

static void test_copy_into_itself(void)
{
    gchar *root = tmp_root();
    gchar *src = J(root, "d");
    g_mkdir(src, 0755);
    gchar *dst = J(src, "d");

    FsCtx c = { 0 };
    g_assert_cmpint(fs_copy(&c, src, dst), ==, FS_ERR);
    g_assert_nonnull(c.error);
    g_assert_false(g_file_test(dst, G_FILE_TEST_EXISTS));
    fs_ctx_clear(&c);

    g_assert_cmpint(fs_move(&c, src, dst), ==, FS_ERR);
    fs_ctx_clear(&c);

    rm_rf(root);
    g_free(src); g_free(dst); g_free(root);
}

static void test_copy_same_file(void)
{
    gchar *root = tmp_root();
    gchar *f = J(root, "f");
    write_file(f, "data");
    FsCtx c = { .policy = FS_OVERWRITE_ALL };
    g_assert_cmpint(fs_copy(&c, f, f), ==, FS_ERR);       /* never truncate */
    gchar *t = read_file(f);
    g_assert_cmpstr(t, ==, "data");
    g_free(t);
    fs_ctx_clear(&c);
    rm_rf(root);
    g_free(f); g_free(root);
}

static FsAnswer answer_skip(FsCtx *c, const char *s, const char *d)
{
    (void)c; (void)s; (void)d;
    return FS_ANS_SKIP;
}

static int conflicts_seen;
static FsAnswer answer_overwrite_all(FsCtx *c, const char *s, const char *d)
{
    (void)c; (void)s; (void)d;
    conflicts_seen++;
    return FS_ANS_OVERWRITE_ALL;
}

static void test_conflicts(void)
{
    gchar *root = tmp_root();
    gchar *a = J(root, "a"), *b = J(root, "b");
    g_mkdir(a, 0755); g_mkdir(b, 0755);
    gchar *a1 = J(a, "1"), *a2 = J(a, "2"), *b1 = J(b, "1"), *b2 = J(b, "2");
    write_file(a1, "new1"); write_file(a2, "new2");
    write_file(b1, "old1"); write_file(b2, "old2");

    /* skip keeps the old contents */
    FsCtx c = { .conflict = answer_skip };
    g_assert_cmpint(fs_copy(&c, a, b), ==, FS_SKIPPED);
    gchar *t = read_file(b1); g_assert_cmpstr(t, ==, "old1"); g_free(t);
    fs_ctx_clear(&c);

    /* no callback + FS_ASK → skip (never overwrite silently) */
    FsCtx q = { 0 };
    g_assert_cmpint(fs_copy(&q, a1, b1), ==, FS_SKIPPED);
    fs_ctx_clear(&q);

    /* overwrite all is asked once, then applies to the rest */
    conflicts_seen = 0;
    FsCtx o = { .conflict = answer_overwrite_all };
    g_assert_cmpint(fs_copy(&o, a, b), ==, FS_OK);
    g_assert_cmpint(conflicts_seen, ==, 1);
    t = read_file(b1); g_assert_cmpstr(t, ==, "new1"); g_free(t);
    t = read_file(b2); g_assert_cmpstr(t, ==, "new2"); g_free(t);
    fs_ctx_clear(&o);

    /* overwrite never writes through a symlink at the destination */
    gchar *victim = J(root, "victim");
    write_file(victim, "keep");
    g_unlink(b1);
    g_assert_cmpint(symlink(victim, b1), ==, 0);
    FsCtx w = { .policy = FS_OVERWRITE_ALL };
    g_assert_cmpint(fs_copy(&w, a1, b1), ==, FS_OK);
    t = read_file(victim); g_assert_cmpstr(t, ==, "keep"); g_free(t);
    fs_ctx_clear(&w);

    rm_rf(root);
    g_free(victim);
    g_free(a1); g_free(a2); g_free(b1); g_free(b2); g_free(a); g_free(b); g_free(root);
}

static void test_move_merge(void)
{
    gchar *root = tmp_root();
    gchar *a = J(root, "a"), *b = J(root, "b");
    g_mkdir(a, 0755); g_mkdir(b, 0755);
    gchar *a1 = J(a, "1"), *b2 = J(b, "2");
    write_file(a1, "one"); write_file(b2, "two");
    gchar *target = J(b, "a");
    g_mkdir(target, 0755);                               /* b/a exists */
    gchar *keep = J(target, "keep");
    write_file(keep, "k");

    FsCtx c = { 0 };
    g_assert_cmpint(fs_move(&c, a, target), ==, FS_OK);  /* merge into b/a */
    g_assert_false(g_file_test(a, G_FILE_TEST_EXISTS));
    gchar *m1 = J(target, "1");
    gchar *t = read_file(m1); g_assert_cmpstr(t, ==, "one"); g_free(t);
    g_assert_true(g_file_test(keep, G_FILE_TEST_EXISTS));
    fs_ctx_clear(&c);

    rm_rf(root);
    g_free(m1); g_free(keep); g_free(target);
    g_free(a1); g_free(b2); g_free(a); g_free(b); g_free(root);
}

static void test_move_skip_keeps_source(void)
{
    gchar *root = tmp_root();
    gchar *s = J(root, "s"), *d = J(root, "d");
    write_file(s, "src"); write_file(d, "dst");
    FsCtx c = { .policy = FS_SKIP_ALL };
    g_assert_cmpint(fs_move(&c, s, d), ==, FS_SKIPPED);
    g_assert_true(g_file_test(s, G_FILE_TEST_EXISTS));
    gchar *t = read_file(d); g_assert_cmpstr(t, ==, "dst"); g_free(t);
    fs_ctx_clear(&c);
    rm_rf(root);
    g_free(s); g_free(d); g_free(root);
}

static void cancel_on_progress(FsCtx *c, const char *name, gint64 bytes)
{
    (void)name;
    if (bytes > 0) fs_cancel(c);
}

static void test_cancel_no_partial(void)
{
    gchar *root = tmp_root();
    gchar *s = J(root, "big"), *d = J(root, "copy");
    gsize n = 2 * 1024 * 1024;                             /* > one buffer */
    gchar *buf = g_malloc0(n);
    g_assert_true(g_file_set_contents(s, buf, (gssize)n, NULL));
    g_free(buf);

    FsCtx c = { .progress = cancel_on_progress };
    g_assert_cmpint(fs_copy(&c, s, d), ==, FS_CANCELLED);
    g_assert_false(g_file_test(d, G_FILE_TEST_EXISTS));    /* cleaned up */
    fs_ctx_clear(&c);
    rm_rf(root);
    g_free(s); g_free(d); g_free(root);
}

static int dir_entries(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    g_assert_nonnull(d);
    int n = 0;
    while (g_dir_read_name(d)) n++;
    g_dir_close(d);
    return n;
}

/* Overwriting a directory that contains the source must be refused –
 * deleting it first would destroy the source (move /x/foo/foo → /x) */
static void test_overwrite_ancestor(void)
{
    gchar *root = tmp_root();
    gchar *foo = J(root, "foo");
    g_mkdir(foo, 0755);
    gchar *inner = J(foo, "foo");
    write_file(inner, "inner");

    FsCtx c = { .policy = FS_OVERWRITE_ALL };
    g_assert_cmpint(fs_move(&c, inner, foo), ==, FS_ERR);
    gchar *t = read_file(inner); g_assert_cmpstr(t, ==, "inner"); g_free(t);
    fs_ctx_clear(&c);

    FsCtx k = { .policy = FS_OVERWRITE_ALL };
    g_assert_cmpint(fs_copy(&k, inner, foo), ==, FS_ERR);
    t = read_file(inner); g_assert_cmpstr(t, ==, "inner"); g_free(t);
    fs_ctx_clear(&k);

    rm_rf(root);
    g_free(inner); g_free(foo); g_free(root);
}

/* Overwrite goes through a temp file: correct result, no leftovers,
 * and a cancelled copy keeps the old file */
static void test_overwrite_safe(void)
{
    gchar *root = tmp_root();
    gchar *s = J(root, "s"), *d = J(root, "d");
    write_file(s, "new"); write_file(d, "old");

    FsCtx c = { .policy = FS_OVERWRITE_ALL };
    g_assert_cmpint(fs_copy(&c, s, d), ==, FS_OK);
    gchar *t = read_file(d); g_assert_cmpstr(t, ==, "new"); g_free(t);
    g_assert_cmpint(dir_entries(root), ==, 2);
    fs_ctx_clear(&c);

    gchar *big = J(root, "big");
    gsize n = 2 * 1024 * 1024;
    gchar *buf = g_malloc0(n);
    g_assert_true(g_file_set_contents(big, buf, (gssize)n, NULL));
    g_free(buf);
    write_file(d, "old");
    FsCtx x = { .policy = FS_OVERWRITE_ALL, .progress = cancel_on_progress };
    g_assert_cmpint(fs_copy(&x, big, d), ==, FS_CANCELLED);
    t = read_file(d); g_assert_cmpstr(t, ==, "old"); g_free(t);
    g_assert_cmpint(dir_entries(root), ==, 3);
    fs_ctx_clear(&x);

    rm_rf(root);
    g_free(big); g_free(s); g_free(d); g_free(root);
}

/* A move that could not read everything must not delete the source */
static void test_move_unreadable_keeps_source(void)
{
    if (getuid() == 0) { g_test_skip("root can read anything"); return; }
    gchar *root = tmp_root();
    gchar *a = J(root, "a"), *b = J(root, "b");
    gchar *as = J(a, "sub"), *bs = J(b, "sub");
    g_assert_cmpint(g_mkdir_with_parents(as, 0755), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(bs, 0755), ==, 0);
    gchar *f = J(as, "f");
    write_file(f, "data");
    g_assert_cmpint(chmod(as, 0), ==, 0);

    FsCtx c = { 0 };
    g_assert_cmpint(fs_move(&c, a, b), ==, FS_ERR);
    g_assert_cmpint(chmod(as, 0755), ==, 0);
    g_assert_true(g_file_test(f, G_FILE_TEST_EXISTS));
    fs_ctx_clear(&c);

    rm_rf(root);
    g_free(f); g_free(as); g_free(bs); g_free(a); g_free(b); g_free(root);
}

static void test_delete(void)
{
    gchar *root = tmp_root();
    gchar *d = J(root, "d");
    gchar *dd = g_build_filename(d, "x", "y", NULL);
    g_assert_cmpint(g_mkdir_with_parents(dd, 0755), ==, 0);
    gchar *f = J(dd, "f");
    write_file(f, "z");
    /* a symlink to outside must be removed, not followed */
    gchar *outside = J(root, "outside");
    write_file(outside, "safe");
    gchar *ln = J(d, "ln");
    g_assert_cmpint(symlink(root, ln), ==, 0);

    FsCtx c = { 0 };
    g_assert_cmpint(fs_delete(&c, d), ==, FS_OK);
    g_assert_false(g_file_test(d, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(outside, G_FILE_TEST_EXISTS));
    g_assert_cmpint(c.files_done, ==, 2);
    fs_ctx_clear(&c);

    rm_rf(root);
    g_free(outside); g_free(ln); g_free(f); g_free(dd); g_free(d); g_free(root);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/util/name-safety",       test_name_safety);
    g_test_add_func("/util/path-inside",       test_path_inside);
    g_test_add_func("/util/remote-join",       test_remote_join);
    g_test_add_func("/util/fmt-size",          test_fmt_size);
    g_test_add_func("/util/archive",           test_arc);
    g_test_add_func("/util/unique-name",       test_unique_name);
    g_test_add_func("/util/uri-list",          test_uri_list);
    g_test_add_func("/util/patterns",          test_patterns);
    g_test_add_func("/fsops/copy-tree",        test_copy_tree);
    g_test_add_func("/fsops/copy-into-itself", test_copy_into_itself);
    g_test_add_func("/fsops/copy-same-file",   test_copy_same_file);
    g_test_add_func("/fsops/conflicts",        test_conflicts);
    g_test_add_func("/fsops/move-merge",       test_move_merge);
    g_test_add_func("/fsops/move-skip",        test_move_skip_keeps_source);
    g_test_add_func("/fsops/cancel",           test_cancel_no_partial);
    g_test_add_func("/fsops/delete",           test_delete);
    g_test_add_func("/fsops/overwrite-ancestor", test_overwrite_ancestor);
    g_test_add_func("/fsops/overwrite-safe",   test_overwrite_safe);
    g_test_add_func("/fsops/move-unreadable",  test_move_unreadable_keeps_source);
    return g_test_run();
}
