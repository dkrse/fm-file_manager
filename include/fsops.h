#ifndef FM_FSOPS_H
#define FM_FSOPS_H

/* ─────────────────────────────────────────────────────────────────── *
 *  File operation engine – GLib only, safe to run in a worker thread.
 *
 *  All functions take full paths.  Progress, cancellation and
 *  overwrite questions go through FsCtx callbacks so the same code
 *  runs under the GTK progress dialog (job.c) and in unit tests.
 * ─────────────────────────────────────────────────────────────────── */

#include <glib.h>
#ifdef HAVE_LIBSSH2
#  include <libssh2.h>
#  include <libssh2_sftp.h>
#endif

/* Result codes – ordered by severity (see fs_worst) */
enum {
    FS_OK        = 0,
    FS_SKIPPED   = 1,   /* user skipped (something) – not an error   */
    FS_CANCELLED = 2,
    FS_ERR       = 3,
};

typedef enum {
    FS_ASK = 0,          /* call ctx->conflict */
    FS_OVERWRITE_ALL,
    FS_SKIP_ALL,
} FsPolicy;

typedef enum {
    FS_ANS_OVERWRITE,
    FS_ANS_OVERWRITE_ALL,
    FS_ANS_SKIP,
    FS_ANS_SKIP_ALL,
    FS_ANS_CANCEL,
} FsAnswer;

/* ── one-shot filter (.gitignore syntax, see fsfilter.c) ──────────── */
typedef struct FsFilter FsFilter;
/* NULL when `text` holds no rules (only blanks/comments) */
FsFilter *fs_filter_new(const char *text);
void      fs_filter_free(FsFilter *f);
guint     fs_filter_rule_count(const FsFilter *f);
/* TRUE: the opposite – process only what the rules match */
void      fs_filter_set_invert(FsFilter *f, gboolean invert);
gboolean  fs_filter_inverted(const FsFilter *f);
/* the directory `rel` or one of its parents is matched by the rules */
gboolean  fs_filter_covers(const FsFilter *f, const char *rel);
/* rel: path relative to the operation root ("dir/sub/file.o") */
gboolean  fs_filter_excluded(const FsFilter *f, const char *rel, gboolean is_dir);

typedef struct FsCtx FsCtx;
struct FsCtx {
    gint      cancelled;     /* atomic – set from any thread         */
    FsPolicy  policy;
    /* name = item being processed, bytes = newly transferred bytes   */
    void     (*progress)(FsCtx *ctx, const char *name, gint64 bytes);
    FsAnswer (*conflict)(FsCtx *ctx, const char *src, const char *dst);
    /* a single file starts (dst NULL for delete); size in bytes or 0 */
    void     (*file_start)(FsCtx *ctx, const char *src, const char *dst, gint64 size);
    gpointer  user;
    gint      files_done;    /* atomic – files copied/deleted         */
    gchar    *error;         /* first error message (owned)           */

    /* Optional filter for this one operation (not owned).  Excluded
     * items are left alone – not copied, moved or deleted – and a
     * directory that keeps excluded items is not removed.  Paths are
     * matched relative to the directory holding the top-level item. */
    const FsFilter *filter;
    gsize     filter_root;   /* set by each top-level fs_* call       */
    gint      filtered;      /* atomic – items left out by the filter */
};

void     fs_ctx_clear(FsCtx *ctx);            /* frees ctx->error  */
gboolean fs_cancelled(FsCtx *ctx);
void     fs_cancel(FsCtx *ctx);
int      fs_worst(int a, int b);

/* Filter helpers for walks outside fsops.c (remote → remote copy):
 * begin = `path` is a top-level item; skip = excluded (and counted). */
void     fs_filter_begin(FsCtx *ctx, const char *path);
gboolean fs_filter_skip (FsCtx *ctx, const char *path, gboolean is_dir);
/* after walking the directory `src` (filtered count was `before`):
 * should its copy at the target be removed if it stayed empty? */
gboolean fs_filter_prune(FsCtx *ctx, const char *src, gint before);

/* ── local ────────────────────────────────────────────────────────── */
gint64 fs_calc_size(FsCtx *ctx, const char *path);
int    fs_copy  (FsCtx *ctx, const char *src, const char *dst);
int    fs_move  (FsCtx *ctx, const char *src, const char *dst);
int    fs_delete(FsCtx *ctx, const char *path);
int    fs_trash (FsCtx *ctx, const char *path);

/* Hex digest of a local file; progress through ctx->progress.  NULL on
 * error (ctx->error set) or when cancelled.  Caller frees. */
gchar *fs_checksum_file(FsCtx *ctx, const char *path, GChecksumType type);

/* ── SFTP (caller must hold the connection lock) ──────────────────── */
#ifdef HAVE_LIBSSH2
gint64 fs_sftp_calc_size(FsCtx *ctx, LIBSSH2_SFTP *sftp, const char *path);
int    fs_sftp_upload   (FsCtx *ctx, LIBSSH2_SFTP *sftp,
                         const char *local, const char *remote);
int    fs_sftp_download (FsCtx *ctx, LIBSSH2_SFTP *sftp,
                         const char *remote, const char *local);
int    fs_sftp_delete   (FsCtx *ctx, LIBSSH2_SFTP *sftp, const char *path);
gboolean fs_sftp_exists (LIBSSH2_SFTP *sftp, const char *path, gboolean *is_dir);
/* rename over an existing file (atomic if the server allows it) */
gboolean fs_sftp_replace(LIBSSH2_SFTP *sftp, const char *from, const char *to);
/* exact (case-sensitive) name lookup in a remote directory listing */
gboolean fs_sftp_has_entry(LIBSSH2_SFTP *sftp, const char *dir, const char *name);
#endif

#endif /* FM_FSOPS_H */
