#ifndef FM_ARCFS_H
#define FM_ARCFS_H

/* ─────────────────────────────────────────────────────────────────── *
 *  Archives as directories (read-only) – GLib + libarchive only, safe
 *  to run in a worker thread.
 *
 *  arcfs_list() reads the whole table of contents once; the panel then
 *  browses it like a directory tree.  arcfs_extract() copies selected
 *  entries out (F5 / F3 / Enter on a file).  Entries with absolute or
 *  ".." paths are dropped – they can never be extracted.
 * ─────────────────────────────────────────────────────────────────── */

#include <glib.h>
#include "fsops.h"

typedef struct {
    gchar   *path;      /* inside the archive: "dir/sub/file", no slashes at the ends */
    gchar   *link;      /* symlink target, or NULL                  */
    gint64   size;
    gint64   mtime;
    guint    mode;      /* permission bits + file type (S_IF*)      */
    gboolean is_dir;
} ArcEntry;

/* TRUE when the name looks like an archive arcfs can open */
gboolean   arcfs_supported(const char *name);

/* Table of contents (ArcEntry*, owned by the array), sorted by path,
 * including directories that are only implied by file paths.
 * NULL + error when the file cannot be read as an archive. */
GPtrArray *arcfs_list(const char *archive, GError **error);

/* Direct children of `dir` ("" = archive root) – borrowed pointers */
GPtrArray *arcfs_children(GPtrArray *entries, const char *dir);

/* Total size of the files below the given inner paths */
gint64     arcfs_size(GPtrArray *entries, const char *const *inner, guint n);

/* Extract the inner paths (files or whole directories) into `dest`.
 * `base` is the archive directory they are listed in ("" = root); it is
 * stripped, so "a/b/x.txt" with base "a/b" lands in dest/x.txt.
 * Existing files go through ctx->conflict like fs_copy().  Returns the
 * worst FS_* result. */
int        arcfs_extract(FsCtx *ctx, const char *archive, const char *base,
                         const char *const *inner, guint n, const char *dest);

#endif /* FM_ARCFS_H */
