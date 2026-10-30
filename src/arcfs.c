#include "arcfs.h"
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef HAVE_LIBARCHIVE
#  include <archive.h>
#  include <archive_entry.h>
#endif

/* ─────────────────────────────────────────────────────────────────── *
 *  Archives as directories – see arcfs.h
 * ─────────────────────────────────────────────────────────────────── */

static const char *const arc_exts[] = {
    ".zip", ".jar", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz", ".tbz2",
    ".tar.xz", ".txz", ".tar.zst", ".tzst", ".tar.lz", ".tar.lzma",
    ".7z", ".iso", ".rar", ".cpio", NULL
};

gboolean arcfs_supported(const char *name)
{
#ifdef HAVE_LIBARCHIVE
    if (!name) return FALSE;
    gchar *low = g_ascii_strdown(name, -1);
    gboolean ok = FALSE;
    for (int i = 0; arc_exts[i] && !ok; i++)
        ok = g_str_has_suffix(low, arc_exts[i]);
    g_free(low);
    return ok;
#else
    (void)name; (void)arc_exts;
    return FALSE;
#endif
}

G_GNUC_UNUSED static void entry_free(gpointer p)
{
    ArcEntry *e = p;
    g_free(e->path);
    g_free(e->link);
    g_free(e);
}

G_GNUC_UNUSED static int entry_cmp(gconstpointer a, gconstpointer b)
{
    const ArcEntry *ea = *(const ArcEntry *const *)a;
    const ArcEntry *eb = *(const ArcEntry *const *)b;
    return strcmp(ea->path, eb->path);
}

/* "./a/b/" → "a/b"; NULL when the path is unsafe or empty */
G_GNUC_UNUSED static gchar *clean_path(const char *raw)
{
    if (!raw) return NULL;
    while (raw[0] == '.' && raw[1] == '/') raw += 2;
    gchar *p = g_strdup(raw);
    gsize n = strlen(p);
    while (n > 0 && p[n - 1] == '/') p[--n] = '\0';
    /* collapse "a//b" */
    gchar **parts = g_strsplit(p, "/", -1);
    GString *s = g_string_new(NULL);
    for (int i = 0; parts[i]; i++) {
        if (!parts[i][0] || strcmp(parts[i], ".") == 0) continue;
        if (s->len) g_string_append_c(s, '/');
        g_string_append(s, parts[i]);
    }
    g_strfreev(parts);
    g_free(p);
    if (!fs_relpath_is_safe(s->str)) { g_string_free(s, TRUE); return NULL; }
    return g_string_free(s, FALSE);
}

#ifdef HAVE_LIBARCHIVE

static struct archive *open_archive(const char *path, GError **error)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    if (archive_read_open_filename(a, path, 64 * 1024) != ARCHIVE_OK) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s",
                    path, archive_error_string(a) ? archive_error_string(a) : "cannot open");
        archive_read_free(a);
        return NULL;
    }
    return a;
}

GPtrArray *arcfs_list(const char *archive, GError **error)
{
    struct archive *a = open_archive(archive, error);
    if (!a) return NULL;

    GHashTable *by_path = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *out = g_ptr_array_new_with_free_func(entry_free);
    struct archive_entry *ae;
    int rc;

    while ((rc = archive_read_next_header(a, &ae)) == ARCHIVE_OK ||
           rc == ARCHIVE_WARN) {
        gchar *path = clean_path(archive_entry_pathname(ae));
        if (!path) continue;                       /* absolute / ".." – dropped */

        ArcEntry *e = g_hash_table_lookup(by_path, path);
        if (!e) {
            e = g_new0(ArcEntry, 1);
            e->path = path;
            g_ptr_array_add(out, e);
            g_hash_table_insert(by_path, e->path, e);
        } else {
            g_free(path);                          /* later entry wins */
            g_clear_pointer(&e->link, g_free);
        }
        mode_t ft = archive_entry_filetype(ae);
        e->is_dir = ft == AE_IFDIR;
        e->mode   = (guint)archive_entry_mode(ae);
        e->size   = e->is_dir ? 0 : archive_entry_size(ae);
        e->mtime  = archive_entry_mtime(ae);
        if (ft == AE_IFLNK && archive_entry_symlink(ae))
            e->link = g_strdup(archive_entry_symlink(ae));
    }
    if (rc != ARCHIVE_EOF) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "%s: %s", archive,
                    archive_error_string(a) ? archive_error_string(a) : "read error");
        archive_read_free(a);
        g_hash_table_destroy(by_path);
        g_ptr_array_unref(out);
        return NULL;
    }
    archive_read_free(a);

    /* directories implied by "a/b/file" without own entries */
    guint n = out->len;
    for (guint i = 0; i < n; i++) {
        gchar *p = g_strdup(((ArcEntry *)g_ptr_array_index(out, i))->path);
        gchar *slash;
        while ((slash = strrchr(p, '/'))) {
            *slash = '\0';
            ArcEntry *d = g_hash_table_lookup(by_path, p);
            if (d) {
                if (!d->is_dir) { d->is_dir = TRUE; d->size = 0; }
                continue;
            }
            d = g_new0(ArcEntry, 1);
            d->path   = g_strdup(p);
            d->is_dir = TRUE;
            d->mode   = S_IFDIR | 0755;
            g_ptr_array_add(out, d);
            g_hash_table_insert(by_path, d->path, d);
        }
        g_free(p);
    }
    g_hash_table_destroy(by_path);
    g_ptr_array_sort(out, entry_cmp);
    return out;
}

#else  /* !HAVE_LIBARCHIVE */

GPtrArray *arcfs_list(const char *archive, GError **error)
{
    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOSYS,
                "%s: built without libarchive", archive);
    return NULL;
}

#endif

static gboolean is_child_of(const char *path, const char *dir)
{
    if (!dir[0]) return strchr(path, '/') == NULL;
    gsize dl = strlen(dir);
    return strncmp(path, dir, dl) == 0 && path[dl] == '/' &&
           strchr(path + dl + 1, '/') == NULL;
}

GPtrArray *arcfs_children(GPtrArray *entries, const char *dir)
{
    GPtrArray *out = g_ptr_array_new();
    for (guint i = 0; entries && i < entries->len; i++) {
        ArcEntry *e = g_ptr_array_index(entries, i);
        if (is_child_of(e->path, dir ? dir : "")) g_ptr_array_add(out, e);
    }
    return out;
}

/* Is `path` one of the selected inner paths or below one of them? */
static gboolean selected(const char *path, const char *const *inner, guint n)
{
    for (guint i = 0; i < n; i++) {
        gsize l = strlen(inner[i]);
        if (strncmp(path, inner[i], l) == 0 && (path[l] == '\0' || path[l] == '/'))
            return TRUE;
    }
    return FALSE;
}

gint64 arcfs_size(GPtrArray *entries, const char *const *inner, guint n)
{
    gint64 total = 0;
    for (guint i = 0; entries && i < entries->len; i++) {
        ArcEntry *e = g_ptr_array_index(entries, i);
        if (!e->is_dir && selected(e->path, inner, n)) total += e->size;
    }
    return total;
}

#ifdef HAVE_LIBARCHIVE

/* ── extraction ───────────────────────────────────────────────────── */

typedef struct {
    FsCtx      *c;
    const char *archive;
    const char *dest;
    gsize       base_len;     /* strlen(base) + 1, or 0 for the root */
    GHashTable *done;         /* inner path → extracted local path (hard links) */
    GPtrArray  *dirs;         /* (local path, mtime) to stamp at the end */
} Ex;

typedef struct { gchar *path; gint64 mtime; guint mode; } DirStamp;

static void dirstamp_free(gpointer p)
{
    DirStamp *d = p;
    g_free(d->path);
    g_free(d);
}

static int ex_fail(FsCtx *c, const char *fmt, const char *a, const char *b)
{
    if (c && !c->error) c->error = g_strdup_printf(fmt, a, b);
    return FS_ERR;
}

static int ex_conflict(FsCtx *c, const char *src, const char *dst)
{
    if (c->policy == FS_OVERWRITE_ALL) return FS_OK;
    if (c->policy == FS_SKIP_ALL || !c->conflict) return FS_SKIPPED;
    switch (c->conflict(c, src, dst)) {
    case FS_ANS_OVERWRITE:     return FS_OK;
    case FS_ANS_OVERWRITE_ALL: c->policy = FS_OVERWRITE_ALL; return FS_OK;
    case FS_ANS_SKIP:          return FS_SKIPPED;
    case FS_ANS_SKIP_ALL:      c->policy = FS_SKIP_ALL;      return FS_SKIPPED;
    default:
        fs_cancel(c);
        return FS_CANCELLED;
    }
}

/* Never write through a symlink that the archive itself created: every
 * directory between dest and the file must be a real directory. */
static gboolean parents_are_dirs(const char *dest, const char *rel)
{
    gchar **parts = g_strsplit(rel, "/", -1);
    GString *p = g_string_new(dest);
    gboolean ok = TRUE;
    for (int i = 0; parts[i] && parts[i + 1]; i++) {
        g_string_append_c(p, '/');
        g_string_append(p, parts[i]);
        struct stat st;
        if (lstat(p->str, &st) != 0) {
            if (errno != ENOENT || mkdir(p->str, 0755) != 0) { ok = FALSE; break; }
        } else if (!S_ISDIR(st.st_mode)) {
            ok = FALSE;
            break;
        }
    }
    g_string_free(p, TRUE);
    g_strfreev(parts);
    return ok;
}

static gchar *tmp_name(const char *path)
{
    gchar *dir  = g_path_get_dirname(path);
    gchar *base = g_path_get_basename(path);
    gchar *t = g_strdup_printf("%s/.%s.fm-%06x", dir, base,
                               (unsigned)g_random_int_range(0, 0x1000000));
    g_free(dir); g_free(base);
    return t;
}

static int write_data(Ex *x, struct archive *a, int fd, const char *name)
{
    const void *buf;
    size_t size;
    la_int64_t off;
    int rc;
    while ((rc = archive_read_data_block(a, &buf, &size, &off)) == ARCHIVE_OK) {
        if (fs_cancelled(x->c)) return FS_CANCELLED;
        if (lseek(fd, off, SEEK_SET) < 0) return ex_fail(x->c, "%s: %s", name, g_strerror(errno));
        const char *p = buf;
        while (size > 0) {
            ssize_t w = write(fd, p, size);
            if (w < 0) {
                if (errno == EINTR) continue;
                return ex_fail(x->c, "%s: %s", name, g_strerror(errno));
            }
            p += w;
            size -= (size_t)w;
            if (x->c->progress) x->c->progress(x->c, name, w);
        }
    }
    if (rc != ARCHIVE_EOF)
        return ex_fail(x->c, "%s: %s", name,
                       archive_error_string(a) ? archive_error_string(a) : "read error");
    /* sparse tail: make the file as long as the archive says */
    return FS_OK;
}

static int extract_entry(Ex *x, struct archive *a, struct archive_entry *ae,
                         const char *rel_in)
{
    FsCtx *c = x->c;
    const char *rel = rel_in + x->base_len;
    if (!rel[0]) return FS_OK;                     /* the base itself */
    gchar *dst = g_build_filename(x->dest, rel, NULL);
    gchar *src = g_strdup_printf("%s/%s", x->archive, rel_in);
    mode_t ft = archive_entry_filetype(ae);
    int r = FS_OK;

    if (!parents_are_dirs(x->dest, rel)) {
        r = ex_fail(c, "%s: %s", dst, "parent is not a directory");
        goto out;
    }

    struct stat st;
    gboolean exists = lstat(dst, &st) == 0;

    if (ft == AE_IFDIR) {
        if (exists && !S_ISDIR(st.st_mode)) {
            r = ex_fail(c, "%s: %s", dst, "exists and is not a directory");
        } else if (!exists && mkdir(dst, 0755) != 0) {
            r = ex_fail(c, "%s: %s", dst, g_strerror(errno));
        } else {
            DirStamp *d = g_new(DirStamp, 1);
            d->path  = g_strdup(dst);
            d->mtime = archive_entry_mtime(ae);
            d->mode  = archive_entry_perm(ae) | 0700;   /* keep it writable for us */
            g_ptr_array_add(x->dirs, d);
        }
        goto out;
    }

    if (exists) {
        if (S_ISDIR(st.st_mode)) {
            r = ex_fail(c, "%s: %s", dst, "a directory with that name exists");
            goto out;
        }
        r = ex_conflict(c, src, dst);
        if (r != FS_OK) goto out;
    }

    gint64 size = archive_entry_size(ae);
    if (c->file_start) c->file_start(c, src, dst, ft == AE_IFREG ? size : 0);
    gchar *tmp = tmp_name(dst);

    const char *hard = archive_entry_hardlink(ae);
    if (hard) {
        gchar *hp = clean_path(hard);
        const char *local = hp ? g_hash_table_lookup(x->done, hp) : NULL;
        if (!local) {
            r = FS_SKIPPED;                        /* link target not extracted */
        } else if (link(local, tmp) != 0) {
            r = ex_fail(c, "%s: %s", dst, g_strerror(errno));
        }
        g_free(hp);
    } else if (ft == AE_IFLNK) {
        const char *target = archive_entry_symlink(ae);
        if (!target || symlink(target, tmp) != 0)
            r = ex_fail(c, "%s: %s", dst, target ? g_strerror(errno) : "bad symlink");
    } else if (ft == AE_IFREG) {
        int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) {
            r = ex_fail(c, "%s: %s", dst, g_strerror(errno));
        } else {
            r = write_data(x, a, fd, rel);
            if (r == FS_OK && ftruncate(fd, size) != 0)
                r = ex_fail(c, "%s: %s", dst, g_strerror(errno));
            if (r == FS_OK)
                fchmod(fd, archive_entry_perm(ae) & 0777);
            if (close(fd) != 0 && r == FS_OK)
                r = ex_fail(c, "%s: %s", dst, g_strerror(errno));
            if (r == FS_OK) {
                struct timeval tv[2] = {
                    { .tv_sec = archive_entry_mtime(ae) },
                    { .tv_sec = archive_entry_mtime(ae) },
                };
                utimes(tmp, tv);
            }
        }
    } else {
        r = FS_SKIPPED;                            /* devices, fifos, sockets */
    }

    if (r == FS_OK && rename(tmp, dst) != 0)
        r = ex_fail(c, "%s: %s", dst, g_strerror(errno));
    if (r != FS_OK) unlink(tmp);
    else {
        g_hash_table_insert(x->done, g_strdup(rel_in), g_strdup(dst));
        g_atomic_int_inc(&c->files_done);
    }
    g_free(tmp);

out:
    g_free(src);
    g_free(dst);
    return r;
}

int arcfs_extract(FsCtx *c, const char *archive, const char *base,
                  const char *const *inner, guint n, const char *dest)
{
    GError *err = NULL;
    struct archive *a = open_archive(archive, &err);
    if (!a) {
        if (!c->error) c->error = g_strdup(err->message);
        g_error_free(err);
        return FS_ERR;
    }
    if (g_mkdir_with_parents(dest, 0755) != 0) {
        archive_read_free(a);
        return ex_fail(c, "%s: %s", dest, g_strerror(errno));
    }

    Ex x = {
        .c        = c,
        .archive  = archive,
        .dest     = dest,
        .base_len = base && base[0] ? strlen(base) + 1 : 0,
        .done     = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free),
        .dirs     = g_ptr_array_new_with_free_func(dirstamp_free),
    };

    int r = FS_OK;
    struct archive_entry *ae;
    int rc;
    while ((rc = archive_read_next_header(a, &ae)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
        if (fs_cancelled(c)) { r = FS_CANCELLED; break; }
        gchar *path = clean_path(archive_entry_pathname(ae));
        if (path && selected(path, inner, n) &&
            (!x.base_len || strncmp(path, base, x.base_len - 1) == 0))
            r = fs_worst(r, extract_entry(&x, a, ae, path));
        g_free(path);
        if (r == FS_CANCELLED) break;
    }
    if (r != FS_CANCELLED && rc != ARCHIVE_EOF && rc != ARCHIVE_OK && rc != ARCHIVE_WARN)
        r = fs_worst(r, ex_fail(c, "%s: %s", archive,
                                archive_error_string(a) ? archive_error_string(a) : "read error"));
    archive_read_free(a);

    /* directory times last – writing files into them changed the mtime */
    for (guint i = 0; i < x.dirs->len; i++) {
        DirStamp *d = g_ptr_array_index(x.dirs, i);
        chmod(d->path, d->mode & 07777);
        struct timeval tv[2] = { { .tv_sec = d->mtime }, { .tv_sec = d->mtime } };
        utimes(d->path, tv);
    }
    g_ptr_array_unref(x.dirs);
    g_hash_table_destroy(x.done);
    return r;
}

#else  /* !HAVE_LIBARCHIVE */

int arcfs_extract(FsCtx *c, const char *archive, const char *base,
                  const char *const *inner, guint n, const char *dest)
{
    (void)base; (void)inner; (void)n; (void)dest;
    if (c && !c->error) c->error = g_strdup_printf("%s: built without libarchive", archive);
    return FS_ERR;
}

#endif
