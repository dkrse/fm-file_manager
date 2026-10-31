#include "util.h"
#include <string.h>
#include <time.h>
#include <fnmatch.h>
#include <sys/stat.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Formatting
 * ─────────────────────────────────────────────────────────────────── */

gchar *fmt_size(goffset sz)
{
    if (sz < 0)            return g_strdup("");
    if (sz < 1024)         return g_strdup_printf("%lld B",  (long long)sz);
    if (sz < 1024*1024LL)  return g_strdup_printf("%.1f KB", sz / 1024.0);
    if (sz < 1024*1024*1024LL)
                           return g_strdup_printf("%.1f MB", sz / (1024.0*1024));
    return                        g_strdup_printf("%.1f GB", sz / (1024.0*1024*1024));
}

gchar *fmt_date(gint64 t)
{
    time_t tt = (time_t)t;
    struct tm tm_buf;
    char buf[32] = "";
    if (localtime_r(&tt, &tm_buf))
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm_buf);
    return g_strdup(buf);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Path / name safety
 * ─────────────────────────────────────────────────────────────────── */

gboolean fs_name_is_safe(const char *name)
{
    if (!name || !name[0]) return FALSE;
    if (strcmp(name, ".") == 0) return FALSE;
    if (strcmp(name, "..") == 0) return FALSE;
    if (strchr(name, '/') != NULL) return FALSE;
    return TRUE;
}

gboolean fs_relpath_is_safe(const char *path)
{
    if (!path || !path[0] || path[0] == '/') return FALSE;
    gchar **parts = g_strsplit(path, "/", -1);
    gboolean ok = TRUE;
    int n = 0;
    for (int i = 0; parts[i]; i++) {
        if (!parts[i][0]) continue;            /* tolerate "a//b" and trailing "/" */
        if (!fs_name_is_safe(parts[i])) { ok = FALSE; break; }
        n++;
    }
    g_strfreev(parts);
    return ok && n > 0;
}

gboolean fs_path_is_inside(const char *path, const char *dir)
{
    if (!path || !dir) return FALSE;
    gsize dl = strlen(dir);
    while (dl > 1 && dir[dl - 1] == '/') dl--;          /* strip trailing / */
    if (strncmp(path, dir, dl) != 0) return FALSE;
    if (dl == 1 && dir[0] == '/') return path[0] == '/';
    return path[dl] == '\0' || path[dl] == '/';
}

gchar *fs_remote_join(const char *dir, const char *name)
{
    if (!dir || !dir[0]) return g_strdup(name);
    if (g_str_has_suffix(dir, "/")) return g_strconcat(dir, name, NULL);
    return g_strconcat(dir, "/", name, NULL);
}

gboolean fs_local_exists(const char *dir, const char *name, gpointer user)
{
    (void)user;
    gchar *p = g_build_filename(dir, name, NULL);
    struct stat st;
    gboolean r = lstat(p, &st) == 0;
    g_free(p);
    return r;
}

gchar *fs_unique_name(const char *dir, const char *name,
                      FsExistsFunc exists, gpointer user)
{
    if (!exists(dir, name, user)) return g_strdup(name);

    /* Split extension (but keep dot-files and archives readable) */
    const char *ext = NULL;
    ArcType at = arc_detect(name);
    if (at != ARC_UNKNOWN && g_str_has_suffix(name, arc_extension(at)))
        ext = name + strlen(name) - strlen(arc_extension(at));
    else {
        const char *dot = strrchr(name, '.');
        if (dot && dot != name) ext = dot;
    }
    gchar *stem = ext ? g_strndup(name, (gsize)(ext - name)) : g_strdup(name);
    if (!ext) ext = "";

    gchar *cand = NULL;
    for (int i = 1; i < 10000; i++) {
        g_free(cand);
        cand = (i == 1) ? g_strdup_printf("%s (copy)%s", stem, ext)
                        : g_strdup_printf("%s (copy %d)%s", stem, i, ext);
        if (!exists(dir, cand, user)) break;
    }
    g_free(stem);
    return cand;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Archives
 * ─────────────────────────────────────────────────────────────────── */

ArcType arc_detect(const char *name)
{
    if (!name) return ARC_UNKNOWN;
    const char *d = strrchr(name, '.');
    if (!d) return ARC_UNKNOWN;

    gchar *low = g_ascii_strdown(name, -1);
    ArcType t = ARC_UNKNOWN;
    /* double extensions first */
    if      (g_str_has_suffix(low, ".tar.gz"))  t = ARC_TAR_GZ;
    else if (g_str_has_suffix(low, ".tar.bz2")) t = ARC_TAR_BZ2;
    else if (g_str_has_suffix(low, ".tar.xz"))  t = ARC_TAR_XZ;
    else if (g_str_has_suffix(low, ".tar.zst")) t = ARC_TAR_ZST;
    else {
        const char *ext = strrchr(low, '.') + 1;
        if      (!strcmp(ext, "tgz"))  t = ARC_TAR_GZ;
        else if (!strcmp(ext, "tbz2")) t = ARC_TAR_BZ2;
        else if (!strcmp(ext, "txz"))  t = ARC_TAR_XZ;
        else if (!strcmp(ext, "tar"))  t = ARC_TAR;
        else if (!strcmp(ext, "zip"))  t = ARC_ZIP;
        else if (!strcmp(ext, "7z"))   t = ARC_7Z;
        else if (!strcmp(ext, "rar"))  t = ARC_RAR;
        else if (!strcmp(ext, "gz"))   t = ARC_TAR_GZ;   /* might be plain gz */
        else if (!strcmp(ext, "bz2"))  t = ARC_TAR_BZ2;
        else if (!strcmp(ext, "xz"))   t = ARC_TAR_XZ;
        else if (!strcmp(ext, "zst"))  t = ARC_TAR_ZST;
    }
    g_free(low);
    return t;
}

const char *arc_extension(ArcType t)
{
    switch (t) {
    case ARC_TAR_GZ:  return ".tar.gz";
    case ARC_TAR_BZ2: return ".tar.bz2";
    case ARC_TAR_XZ:  return ".tar.xz";
    case ARC_TAR_ZST: return ".tar.zst";
    case ARC_TAR:     return ".tar";
    case ARC_ZIP:     return ".zip";
    case ARC_7Z:      return ".7z";
    case ARC_RAR:     return ".rar";
    default:          return "";
    }
}

gchar *arc_with_extension(const char *name, ArcType t)
{
    gchar *stem = g_strdup(name ? name : "");
    gchar *low  = g_ascii_strdown(stem, -1);
    static const char *exts[] = { ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst",
                                  ".tgz", ".tar", ".zip", ".7z", ".rar", NULL };
    for (int i = 0; exts[i]; i++) {
        if (g_str_has_suffix(low, exts[i])) {
            stem[strlen(stem) - strlen(exts[i])] = '\0';
            break;
        }
    }
    g_free(low);
    gchar *r = g_strconcat(stem, arc_extension(t), NULL);
    g_free(stem);
    return r;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Clipboard
 * ─────────────────────────────────────────────────────────────────── */

GList *fs_parse_uri_list(const char *data, gsize len, gboolean *is_cut)
{
    if (is_cut) *is_cut = FALSE;
    if (!data) return NULL;
    gchar *txt = g_strndup(data, len);
    gchar **lines = g_strsplit_set(txt, "\r\n", -1);
    g_free(txt);

    GList *out = NULL;
    gboolean first = TRUE;
    for (int i = 0; lines[i]; i++) {
        gchar *l = g_strstrip(lines[i]);
        if (!l[0] || l[0] == '#') continue;
        if (first) {
            first = FALSE;
            if (strcmp(l, "cut") == 0)  { if (is_cut) *is_cut = TRUE; continue; }
            if (strcmp(l, "copy") == 0) continue;
        }
        gchar *path = NULL;
        if (g_str_has_prefix(l, "file://"))
            path = g_filename_from_uri(l, NULL, NULL);
        else if (l[0] == '/')
            path = g_strdup(l);
        if (path) out = g_list_prepend(out, path);
    }
    g_strfreev(lines);
    return g_list_reverse(out);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Selection patterns
 * ─────────────────────────────────────────────────────────────────── */

gboolean fs_match_patterns(const char *patterns, const char *name)
{
    if (!patterns || !name) return FALSE;
    gchar **pats = g_strsplit(patterns, ";", -1);
    gboolean hit = FALSE;
    for (int i = 0; pats[i] && !hit; i++) {
        gchar *p = g_strstrip(pats[i]);
        if (!p[0]) continue;
        /* "*.*" traditionally means "everything", also files without a dot */
        if (strcmp(p, "*.*") == 0) { hit = TRUE; break; }
        hit = fnmatch(p, name, FNM_CASEFOLD) == 0;
    }
    g_strfreev(pats);
    return hit;
}

/* ── multi-rename ─────────────────────────────────────────────────── */

gchar *mrn_apply(const char *name, gboolean is_dir, const char *mask,
                 const char *find, const char *repl, MrnCase cs,
                 guint counter, guint digits)
{
    const char *dot = is_dir ? NULL : strrchr(name, '.');
    if (dot == name) dot = NULL;                  /* ".bashrc" has no extension */
    gchar *stem = dot ? g_strndup(name, dot - name) : g_strdup(name);
    const char *ext = dot ? dot + 1 : "";

    GString *out = g_string_new(NULL);
    for (const char *m = mask; *m; ) {
        if (g_str_has_prefix(m, "[N]")) { g_string_append(out, stem); m += 3; }
        else if (g_str_has_prefix(m, "[E]")) {
            if (!ext[0] && out->len && out->str[out->len - 1] == '.')
                g_string_truncate(out, out->len - 1);
            g_string_append(out, ext);
            m += 3;
        }
        else if (g_str_has_prefix(m, "[C]")) {
            g_string_append_printf(out, "%0*u", (int)MIN(digits, 10), counter);
            m += 3;
        }
        else g_string_append_c(out, *m++);
    }
    g_free(stem);

    gchar *r = g_string_free(out, FALSE);
    if (find && find[0]) {
        gchar **parts = g_strsplit(r, find, -1);
        g_free(r);
        r = g_strjoinv(repl ? repl : "", parts);
        g_strfreev(parts);
    }
    if (cs != MRN_CASE_KEEP) {
        gchar *c = cs == MRN_CASE_LOWER ? g_utf8_strdown(r, -1) : g_utf8_strup(r, -1);
        g_free(r);
        r = c;
    }
    return r;
}
