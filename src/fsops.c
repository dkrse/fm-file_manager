#include "fsops.h"
#include "util.h"
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>

#define COPY_BUF (256 * 1024)

/* ─────────────────────────────────────────────────────────────────── *
 *  Context helpers
 * ─────────────────────────────────────────────────────────────────── */

void fs_ctx_clear(FsCtx *ctx)
{
    if (!ctx) return;
    g_clear_pointer(&ctx->error, g_free);
}

gboolean fs_cancelled(FsCtx *ctx)
{
    return ctx && g_atomic_int_get(&ctx->cancelled);
}

void fs_cancel(FsCtx *ctx)
{
    if (ctx) g_atomic_int_set(&ctx->cancelled, 1);
}

int fs_worst(int a, int b)
{
    return a > b ? a : b;
}

static void report(FsCtx *c, const char *name, gint64 bytes)
{
    if (c && c->progress) c->progress(c, name, bytes);
}

static void start_file(FsCtx *c, const char *src, const char *dst, gint64 size)
{
    if (c && c->file_start) c->file_start(c, src, dst, size);
}

static void count_file(FsCtx *c)
{
    if (c) g_atomic_int_inc(&c->files_done);
}

static int fail_msg(FsCtx *c, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static int fail_msg(FsCtx *c, const char *fmt, ...)
{
    if (c && !c->error) {
        va_list ap;
        va_start(ap, fmt);
        c->error = g_strdup_vprintf(fmt, ap);
        va_end(ap);
    }
    return FS_ERR;
}

static int fail(FsCtx *c, const char *path, int err)
{
    return fail_msg(c, "%s: %s", path, g_strerror(err));
}

/* Overwrite question → FS_OK (go ahead), FS_SKIPPED or FS_CANCELLED */
static int ask_conflict(FsCtx *c, const char *src, const char *dst)
{
    if (!c) return FS_SKIPPED;
    if (c->policy == FS_OVERWRITE_ALL) return FS_OK;
    if (c->policy == FS_SKIP_ALL)      return FS_SKIPPED;
    if (!c->conflict)                  return FS_SKIPPED;

    switch (c->conflict(c, src, dst)) {
    case FS_ANS_OVERWRITE:     return FS_OK;
    case FS_ANS_OVERWRITE_ALL: c->policy = FS_OVERWRITE_ALL; return FS_OK;
    case FS_ANS_SKIP:          return FS_SKIPPED;
    case FS_ANS_SKIP_ALL:      c->policy = FS_SKIP_ALL;      return FS_SKIPPED;
    case FS_ANS_CANCEL:
    default:
        fs_cancel(c);
        return FS_CANCELLED;
    }
}

static gboolean is_dot(const char *n)
{
    return n[0] == '.' && (n[1] == '\0' || (n[1] == '.' && n[2] == '\0'));
}

/* Refuse to copy/move a directory into itself (resolves symlinks) */
static gboolean dst_inside_src(const char *src, const char *dst)
{
    char *rs = realpath(src, NULL);
    if (!rs) return FALSE;
    gchar *dparent = g_path_get_dirname(dst);
    char *rp = realpath(dparent, NULL);
    g_free(dparent);
    gboolean inside = FALSE;
    if (rp) {
        gchar *base = g_path_get_basename(dst);
        gchar *rd = g_build_filename(rp, base, NULL);
        inside = fs_path_is_inside(rd, rs);
        g_free(base); g_free(rd);
        free(rp);
    }
    free(rs);
    return inside;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Local: size / delete
 * ─────────────────────────────────────────────────────────────────── */

gint64 fs_calc_size(FsCtx *ctx, const char *path)
{
    if (fs_cancelled(ctx)) return 0;

    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return st.st_size;

    gint64 total = 0;
    DIR *dir = opendir(path);
    if (!dir) return 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && !fs_cancelled(ctx)) {
        if (is_dot(de->d_name)) continue;
        gchar *sub = g_build_filename(path, de->d_name, NULL);
        total += fs_calc_size(ctx, sub);
        g_free(sub);
    }
    closedir(dir);
    return total;
}

static int delete_r(FsCtx *c, const char *path, gboolean count)
{
    if (fs_cancelled(c)) return FS_CANCELLED;

    struct stat st;
    if (lstat(path, &st) != 0) return fail(c, path, errno);

    if (!S_ISDIR(st.st_mode)) {
        if (count) start_file(c, path, NULL, 0);
        if (unlink(path) != 0) return fail(c, path, errno);
        if (count) {
            count_file(c);
            const char *slash = strrchr(path, '/');
            report(c, slash ? slash + 1 : path, 0);
        }
        return FS_OK;
    }

    DIR *dir = opendir(path);
    if (!dir) return fail(c, path, errno);
    int r = FS_OK;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (is_dot(de->d_name)) continue;
        gchar *sub = g_build_filename(path, de->d_name, NULL);
        r = fs_worst(r, delete_r(c, sub, count));
        g_free(sub);
        if (r == FS_CANCELLED) break;
    }
    closedir(dir);
    if (r != FS_OK) return r;
    if (rmdir(path) != 0) return fail(c, path, errno);
    return FS_OK;
}

int fs_delete(FsCtx *ctx, const char *path)
{
    return delete_r(ctx, path, TRUE);
}

int fs_trash(FsCtx *ctx, const char *path)
{
    if (fs_cancelled(ctx)) return FS_CANCELLED;
    start_file(ctx, path, NULL, 0);
    GFile *f = g_file_new_for_path(path);
    GError *err = NULL;
    gboolean ok = g_file_trash(f, NULL, &err);
    g_object_unref(f);
    if (!ok) {
        fail_msg(ctx, "%s: %s", path, err ? err->message : "cannot move to trash");
        g_clear_error(&err);
        return FS_ERR;
    }
    count_file(ctx);
    gchar *name = g_path_get_basename(path);
    report(ctx, name, 0);
    g_free(name);
    return FS_OK;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Local: copy
 * ─────────────────────────────────────────────────────────────────── */

/* Hidden temp name next to `path` – a replacement is written there and
 * renamed over the target only once it is complete. */
static gchar *tmp_sibling(const char *path)
{
    gchar *dir  = g_path_get_dirname(path);
    gchar *base = g_path_get_basename(path);
    gchar *t = g_strdup_printf("%s/.%s.fm-%06x", dir, base,
                               (unsigned)g_random_int_range(0, 0x1000000));
    g_free(dir); g_free(base);
    return t;
}

/* Would replacing the directory `dst` destroy `src`?  (src lies inside
 * it – e.g. moving /x/foo/foo onto /x/foo).  The last component of src
 * is not resolved: a symlink inside dst is still inside it. */
static gboolean src_inside_dst(const char *src, const char *dst)
{
    gchar *sparent = g_path_get_dirname(src);
    char *rsp = realpath(sparent, NULL);
    g_free(sparent);
    char *rd = realpath(dst, NULL);
    gboolean inside = FALSE;
    if (rsp && rd) {
        gchar *base = g_path_get_basename(src);
        gchar *rs = g_build_filename(rsp, base, NULL);
        inside = fs_path_is_inside(rs, rd);
        g_free(base); g_free(rs);
    }
    free(rsp); free(rd);
    return inside;
}

/* readdir() that tells end-of-directory (NULL, *err = 0) from errors */
static struct dirent *read_dir(DIR *dir, int *err)
{
    errno = 0;
    struct dirent *de = readdir(dir);
    *err = de ? 0 : errno;
    return de;
}

static void apply_attrs(const char *path, const struct stat *st)
{
    chmod(path, st->st_mode & 07777);
    struct timespec ts[2] = { st->st_atim, st->st_mtim };
    utimensat(AT_FDCWD, path, ts, 0);
}

/* tmp != NULL: write there and rename over dst on success */
static int copy_reg(FsCtx *c, const char *src, const char *dst, const char *tmp,
                    const struct stat *st, const char *name)
{
    const char *out = tmp ? tmp : dst;
    start_file(c, src, dst, st->st_size);
    int fin = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fin < 0) return fail(c, src, errno);

    /* O_EXCL: never write through an existing file or symlink */
    int fout = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fout < 0) { int e = errno; close(fin); return fail(c, dst, e); }

    char *buf = g_malloc(COPY_BUF);
    int r = FS_OK;
    ssize_t n;
    while ((n = read(fin, buf, COPY_BUF)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            r = fail(c, src, errno);
            break;
        }
        if (fs_cancelled(c)) { r = FS_CANCELLED; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(fout, buf + w, (size_t)(n - w));
            if (k < 0) {
                if (errno == EINTR) continue;
                r = fail(c, dst, errno);
                break;
            }
            w += k;
        }
        if (r != FS_OK) break;
        report(c, name, n);
    }
    g_free(buf);
    close(fin);

    if (r == FS_OK) {
        fchmod(fout, st->st_mode & 07777);
        struct timespec ts[2] = { st->st_atim, st->st_mtim };
        futimens(fout, ts);
        if (close(fout) != 0) r = fail(c, dst, errno);
    } else {
        close(fout);
    }
    if (r == FS_OK && tmp && rename(tmp, dst) != 0) r = fail(c, dst, errno);
    if (r != FS_OK) unlink(out);           /* no partial files */
    else            count_file(c);
    return r;
}

static int make_symlink(FsCtx *c, const char *target, const char *dst,
                        gboolean replace)
{
    gchar *tmp = replace ? tmp_sibling(dst) : NULL;
    int r = FS_OK;
    if (symlink(target, tmp ? tmp : dst) != 0) r = fail(c, dst, errno);
    else if (tmp && rename(tmp, dst) != 0) { r = fail(c, dst, errno); unlink(tmp); }
    if (r == FS_OK) count_file(c);
    g_free(tmp);
    return r;
}

static int copy_any(FsCtx *c, const char *src, const char *dst, gboolean confirmed);

static int copy_dir_contents(FsCtx *c, const char *src, const char *dst)
{
    DIR *dir = opendir(src);
    if (!dir) return fail(c, src, errno);
    int r = FS_OK, err;
    struct dirent *de;
    while ((de = read_dir(dir, &err)) != NULL) {
        if (is_dot(de->d_name)) continue;
        gchar *sp = g_build_filename(src, de->d_name, NULL);
        gchar *dp = g_build_filename(dst, de->d_name, NULL);
        r = fs_worst(r, copy_any(c, sp, dp, FALSE));
        g_free(sp); g_free(dp);
        if (r == FS_CANCELLED) break;
    }
    if (!de && err) r = fs_worst(r, fail(c, src, err));
    closedir(dir);
    return r;
}

/* confirmed: the overwrite question was already answered (move → EXDEV) */
static int copy_any(FsCtx *c, const char *src, const char *dst, gboolean confirmed)
{
    if (fs_cancelled(c)) return FS_CANCELLED;

    struct stat st, dt;
    if (lstat(src, &st) != 0) return fail(c, src, errno);

    gchar *name = g_path_get_basename(src);
    report(c, name, 0);
    int r = FS_OK;
    gboolean replace = FALSE;

    if (lstat(dst, &dt) == 0) {
        if (st.st_dev == dt.st_dev && st.st_ino == dt.st_ino) {
            r = fail_msg(c, "%s: source and destination are the same file", dst);
            goto out;
        }
        if (S_ISDIR(st.st_mode) && S_ISDIR(dt.st_mode)) {
            /* merge directories – conflicts are asked per file */
            r = copy_dir_contents(c, src, dst);
            if (r == FS_OK) apply_attrs(dst, &st);
            goto out;
        }
        if (S_ISDIR(dt.st_mode) && src_inside_dst(src, dst)) {
            r = fail_msg(c, "Cannot overwrite '%s' with an item inside it", dst);
            goto out;
        }
        if (!confirmed) r = ask_conflict(c, src, dst);
        if (r != FS_OK) goto out;
        /* file over file: written beside it and renamed over on success;
         * a directory on either side has to be removed first */
        if (S_ISDIR(st.st_mode) || S_ISDIR(dt.st_mode)) {
            r = delete_r(c, dst, FALSE);
            if (r != FS_OK) goto out;
        } else {
            replace = TRUE;
        }
    }

    if (S_ISLNK(st.st_mode)) {
        gchar *target = g_file_read_link(src, NULL);
        if (!target) r = fail(c, src, errno);
        else         r = make_symlink(c, target, dst, replace);
        g_free(target);
    } else if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst, 0700) != 0) { r = fail(c, dst, errno); goto out; }
        r = copy_dir_contents(c, src, dst);
        apply_attrs(dst, &st);
    } else if (S_ISREG(st.st_mode)) {
        gchar *tmp = replace ? tmp_sibling(dst) : NULL;
        r = copy_reg(c, src, dst, tmp, &st, name);
        g_free(tmp);
    } else {
        r = fail_msg(c, "%s: special files are not copied", src);
    }

out:
    g_free(name);
    return r;
}

int fs_copy(FsCtx *ctx, const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) == 0 && S_ISDIR(st.st_mode) && dst_inside_src(src, dst))
        return fail_msg(ctx, "Cannot copy '%s' into itself", src);
    return copy_any(ctx, src, dst, FALSE);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Local: move
 * ─────────────────────────────────────────────────────────────────── */

static int move_any(FsCtx *c, const char *src, const char *dst)
{
    if (fs_cancelled(c)) return FS_CANCELLED;

    struct stat st, dt;
    if (lstat(src, &st) != 0) return fail(c, src, errno);

    gchar *name = g_path_get_basename(src);
    report(c, name, 0);
    int r = FS_OK;
    gboolean confirmed = FALSE;

    if (lstat(dst, &dt) == 0) {
        if (st.st_dev == dt.st_dev && st.st_ino == dt.st_ino) {
            r = fail_msg(c, "%s: source and destination are the same file", dst);
            goto out;
        }
        if (S_ISDIR(st.st_mode) && S_ISDIR(dt.st_mode)) {
            /* merge: move children one by one, then remove empty source */
            DIR *dir = opendir(src);
            if (!dir) { r = fail(c, src, errno); goto out; }
            int err;
            struct dirent *de;
            while ((de = read_dir(dir, &err)) != NULL) {
                if (is_dot(de->d_name)) continue;
                gchar *sp = g_build_filename(src, de->d_name, NULL);
                gchar *dp = g_build_filename(dst, de->d_name, NULL);
                r = fs_worst(r, move_any(c, sp, dp));
                g_free(sp); g_free(dp);
                if (r == FS_CANCELLED) break;
            }
            if (!de && err) r = fs_worst(r, fail(c, src, err));
            closedir(dir);
            if (r == FS_OK && rmdir(src) != 0) r = fail(c, src, errno);
            goto out;
        }
        if (S_ISDIR(dt.st_mode) && src_inside_dst(src, dst)) {
            r = fail_msg(c, "Cannot overwrite '%s' with an item inside it", dst);
            goto out;
        }
        r = ask_conflict(c, src, dst);
        if (r != FS_OK) goto out;
        /* rename() replaces a file atomically; a directory on either
         * side has to be removed first */
        if (S_ISDIR(st.st_mode) || S_ISDIR(dt.st_mode)) {
            r = delete_r(c, dst, FALSE);
            if (r != FS_OK) goto out;
        } else {
            confirmed = TRUE;
        }
    }

    start_file(c, src, dst, 0);
    if (rename(src, dst) == 0) {
        gint64 bytes = S_ISDIR(st.st_mode)
                       ? (c && c->progress ? fs_calc_size(NULL, dst) : 0)
                       : st.st_size;
        report(c, name, bytes);
        count_file(c);
        goto out;
    }
    if (errno != EXDEV) { r = fail(c, src, errno); goto out; }

    /* different filesystem: copy, then delete the source – only when
     * everything was copied */
    r = copy_any(c, src, dst, confirmed);
    if (r == FS_OK) r = delete_r(c, src, FALSE);

out:
    g_free(name);
    return r;
}

int fs_move(FsCtx *ctx, const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) == 0 && S_ISDIR(st.st_mode) && dst_inside_src(src, dst))
        return fail_msg(ctx, "Cannot move '%s' into itself", src);
    return move_any(ctx, src, dst);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  SFTP
 * ─────────────────────────────────────────────────────────────────── */

#ifdef HAVE_LIBSSH2

#define NAME_BUF 1024

static int sftp_fail(FsCtx *c, LIBSSH2_SFTP *sftp, const char *path)
{
    unsigned long e = libssh2_sftp_last_error(sftp);
    const char *msg;
    switch (e) {
    case LIBSSH2_FX_NO_SUCH_FILE:       msg = "No such file or directory"; break;
    case LIBSSH2_FX_PERMISSION_DENIED:  msg = "Permission denied";         break;
    case LIBSSH2_FX_FILE_ALREADY_EXISTS:msg = "File exists";               break;
    case LIBSSH2_FX_NO_SPACE_ON_FILESYSTEM:
    case LIBSSH2_FX_QUOTA_EXCEEDED:     msg = "No space left";             break;
    case LIBSSH2_FX_DIR_NOT_EMPTY:      msg = "Directory not empty";       break;
    case LIBSSH2_FX_CONNECTION_LOST:
    case LIBSSH2_FX_NO_CONNECTION:      msg = "Connection lost";           break;
    default:                            msg = "SFTP error";                break;
    }
    return fail_msg(c, "%s: %s (%lu)", path, msg, e);
}

static gboolean attr_is_dir(const LIBSSH2_SFTP_ATTRIBUTES *a)
{
    return (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
           LIBSSH2_SFTP_S_ISDIR(a->permissions);
}

static gboolean attr_is_link(const LIBSSH2_SFTP_ATTRIBUTES *a)
{
    return (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
           LIBSSH2_SFTP_S_ISLNK(a->permissions);
}

gboolean fs_sftp_exists(LIBSSH2_SFTP *sftp, const char *path, gboolean *is_dir)
{
    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_lstat(sftp, path, &a) != 0) return FALSE;
    if (is_dir) *is_dir = attr_is_dir(&a);
    return TRUE;
}

gint64 fs_sftp_calc_size(FsCtx *ctx, LIBSSH2_SFTP *sftp, const char *path)
{
    if (fs_cancelled(ctx)) return 0;

    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_lstat(sftp, path, &a) != 0) return 0;
    if (!attr_is_dir(&a))
        return (a.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (gint64)a.filesize : 0;

    gint64 total = 0;
    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(sftp, path);
    if (!h) return 0;
    char name[NAME_BUF], longentry[NAME_BUF];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (libssh2_sftp_readdir_ex(h, name, sizeof(name), longentry,
                                   sizeof(longentry), &attrs) > 0) {
        if (fs_cancelled(ctx)) break;
        if (is_dot(name)) continue;
        if (attr_is_dir(&attrs)) {
            gchar *sub = fs_remote_join(path, name);
            total += fs_sftp_calc_size(ctx, sftp, sub);
            g_free(sub);
        } else if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
            total += (gint64)attrs.filesize;
        }
    }
    libssh2_sftp_closedir(h);
    return total;
}

static int sftp_delete_r(FsCtx *c, LIBSSH2_SFTP *sftp, const char *path,
                         gboolean count)
{
    if (fs_cancelled(c)) return FS_CANCELLED;

    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_lstat(sftp, path, &a) != 0) return sftp_fail(c, sftp, path);

    if (!attr_is_dir(&a)) {
        if (count) start_file(c, path, NULL, 0);
        if (libssh2_sftp_unlink(sftp, path) != 0) return sftp_fail(c, sftp, path);
        if (count) {
            count_file(c);
            const char *slash = strrchr(path, '/');
            report(c, slash ? slash + 1 : path, 0);
        }
        return FS_OK;
    }

    LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(sftp, path);
    if (!h) return sftp_fail(c, sftp, path);

    /* Collect names first – deleting while iterating is not portable */
    GPtrArray *names = g_ptr_array_new_with_free_func(g_free);
    char name[NAME_BUF], longentry[NAME_BUF];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int rc;
    while ((rc = libssh2_sftp_readdir_ex(h, name, sizeof(name), longentry,
                                         sizeof(longentry), &attrs)) > 0) {
        if (!is_dot(name)) g_ptr_array_add(names, g_strdup(name));
    }
    int r = rc < 0 ? sftp_fail(c, sftp, path) : FS_OK;
    libssh2_sftp_closedir(h);

    for (guint i = 0; i < names->len; i++) {
        gchar *sub = fs_remote_join(path, names->pdata[i]);
        r = fs_worst(r, sftp_delete_r(c, sftp, sub, count));
        g_free(sub);
        if (r == FS_CANCELLED) break;
    }
    g_ptr_array_unref(names);
    if (r != FS_OK) return r;
    if (libssh2_sftp_rmdir(sftp, path) != 0) return sftp_fail(c, sftp, path);
    return FS_OK;
}

int fs_sftp_delete(FsCtx *ctx, LIBSSH2_SFTP *sftp, const char *path)
{
    return sftp_delete_r(ctx, sftp, path, TRUE);
}

gboolean fs_sftp_replace(LIBSSH2_SFTP *sftp, const char *from, const char *to)
{
#ifdef libssh2_sftp_posix_rename
    /* OpenSSH extension: atomic rename(2) semantics */
    if (libssh2_sftp_posix_rename(sftp, from, to) == 0) return TRUE;
#endif
    if (libssh2_sftp_rename_ex(sftp, from, (unsigned)strlen(from),
                               to, (unsigned)strlen(to),
                               LIBSSH2_SFTP_RENAME_OVERWRITE |
                               LIBSSH2_SFTP_RENAME_ATOMIC |
                               LIBSSH2_SFTP_RENAME_NATIVE) == 0)
        return TRUE;
    /* SFTP v3 servers refuse to rename over an existing file */
    libssh2_sftp_unlink(sftp, to);
    return libssh2_sftp_rename_ex(sftp, from, (unsigned)strlen(from),
                                  to, (unsigned)strlen(to), 0) == 0;
}

/* ── upload ───────────────────────────────────────────────────────── */

/* tmp != NULL: write there and rename over remote on success */
static int upload_file(FsCtx *c, LIBSSH2_SFTP *sftp, const char *local,
                       const char *remote, const char *tmp,
                       const struct stat *st, const char *name)
{
    const char *out = tmp ? tmp : remote;
    start_file(c, local, remote, st->st_size);
    int fin = open(local, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fin < 0) return fail(c, local, errno);

    LIBSSH2_SFTP_HANDLE *fout = libssh2_sftp_open(sftp, out,
        LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT |
        (tmp ? LIBSSH2_FXF_EXCL : LIBSSH2_FXF_TRUNC),
        (long)(st->st_mode & 0777));
    if (!fout) { close(fin); return sftp_fail(c, sftp, remote); }

    char *buf = g_malloc(COPY_BUF);
    int r = FS_OK;
    ssize_t n;
    while ((n = read(fin, buf, COPY_BUF)) != 0) {
        if (n < 0) {
            if (errno == EINTR) continue;
            r = fail(c, local, errno);
            break;
        }
        if (fs_cancelled(c)) { r = FS_CANCELLED; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = libssh2_sftp_write(fout, buf + w, (size_t)(n - w));
            if (k < 0) { r = sftp_fail(c, sftp, remote); break; }
            w += k;
        }
        if (r != FS_OK) break;
        report(c, name, n);
    }
    g_free(buf);
    close(fin);
    libssh2_sftp_close(fout);

    if (r == FS_OK) {
        LIBSSH2_SFTP_ATTRIBUTES a = { 0 };
        a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
        a.atime = (unsigned long)st->st_atime;
        a.mtime = (unsigned long)st->st_mtime;
        libssh2_sftp_setstat(sftp, out, &a);
        if (tmp && !fs_sftp_replace(sftp, tmp, remote))
            r = sftp_fail(c, sftp, remote);
    }
    if (r != FS_OK) {
        libssh2_sftp_unlink(sftp, out);       /* no partial files */
        return r;
    }
    count_file(c);
    return FS_OK;
}

/* Symlinks are recreated, never followed: a link may point anywhere on
 * the local machine (e.g. one downloaded from another server). */
static int upload_link(FsCtx *c, LIBSSH2_SFTP *sftp, const char *local,
                       const char *remote, gboolean replace)
{
    gchar *target = g_file_read_link(local, NULL);
    if (!target) return fail(c, local, errno);
    gchar *tmp = replace ? tmp_sibling(remote) : NULL;
    int r = FS_OK;
    /* OpenSSH swaps the SSH_FXP_SYMLINK arguments; libssh2 sends them as
     * (target, link) which is what OpenSSH expects */
    const char *link = tmp ? tmp : remote;
    if (libssh2_sftp_symlink(sftp, target, (char *)link) != 0) {
        /* some libssh2 versions return a positive value on success –
         * trust the server: did the link appear with our target? */
        char buf[4096];
        int n = libssh2_sftp_readlink(sftp, link, buf, sizeof(buf) - 1);
        if (n < 0 || (buf[n] = '\0', strcmp(buf, target) != 0))
            r = sftp_fail(c, sftp, remote);
    }
    if (r == FS_OK && tmp && !fs_sftp_replace(sftp, tmp, remote)) {
        r = sftp_fail(c, sftp, remote);
        libssh2_sftp_unlink(sftp, tmp);
    }
    if (r == FS_OK) count_file(c);
    g_free(tmp); g_free(target);
    return r;
}

static int upload_any(FsCtx *c, LIBSSH2_SFTP *sftp, const char *local,
                      const char *remote)
{
    if (fs_cancelled(c)) return FS_CANCELLED;

    struct stat st;
    if (lstat(local, &st) != 0) return fail(c, local, errno);

    gchar *name = g_path_get_basename(local);
    report(c, name, 0);
    int r = FS_OK;
    gboolean replace = FALSE;

    gboolean rdir = FALSE;
    gboolean need_mkdir = S_ISDIR(st.st_mode);
    if (fs_sftp_exists(sftp, remote, &rdir)) {
        if (S_ISDIR(st.st_mode) && rdir) {
            need_mkdir = FALSE;                       /* merge */
        } else {
            r = ask_conflict(c, local, remote);
            if (r != FS_OK) goto out;
            /* file over file: replaced only after a complete upload */
            if (S_ISDIR(st.st_mode) || rdir) {
                r = sftp_delete_r(c, sftp, remote, FALSE);
                if (r != FS_OK) goto out;
            } else {
                replace = TRUE;
            }
        }
    }
    if (need_mkdir &&
        libssh2_sftp_mkdir(sftp, remote, (long)(st.st_mode & 0777) | 0700) != 0) {
        r = sftp_fail(c, sftp, remote);
        goto out;
    }

    if (S_ISLNK(st.st_mode)) {
        r = upload_link(c, sftp, local, remote, replace);
    } else if (S_ISDIR(st.st_mode)) {
        DIR *dir = opendir(local);
        if (!dir) { r = fail(c, local, errno); goto out; }
        int err;
        struct dirent *de;
        while ((de = read_dir(dir, &err)) != NULL) {
            if (is_dot(de->d_name)) continue;
            gchar *lp = g_build_filename(local, de->d_name, NULL);
            gchar *rp = fs_remote_join(remote, de->d_name);
            r = fs_worst(r, upload_any(c, sftp, lp, rp));
            g_free(lp); g_free(rp);
            if (r == FS_CANCELLED) break;
        }
        if (!de && err) r = fs_worst(r, fail(c, local, err));
        closedir(dir);
    } else if (S_ISREG(st.st_mode)) {
        gchar *tmp = replace ? tmp_sibling(remote) : NULL;
        r = upload_file(c, sftp, local, remote, tmp, &st, name);
        g_free(tmp);
    } else {
        r = fail_msg(c, "%s: special files are not copied", local);
    }

out:
    g_free(name);
    return r;
}

int fs_sftp_upload(FsCtx *ctx, LIBSSH2_SFTP *sftp, const char *local,
                   const char *remote)
{
    return upload_any(ctx, sftp, local, remote);
}

/* ── download ─────────────────────────────────────────────────────── */

/* tmp != NULL: write there and rename over local on success */
static int download_file(FsCtx *c, LIBSSH2_SFTP *sftp, const char *remote,
                         const char *local, const char *tmp,
                         const LIBSSH2_SFTP_ATTRIBUTES *a, const char *name)
{
    const char *out = tmp ? tmp : local;
    start_file(c, remote, local,
               (a->flags & LIBSSH2_SFTP_ATTR_SIZE) ? (gint64)a->filesize : 0);
    LIBSSH2_SFTP_HANDLE *fin = libssh2_sftp_open(sftp, remote, LIBSSH2_FXF_READ, 0);
    if (!fin) return sftp_fail(c, sftp, remote);

    int fout = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fout < 0) { int e = errno; libssh2_sftp_close(fin); return fail(c, local, e); }

    char *buf = g_malloc(COPY_BUF);
    int r = FS_OK;
    ssize_t n;
    while ((n = libssh2_sftp_read(fin, buf, COPY_BUF)) != 0) {
        if (n < 0) { r = sftp_fail(c, sftp, remote); break; }
        if (fs_cancelled(c)) { r = FS_CANCELLED; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = write(fout, buf + w, (size_t)(n - w));
            if (k < 0) {
                if (errno == EINTR) continue;
                r = fail(c, local, errno);
                break;
            }
            w += k;
        }
        if (r != FS_OK) break;
        report(c, name, n);
    }
    g_free(buf);
    libssh2_sftp_close(fin);

    if (r == FS_OK) {
        mode_t mode = (a->flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                      ? (mode_t)(a->permissions & 0777) : 0644;
        fchmod(fout, mode);
        if (a->flags & LIBSSH2_SFTP_ATTR_ACMODTIME) {
            struct timespec ts[2] = { { (time_t)a->atime, 0 },
                                      { (time_t)a->mtime, 0 } };
            futimens(fout, ts);
        }
        if (close(fout) != 0) r = fail(c, local, errno);
    } else {
        close(fout);
    }
    if (r == FS_OK && tmp && rename(tmp, local) != 0) r = fail(c, local, errno);
    if (r != FS_OK) unlink(out);
    else            count_file(c);
    return r;
}

static int download_any(FsCtx *c, LIBSSH2_SFTP *sftp, const char *remote,
                        const char *local)
{
    if (fs_cancelled(c)) return FS_CANCELLED;

    LIBSSH2_SFTP_ATTRIBUTES a;
    if (libssh2_sftp_lstat(sftp, remote, &a) != 0) return sftp_fail(c, sftp, remote);

    gchar *name = g_path_get_basename(remote);
    report(c, name, 0);
    int r = FS_OK;
    gboolean is_dir = attr_is_dir(&a);
    gboolean replace = FALSE;

    struct stat lt;
    gboolean need_mkdir = is_dir;
    if (lstat(local, &lt) == 0) {
        if (is_dir && S_ISDIR(lt.st_mode)) {
            need_mkdir = FALSE;                       /* merge */
        } else {
            r = ask_conflict(c, remote, local);
            if (r != FS_OK) goto out;
            /* file over file: replaced only after a complete download */
            if (is_dir || S_ISDIR(lt.st_mode)) {
                r = delete_r(c, local, FALSE);
                if (r != FS_OK) goto out;
            } else {
                replace = TRUE;
            }
        }
    }
    if (need_mkdir && mkdir(local, 0700) != 0) { r = fail(c, local, errno); goto out; }

    if (attr_is_link(&a)) {
        char target[PATH_MAX];
        int n = libssh2_sftp_readlink(sftp, remote, target, sizeof(target) - 1);
        if (n < 0) r = sftp_fail(c, sftp, remote);
        else {
            target[n] = '\0';
            r = make_symlink(c, target, local, replace);
        }
    } else if (is_dir) {
        LIBSSH2_SFTP_HANDLE *h = libssh2_sftp_opendir(sftp, remote);
        if (!h) { r = sftp_fail(c, sftp, remote); goto out; }
        char nm[NAME_BUF], longentry[NAME_BUF];
        LIBSSH2_SFTP_ATTRIBUTES attrs;
        int rc;
        while ((rc = libssh2_sftp_readdir_ex(h, nm, sizeof(nm), longentry,
                                             sizeof(longentry), &attrs)) > 0) {
            if (is_dot(nm)) continue;
            if (!fs_name_is_safe(nm)) {                  /* hostile server */
                r = fs_worst(r, FS_SKIPPED);
                continue;
            }
            gchar *rp = fs_remote_join(remote, nm);
            gchar *lp = g_build_filename(local, nm, NULL);
            r = fs_worst(r, download_any(c, sftp, rp, lp));
            g_free(rp); g_free(lp);
            if (r == FS_CANCELLED) break;
        }
        /* a read error must not look like the end of the directory –
         * a move would then delete what was not copied */
        if (rc < 0 && r != FS_CANCELLED) r = fs_worst(r, sftp_fail(c, sftp, remote));
        libssh2_sftp_closedir(h);
        if (a.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
            chmod(local, (a.permissions & 0777) | 0700);
    } else {
        gchar *tmp = replace ? tmp_sibling(local) : NULL;
        r = download_file(c, sftp, remote, local, tmp, &a, name);
        g_free(tmp);
    }

out:
    g_free(name);
    return r;
}

int fs_sftp_download(FsCtx *ctx, LIBSSH2_SFTP *sftp, const char *remote,
                     const char *local)
{
    return download_any(ctx, sftp, remote, local);
}

#endif /* HAVE_LIBSSH2 */
