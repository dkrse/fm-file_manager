#ifndef FM_UTIL_H
#define FM_UTIL_H

/* Pure GLib helpers – no GTK dependency, unit-tested in tests/ */

#include <glib.h>

/* ── formatting ───────────────────────────────────────────────────── */
gchar *fmt_size(goffset sz);                 /* "1.2 MB" – caller frees */
gchar *fmt_date(gint64 t);                   /* "2026-01-31 12:00"      */

/* ── path / name safety ───────────────────────────────────────────── */

/* Single path component that cannot escape its directory */
gboolean fs_name_is_safe(const char *name);

/* Relative path made of safe components ("a/b/c", no "..", not absolute) */
gboolean fs_relpath_is_safe(const char *path);

/* TRUE when `path` equals `dir` or lies below it (string-based, both absolute) */
gboolean fs_path_is_inside(const char *path, const char *dir);

/* Join for remote (POSIX) paths: avoids "//name" when dir is "/" */
gchar *fs_remote_join(const char *dir, const char *name);

/* "name (copy).ext", "name (copy 2).ext" … – first one for which
 * exists(candidate, user) returns FALSE. Returns basename only. */
typedef gboolean (*FsExistsFunc)(const char *dir, const char *name, gpointer user);
gchar *fs_unique_name(const char *dir, const char *name,
                      FsExistsFunc exists, gpointer user);
gboolean fs_local_exists(const char *dir, const char *name, gpointer user);

/* ── archives ─────────────────────────────────────────────────────── */
typedef enum {
    ARC_UNKNOWN,
    ARC_TAR_GZ,
    ARC_TAR_BZ2,
    ARC_TAR_XZ,
    ARC_TAR_ZST,
    ARC_TAR,
    ARC_ZIP,
    ARC_7Z,
    ARC_RAR,
} ArcType;

ArcType     arc_detect(const char *name);
const char *arc_extension(ArcType t);         /* ".tar.gz" etc. */
/* Replace a known archive extension (or append) – caller frees */
gchar      *arc_with_extension(const char *name, ArcType t);

/* ── clipboard ────────────────────────────────────────────────────── */

/* Parse "x-special/gnome-copied-files" ("copy|cut\nuri…") or
 * "text/uri-list" data into a list of local paths (g_free each).
 * *is_cut is set when the gnome format says "cut". */
GList *fs_parse_uri_list(const char *data, gsize len, gboolean *is_cut);

/* ── selection patterns ───────────────────────────────────────────── */

/* "*.c;*.h" – case-insensitive glob match against any sub-pattern */
gboolean fs_match_patterns(const char *patterns, const char *name);

#endif /* FM_UTIL_H */
