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

/* ── arcfs: archives as directories ──────────────────────────────── */

#ifdef HAVE_LIBARCHIVE
#include "arcfs.h"
#include <archive.h>
#include <archive_entry.h>

/* Build a tar with libarchive – lets the tests craft hostile entries
 * ("../x", symlinks pointing outside) that tar(1) would refuse. */
typedef struct { const char *path; const char *data; const char *link; gboolean dir; } TarItem;

static void make_tar(const char *file, const TarItem *items)
{
    struct archive *a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    g_assert_cmpint(archive_write_open_filename(a, file), ==, ARCHIVE_OK);
    for (const TarItem *it = items; it->path; it++) {
        struct archive_entry *e = archive_entry_new();
        archive_entry_set_pathname(e, it->path);
        archive_entry_set_mtime(e, 1000000000, 0);
        if (it->dir) {
            archive_entry_set_filetype(e, AE_IFDIR);
            archive_entry_set_perm(e, 0755);
        } else if (it->link) {
            archive_entry_set_filetype(e, AE_IFLNK);
            archive_entry_set_symlink(e, it->link);
            archive_entry_set_perm(e, 0777);
        } else {
            archive_entry_set_filetype(e, AE_IFREG);
            archive_entry_set_perm(e, 0640);
            archive_entry_set_size(e, (la_int64_t)strlen(it->data));
        }
        archive_write_header(a, e);
        if (!it->dir && !it->link) archive_write_data(a, it->data, strlen(it->data));
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
}

static FsAnswer arc_overwrite(FsCtx *c, const char *s, const char *d)
{
    (void)c; (void)s; (void)d;
    return FS_ANS_OVERWRITE;
}

static void test_arcfs_list(void)
{
    gchar *root = tmp_root(), *tar = J(root, "t.tar");
    static const TarItem items[] = {
        { "./top.txt",        "T",  NULL, FALSE },
        { "a/b/deep.txt",     "DD", NULL, FALSE },   /* a, a/b implied */
        { "../evil.txt",      "E",  NULL, FALSE },   /* dropped */
        { "a/ln",             NULL, "b/deep.txt", FALSE },
        { NULL, NULL, NULL, FALSE }
    };
    make_tar(tar, items);
    g_assert_true(arcfs_supported(tar));
    g_assert_false(arcfs_supported("x.txt"));

    GError *err = NULL;
    GPtrArray *e = arcfs_list(tar, &err);
    g_assert_no_error(err);
    /* top.txt, a, a/b, a/b/deep.txt, a/ln */
    g_assert_cmpuint(e->len, ==, 5);
    for (guint i = 0; i < e->len; i++) {
        ArcEntry *x = g_ptr_array_index(e, i);
        g_assert_true(fs_relpath_is_safe(x->path));
    }

    GPtrArray *rootc = arcfs_children(e, "");
    g_assert_cmpuint(rootc->len, ==, 2);                    /* a, top.txt */
    GPtrArray *ac = arcfs_children(e, "a");
    g_assert_cmpuint(ac->len, ==, 2);                       /* a/b, a/ln */
    gboolean saw_dir = FALSE, saw_link = FALSE;
    for (guint i = 0; i < ac->len; i++) {
        ArcEntry *x = g_ptr_array_index(ac, i);
        if (strcmp(x->path, "a/b") == 0) saw_dir = x->is_dir;
        if (strcmp(x->path, "a/ln") == 0) saw_link = g_strcmp0(x->link, "b/deep.txt") == 0;
    }
    g_assert_true(saw_dir && saw_link);

    const char *sel[] = { "a" };
    g_assert_cmpint(arcfs_size(e, sel, 1), ==, 2);

    g_ptr_array_unref(rootc); g_ptr_array_unref(ac); g_ptr_array_unref(e);
    g_assert_null(arcfs_list(root, &err));                  /* a directory */
    g_clear_error(&err);
    rm_rf(root);
    g_free(tar); g_free(root);
}

static void test_arcfs_extract(void)
{
    gchar *root = tmp_root(), *tar = J(root, "t.tar"), *out = J(root, "out");
    static const TarItem items[] = {
        { "a/b/x.txt",  "X",  NULL, FALSE },
        { "a/b/s/y.txt","Y",  NULL, FALSE },
        { "a/ln",       NULL, "b/x.txt", FALSE },
        { "other.txt",  "O",  NULL, FALSE },
        { NULL, NULL, NULL, FALSE }
    };
    make_tar(tar, items);

    /* from inside "a/b": the base is stripped */
    FsCtx c = { .conflict = arc_overwrite };
    const char *sel[] = { "a/b/x.txt", "a/b/s" };
    g_assert_cmpint(arcfs_extract(&c, tar, "a/b", sel, 2, out), ==, FS_OK);
    gchar *x = J(out, "x.txt"), *y = g_build_filename(out, "s", "y.txt", NULL);
    gchar *dx = read_file(x), *dy = read_file(y);
    g_assert_cmpstr(dx, ==, "X");
    g_assert_cmpstr(dy, ==, "Y");
    struct stat st;
    g_assert_cmpint(stat(x, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0640);
    g_assert_cmpint(st.st_mtime, ==, 1000000000);
    gchar *other = J(out, "other.txt");
    g_assert_false(g_file_test(other, G_FILE_TEST_EXISTS));
    g_free(other);

    /* overwrite keeps working, no temp files left */
    g_assert_cmpint(arcfs_extract(&c, tar, "a/b", sel, 1, out), ==, FS_OK);
    GDir *d = g_dir_open(out, 0, NULL);
    const char *n;
    while ((n = g_dir_read_name(d))) g_assert_null(strstr(n, ".fm-"));
    g_dir_close(d);

    /* symlinks are recreated, not followed */
    const char *sel2[] = { "a/ln" };
    g_assert_cmpint(arcfs_extract(&c, tar, "a", sel2, 1, out), ==, FS_OK);
    gchar *ln = J(out, "ln");
    gchar *t = g_file_read_link(ln, NULL);
    g_assert_cmpstr(t, ==, "b/x.txt");

    fs_ctx_clear(&c);
    g_free(t); g_free(ln); g_free(dx); g_free(dy); g_free(x); g_free(y);
    rm_rf(root);
    g_free(tar); g_free(out); g_free(root);
}

/* An archive that plants a symlink to a directory outside and then
 * writes "through" it must not touch the outside directory. */
static void test_arcfs_symlink_escape(void)
{
    gchar *root = tmp_root(), *tar = J(root, "evil.tar");
    gchar *out = J(root, "out"), *outside = J(root, "outside");
    g_mkdir(outside, 0755);
    TarItem items[] = {
        { "esc",         NULL, outside, FALSE },
        { "esc/pwn.txt", "P",  NULL,    FALSE },
        { NULL, NULL, NULL, FALSE }
    };
    make_tar(tar, items);

    FsCtx c = { .conflict = arc_overwrite };
    const char *sel[] = { "esc" };
    int r = arcfs_extract(&c, tar, "", sel, 1, out);
    g_assert_cmpint(r, ==, FS_ERR);
    gchar *pwn = J(outside, "pwn.txt");
    g_assert_false(g_file_test(pwn, G_FILE_TEST_EXISTS));
    fs_ctx_clear(&c);
    rm_rf(root);
    g_free(pwn); g_free(tar); g_free(out); g_free(outside); g_free(root);
}
#endif /* HAVE_LIBARCHIVE */

static void check_mrn(const char *name, gboolean dir, const char *mask, const char *find,
                      const char *repl, MrnCase cs, guint n, guint dg, const char *want)
{
    gchar *r = mrn_apply(name, dir, mask, find, repl, cs, n, dg);
    g_assert_cmpstr(r, ==, want);
    g_free(r);
}

static void test_multi_rename(void)
{
    check_mrn("photo.jpg",  FALSE, "[N].[E]",          "", "", MRN_CASE_KEEP, 1, 1, "photo.jpg");
    check_mrn("photo.jpg",  FALSE, "img_[C].[E]",      "", "", MRN_CASE_KEEP, 7, 3, "img_007.jpg");
    check_mrn("a.tar.gz",   FALSE, "[N]_x.[E]",        "", "", MRN_CASE_KEEP, 1, 1, "a.tar_x.gz");
    check_mrn("README",     FALSE, "[N].[E]",          "", "", MRN_CASE_KEEP, 1, 1, "README");
    check_mrn(".bashrc",    FALSE, "[N].[E]",          "", "", MRN_CASE_KEEP, 1, 1, ".bashrc");
    check_mrn("my.dir",     TRUE,  "[N]-[C]",          "", "", MRN_CASE_KEEP, 2, 2, "my.dir-02");
    check_mrn("a b c.txt",  FALSE, "[N].[E]",          " ", "_", MRN_CASE_KEEP, 1, 1, "a_b_c.txt");
    check_mrn("Foto.JPG",   FALSE, "[N].[E]",          "", "", MRN_CASE_LOWER, 1, 1, "foto.jpg");
    check_mrn("čaj.txt",    FALSE, "[N].[E]",          "", "", MRN_CASE_UPPER, 1, 1, "ČAJ.TXT");
}

/* ── content search (fs_file_contains) ───────────────────────────── */

static gchar *contains_in(const char *dir, const char *data, gssize len,
                          const char *needle, int *line)
{
    gchar *f = J(dir, "c.txt");
    g_assert_true(g_file_set_contents(f, data, len, NULL));
    gchar *r = fs_file_contains(f, needle, line, NULL);
    g_free(f);
    return r;
}

static void test_content_basic(void)
{
    gchar *d = tmp_root();
    int line = 0;
    gchar *r = contains_in(d, "alpha\nbeta\n  Hello World  \n", -1, "hello", &line);
    g_assert_cmpstr(r, ==, "Hello World");          /* stripped snippet */
    g_assert_cmpint(line, ==, 3);
    g_free(r);

    g_assert_null(contains_in(d, "alpha\nbeta\n", -1, "gamma", &line));
    /* binary (NUL in the first 512 bytes) is skipped */
    g_assert_null(contains_in(d, "abc\0hello", 9, "hello", &line));
    /* non-ASCII needle, case-insensitive */
    r = contains_in(d, "x\nŽLTÝ kôň\n", -1, "žltý", &line);
    g_assert_nonnull(r);
    g_assert_cmpint(line, ==, 2);
    g_free(r);
    rm_rf(d); g_free(d);
}

/* a needle spanning the 4 KB read boundary of a long line */
static void test_content_chunk_boundary(void)
{
    gchar *d = tmp_root();
    for (int off = 4090; off <= 4100; off++) {
        GString *s = g_string_new("first\n");
        for (int i = 0; i < off; i++) g_string_append_c(s, 'a');
        g_string_append(s, "NEEDLE");
        for (int i = 0; i < 9000; i++) g_string_append_c(s, 'b');
        g_string_append(s, "\nlast NEEDLE\n");
        int line = 0;
        gchar *r = contains_in(d, s->str, (gssize)s->len, "needle", &line);
        g_assert_nonnull(r);
        g_assert_cmpint(line, ==, 2);            /* the long line, not "last" */
        g_assert_true(g_utf8_validate(r, -1, NULL));
        g_assert_cmpint(g_utf8_strlen(r, -1), <=, 200);
        g_free(r);
        g_string_free(s, TRUE);
    }
    /* line counting across a multi-chunk line */
    GString *s = g_string_new(NULL);
    for (int i = 0; i < 20000; i++) g_string_append_c(s, 'z');
    g_string_append(s, "\nfoo\nbar target\n");
    int line = 0;
    gchar *r = contains_in(d, s->str, (gssize)s->len, "target", &line);
    g_assert_cmpint(line, ==, 3);
    g_free(r);
    g_string_free(s, TRUE);
    rm_rf(d); g_free(d);
}

/* invalid UTF-8 in a long matching line must not read/write out of bounds */
static void test_content_invalid_utf8(void)
{
    gchar *d = tmp_root();
    GString *s = g_string_new(NULL);
    g_string_append(s, "match ");
    for (int i = 0; i < 4080; i++) g_string_append_c(s, (char)0xF0);  /* lead bytes */
    g_string_append(s, "\n");
    int line = 0;
    gchar *r = contains_in(d, s->str, (gssize)s->len, "match", &line);
    g_assert_nonnull(r);
    g_assert_true(g_utf8_validate(r, -1, NULL));
    g_assert_cmpint(g_utf8_strlen(r, -1), <=, 200);
    g_free(r);
    g_string_free(s, TRUE);

    gint cancelled = 1;              /* cancelled before start: no result */
    gchar *f = J(d, "c.txt");
    g_assert_null(fs_file_contains(f, "match", NULL, &cancelled));
    g_assert_null(fs_file_contains("/nonexistent/x", "a", NULL, NULL));
    g_free(f);
    rm_rf(d); g_free(d);
}

static void test_parse_size(void)
{
    g_assert_cmpint(fs_parse_size("", 0), ==, -1);
    g_assert_cmpint(fs_parse_size(NULL, 0), ==, -1);
    g_assert_cmpint(fs_parse_size("abc", 0), ==, -1);
    g_assert_cmpint(fs_parse_size("-1", 0), ==, -1);
    g_assert_cmpint(fs_parse_size("10", 0), ==, 10);
    g_assert_cmpint(fs_parse_size("1.5", 1), ==, 1536);
    g_assert_cmpint(fs_parse_size("2", 3), ==, (gint64)2 << 30);
    /* out-of-range unit index (e.g. GTK_INVALID_LIST_POSITION) → bytes */
    g_assert_cmpint(fs_parse_size("7", 99), ==, 7);
    g_assert_cmpint(fs_parse_size("7", -1), ==, 7);
    g_assert_cmpint(fs_parse_size("1e30", 3), ==, G_MAXINT64);
}

static void test_fmt_date(void)
{
    gchar *a = fmt_date(0);
    g_assert_nonnull(a);
    g_free(a);
    gchar *b = fmt_date(1767225600);           /* 2026-01-01 UTC */
    g_assert_true(g_str_has_prefix(b, "2025-12-31") || g_str_has_prefix(b, "2026-01-01"));
    g_free(b);
}

static void test_relpath_safe(void)
{
    g_assert_true (fs_relpath_is_safe("a/b/c"));
    g_assert_false(fs_relpath_is_safe("/abs"));
    g_assert_false(fs_relpath_is_safe("a/../b"));
    g_assert_false(fs_relpath_is_safe(".."));
    g_assert_false(fs_relpath_is_safe(""));
}


/* ── checksums / compare ─────────────────────────────────────────── */

static void test_checksum_parse(void)
{
    gchar *h, *n;
    g_assert_true(fs_parse_checksum_line(
        "d41d8cd98f00b204e9800998ecf8427e  empty file.txt\n", &h, &n));
    g_assert_cmpstr(h, ==, "d41d8cd98f00b204e9800998ecf8427e");
    g_assert_cmpstr(n, ==, "empty file.txt");
    g_free(h); g_free(n);
    g_assert_true(fs_parse_checksum_line(
        "D41D8CD98F00B204E9800998ECF8427E *bin.iso\r\n", &h, &n));
    g_assert_cmpstr(h, ==, "d41d8cd98f00b204e9800998ecf8427e");
    g_assert_cmpstr(n, ==, "bin.iso");
    g_free(h); g_free(n);
    g_assert_true(fs_parse_checksum_line(
        "MD5 (a (1).txt) = d41d8cd98f00b204e9800998ecf8427e", &h, &n));
    g_assert_cmpstr(n, ==, "a (1).txt");
    g_free(h); g_free(n);
    g_assert_false(fs_parse_checksum_line("# comment", &h, &n));
    g_assert_false(fs_parse_checksum_line("", &h, &n));
    g_assert_false(fs_parse_checksum_line("abc  short", &h, &n));
    g_assert_false(fs_parse_checksum_line(
        "d41d8cd98f00b204e9800998ecf8427e nospace", &h, &n));
    g_assert_null(h); g_assert_null(n);

    GChecksumType t;
    g_assert_true(fs_checksum_type_for_hex(64, &t));
    g_assert_cmpint(t, ==, G_CHECKSUM_SHA256);
    g_assert_false(fs_checksum_type_for_hex(10, &t));
}

static void test_checksum_file(void)
{
    gchar *root = tmp_root();
    gchar *f = g_build_filename(root, "abc.txt", NULL);
    write_file(f, "abc");
    FsCtx ctx = { 0 };
    gchar *s = fs_checksum_file(&ctx, f, G_CHECKSUM_SHA256);
    g_assert_cmpstr(s, ==,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    g_free(s);
    s = fs_checksum_file(&ctx, f, G_CHECKSUM_MD5);
    g_assert_cmpstr(s, ==, "900150983cd24fb0d6963f7d28e17f72");
    g_free(s);
    gchar *missing = g_build_filename(root, "nope", NULL);
    g_assert_null(fs_checksum_file(&ctx, missing, G_CHECKSUM_MD5));
    g_assert_nonnull(ctx.error);
    fs_ctx_clear(&ctx);
    g_unlink(f);
    g_rmdir(root);
    g_free(missing); g_free(f); g_free(root);
}

/* st_size 0 but content (procfs): copy_file_range copies nothing there –
 * the read/write fallback must still copy the text */
static void test_copy_virtual_file(void)
{
    gchar *root = tmp_root();
    gchar *dst = g_build_filename(root, "status", NULL);
    FsCtx ctx = { 0 };
    g_assert_cmpint(fs_copy(&ctx, "/proc/self/status", dst), ==, FS_OK);
    gchar *s = read_file(dst);
    g_assert_nonnull(strstr(s, "Name:"));
    g_free(s);
    fs_ctx_clear(&ctx);
    g_unlink(dst);
    g_rmdir(root);
    g_free(dst); g_free(root);
}

static void test_net_uri(void)
{
    gchar *u;
    u = fs_net_normalize_uri("  nas/share "); g_assert_cmpstr(u, ==, "smb://nas/share"); g_free(u);
    u = fs_net_normalize_uri("//nas/share");  g_assert_cmpstr(u, ==, "smb://nas/share"); g_free(u);
    u = fs_net_normalize_uri("\\\\nas\\share"); g_assert_cmpstr(u, ==, "smb://nas/share"); g_free(u);
    u = fs_net_normalize_uri("davs://h/x");   g_assert_cmpstr(u, ==, "davs://h/x"); g_free(u);
    g_assert_null(fs_net_normalize_uri("file:///etc"));
    g_assert_null(fs_net_normalize_uri("   "));
}

static void test_compare(void)
{
    g_assert_cmpint(fs_compare_files(FALSE, 1, 100, 0, 0, 2), ==, FS_CMP_ONLY);
    g_assert_cmpint(fs_compare_files(TRUE, 1, 100, 1, 101, 2), ==, FS_CMP_SAME);
    g_assert_cmpint(fs_compare_files(TRUE, 1, 100, 2, 100, 2), ==, FS_CMP_DIFFERENT);
    g_assert_cmpint(fs_compare_files(TRUE, 1, 200, 1, 100, 2), ==, FS_CMP_NEWER);
    g_assert_cmpint(fs_compare_files(TRUE, 1, 100, 1, 200, 2), ==, FS_CMP_OLDER);
}


/* ── added: core helpers without coverage ────────────────────────── */

static void test_worst_and_cancel(void)
{
    g_assert_cmpint(fs_worst(FS_OK, FS_ERR), ==, FS_ERR);
    g_assert_cmpint(fs_worst(FS_ERR, FS_OK), ==, FS_ERR);
    g_assert_cmpint(fs_worst(FS_SKIPPED, FS_CANCELLED), ==, FS_CANCELLED);
    g_assert_cmpint(fs_worst(FS_OK, FS_OK), ==, FS_OK);
    FsCtx c = { 0 };
    g_assert_false(fs_cancelled(&c));
    fs_cancel(&c);
    g_assert_true(fs_cancelled(&c));
}

static void test_checksum_names(void)
{
    static const struct { gsize len; GChecksumType t; } m[] = {
        { 32, G_CHECKSUM_MD5 }, { 40, G_CHECKSUM_SHA1 },
        { 64, G_CHECKSUM_SHA256 }, { 128, G_CHECKSUM_SHA512 },
    };
    for (guint i = 0; i < G_N_ELEMENTS(m); i++) {
        GChecksumType t;
        g_assert_true(fs_checksum_type_for_hex(m[i].len, &t));
        g_assert_cmpint(t, ==, m[i].t);
        g_assert_nonnull(fs_checksum_name(t));
        g_assert_cmpuint(strlen(fs_checksum_name(t)), >, 0);
        const char *ext = fs_checksum_ext(t);
        g_assert_nonnull(ext);
        g_assert_true(ext[0] == '.');
    }
    GChecksumType t;
    g_assert_false(fs_checksum_type_for_hex(33, &t));
    g_assert_false(fs_checksum_type_for_hex(0, &t));
}

static void test_calc_size(void)
{
    gchar *root = tmp_root(), *d = J(root, "d");
    g_assert_cmpint(g_mkdir(d, 0755), ==, 0);
    gchar *a = J(d, "a"), *b = J(d, "b"), *ln = J(d, "ln");
    write_file(a, "12345");
    write_file(b, "123");
    g_assert_cmpint(symlink("/nonexistent-target-xyz", ln), ==, 0);
    FsCtx c = { 0 };
    gint64 sz = fs_calc_size(&c, d);
    /* files 8 B + the link itself (not followed) */
    g_assert_cmpint(sz, >=, 8);
    g_assert_cmpint(sz, <=, 8 + (gint64)strlen("/nonexistent-target-xyz"));
    g_assert_cmpint(fs_calc_size(&c, a), ==, 5);
    FsCtx cc = { 0 };
    fs_cancel(&cc);
    g_assert_cmpint(fs_calc_size(&cc, d), <=, 8 + 64);  /* must not hang/crash */
    fs_ctx_clear(&c); fs_ctx_clear(&cc);
    rm_rf(root);
    g_free(a); g_free(b); g_free(ln); g_free(d); g_free(root);
}

static void test_local_exists(void)
{
    gchar *root = tmp_root(), *f = J(root, "f"), *ln = J(root, "dangling");
    write_file(f, "x");
    g_assert_cmpint(symlink("/nonexistent-target-xyz", ln), ==, 0);
    g_assert_true(fs_local_exists(root, "f", NULL));
    g_assert_false(fs_local_exists(root, "missing", NULL));
    g_assert_true(fs_local_exists(root, "dangling", NULL));   /* lstat */
    rm_rf(root);
    g_free(f); g_free(ln); g_free(root);
}

static void test_content_extra(void)
{
    gchar *root = tmp_root(), *t = J(root, "t.txt"), *bin = J(root, "b.bin");
    write_file(t, "first\nsecond line\nŽLUŤ kôň here\n");
    int line = 0;
    gchar *needle = g_utf8_strdown("žluť", -1);
    gchar *hit = fs_file_contains(t, needle, &line, NULL);
    g_assert_nonnull(hit);
    g_assert_cmpint(line, ==, 3);
    g_assert_true(g_utf8_validate(hit, -1, NULL));
    g_free(hit);

    gint cancelled = 1;
    g_assert_null(fs_file_contains(t, "second", &line, &cancelled));

    static const char bdata[] = "abc\0second\0\0\0";
    g_assert_true(g_file_set_contents(bin, bdata, sizeof bdata, NULL));
    g_assert_null(fs_file_contains(bin, "second", &line, NULL));

    g_assert_null(fs_file_contains(t, "notthere", &line, NULL));
    g_free(needle);
    rm_rf(root);
    g_free(t); g_free(bin); g_free(root);
}

#ifdef HAVE_LIBARCHIVE
static FsAnswer arc_skip(FsCtx *c, const char *s, const char *d)
{
    (void)c; (void)s; (void)d;
    return FS_ANS_SKIP;
}

/* base "a" must not match a sibling "ab/…" and an entry equal to base
 * must not read past its end */
static void test_arcfs_base_prefix(void)
{
    gchar *root = tmp_root(), *tar = J(root, "t.tar"), *out = J(root, "out");
    static const TarItem items[] = {
        { "a",      NULL, NULL, TRUE  },
        { "a/x",    "X",  NULL, FALSE },
        { "ab/y",   "YY", NULL, FALSE },
        { NULL, NULL, NULL, FALSE }
    };
    make_tar(tar, items);

    GError *err = NULL;
    GPtrArray *e = arcfs_list(tar, &err);
    g_assert_no_error(err);
    const char *sa[] = { "a" };
    g_assert_cmpint(arcfs_size(e, sa, 1), ==, 1);      /* not ab/y */
    GPtrArray *ac = arcfs_children(e, "a");
    g_assert_cmpuint(ac->len, ==, 1);
    g_ptr_array_unref(ac);
    GPtrArray *rc = arcfs_children(e, "");
    g_assert_cmpuint(rc->len, ==, 2);                  /* a, ab */
    g_ptr_array_unref(rc);
    g_ptr_array_unref(e);

    FsCtx c = { .conflict = arc_overwrite };
    const char *sel[] = { "a/x" };
    g_assert_cmpint(arcfs_extract(&c, tar, "a", sel, 1, out), ==, FS_OK);
    gchar *x = J(out, "x"), *y = J(out, "y"), *ab = J(out, "ab");
    g_assert_true(g_file_test(x, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(y, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(ab, G_FILE_TEST_EXISTS));
    fs_ctx_clear(&c);

    /* conflicts: skip keeps the existing file, overwrite replaces it */
    write_file(x, "OLD");
    FsCtx s = { .conflict = arc_skip };
    g_assert_cmpint(arcfs_extract(&s, tar, "a", sel, 1, out), ==, FS_SKIPPED);
    gchar *d = read_file(x); g_assert_cmpstr(d, ==, "OLD"); g_free(d);
    fs_ctx_clear(&s);
    FsCtx o = { .policy = FS_OVERWRITE_ALL };
    g_assert_cmpint(arcfs_extract(&o, tar, "a", sel, 1, out), ==, FS_OK);
    d = read_file(x); g_assert_cmpstr(d, ==, "X"); g_free(d);
    fs_ctx_clear(&o);

    /* cancelled before start */
    FsCtx k = { .conflict = arc_overwrite };
    fs_cancel(&k);
    g_assert_cmpint(arcfs_extract(&k, tar, "a", sel, 1, out), ==, FS_CANCELLED);
    fs_ctx_clear(&k);

    rm_rf(root);
    g_free(x); g_free(y); g_free(ab); g_free(tar); g_free(out); g_free(root);
}

/* existing destination dirs keep their own mode/mtime; new ones get the
 * archive's */
static void test_arcfs_existing_dir_untouched(void)
{
    gchar *root = tmp_root(), *tar = J(root, "t.tar"), *out = J(root, "out");
    static const TarItem items[] = {
        { "a",   NULL, NULL, TRUE  },
        { "a/x", "X",  NULL, FALSE },
        { "b",   NULL, NULL, TRUE  },
        { "b/y", "Y",  NULL, FALSE },
        { NULL, NULL, NULL, FALSE }
    };
    make_tar(tar, items);
    gchar *a = J(out, "a"), *b = J(out, "b");
    g_assert_cmpint(g_mkdir_with_parents(a, 0700), ==, 0);
    chmod(a, 0700);

    FsCtx c = { .conflict = arc_overwrite };
    const char *sel[] = { "a", "b" };
    g_assert_cmpint(arcfs_extract(&c, tar, "", sel, 2, out), ==, FS_OK);
    fs_ctx_clear(&c);

    struct stat st;
    g_assert_cmpint(lstat(a, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 07777, ==, 0700);   /* not the archive's 0755 */
    g_assert_cmpint(lstat(b, &st), ==, 0);
    g_assert_cmpint((gint64)st.st_mtime, ==, 1000000000);

    rm_rf(root);
    g_free(a); g_free(b); g_free(tar); g_free(out); g_free(root);
}

/* streamed zip entries carry no size in the header: the written data
 * must survive (it used to be truncated to 0 bytes) */
static void test_arcfs_zip_no_size(void)
{
    gchar *root = tmp_root(), *zip = J(root, "t.zip"), *out = J(root, "out");
    struct archive *w = archive_write_new();
    archive_write_set_format_zip(w);
    g_assert_cmpint(archive_write_open_filename(w, zip), ==, ARCHIVE_OK);
    struct archive_entry *e = archive_entry_new();
    archive_entry_set_pathname(e, "s.txt");
    archive_entry_set_filetype(e, AE_IFREG);
    archive_entry_set_perm(e, 0644);
    archive_entry_unset_size(e);
    archive_write_header(w, e);
    archive_write_data(w, "streamed", 8);
    archive_entry_free(e);
    archive_write_close(w);
    archive_write_free(w);

    g_mkdir_with_parents(out, 0755);
    FsCtx c = { .conflict = arc_overwrite };
    const char *sel[] = { "s.txt" };
    g_assert_cmpint(arcfs_extract(&c, zip, "", sel, 1, out), ==, FS_OK);
    fs_ctx_clear(&c);
    gchar *s = J(out, "s.txt"), *d = read_file(s);
    g_assert_cmpstr(d, ==, "streamed");
    g_free(d); g_free(s);
    rm_rf(root);
    g_free(zip); g_free(out); g_free(root);
}
#endif /* HAVE_LIBARCHIVE */

/* a cancelled walk stops at once (move_any's progress size uses it) */
static void test_calc_size_cancelled(void)
{
    gchar *root = tmp_root(), *f = J(root, "f");
    write_file(f, "12345");
    FsCtx c = { 0 };
    g_assert_cmpint(fs_calc_size(&c, root), ==, 5);
    fs_cancel(&c);
    g_assert_cmpint(fs_calc_size(&c, root), ==, 0);
    fs_ctx_clear(&c);
    rm_rf(root);
    g_free(f); g_free(root);
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
    g_test_add_func("/util/multi-rename",      test_multi_rename);
    g_test_add_func("/util/content-basic",     test_content_basic);
    g_test_add_func("/util/content-chunks",    test_content_chunk_boundary);
    g_test_add_func("/util/content-bad-utf8",  test_content_invalid_utf8);
    g_test_add_func("/util/parse-size",        test_parse_size);
    g_test_add_func("/util/fmt-date",          test_fmt_date);
    g_test_add_func("/util/relpath-safe",      test_relpath_safe);
    g_test_add_func("/util/checksum-parse",    test_checksum_parse);
    g_test_add_func("/util/compare",           test_compare);
    g_test_add_func("/util/net-uri",           test_net_uri);
    g_test_add_func("/fsops/checksum-file",    test_checksum_file);
    g_test_add_func("/fsops/copy-virtual",     test_copy_virtual_file);
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
#ifdef HAVE_LIBARCHIVE
    g_test_add_func("/arcfs/list",             test_arcfs_list);
    g_test_add_func("/arcfs/extract",          test_arcfs_extract);
    g_test_add_func("/arcfs/symlink-escape",   test_arcfs_symlink_escape);
    g_test_add_func("/arcfs/base-prefix",      test_arcfs_base_prefix);
    g_test_add_func("/arcfs/existing-dir",     test_arcfs_existing_dir_untouched);
    g_test_add_func("/arcfs/zip-no-size",      test_arcfs_zip_no_size);
#endif
    g_test_add_func("/fsops/worst-cancel",     test_worst_and_cancel);
    g_test_add_func("/fsops/calc-size",        test_calc_size);
    g_test_add_func("/util/checksum-names",    test_checksum_names);
    g_test_add_func("/util/local-exists",      test_local_exists);
    g_test_add_func("/util/content-extra",     test_content_extra);
    g_test_add_func("/fsops/calc-size-cancel", test_calc_size_cancelled);
    return g_test_run();
}
