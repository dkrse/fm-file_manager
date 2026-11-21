#include "util.h"
#include <string.h>
#include <time.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

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

/* ── Content search ───────────────────────────────────────────────── */

/* Case-insensitive search of an ASCII needle in hay[0..len) – no allocation */
static gboolean ascii_contains(const char *hay, size_t len,
                               const char *needle_lower, size_t nlen)
{
    if (nlen == 0) return TRUE;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (g_ascii_tolower(hay[i]) != needle_lower[0]) continue;
        size_t k = 1;
        while (k < nlen && g_ascii_tolower(hay[i + k]) == needle_lower[k]) k++;
        if (k == nlen) return TRUE;
    }
    return FALSE;
}

static gboolean str_is_ascii(const char *s)
{
    for (; *s; s++) if ((unsigned char)*s >= 0x80) return FALSE;
    return TRUE;
}

#define FC_CHUNK 4096
#define FC_SNIPPET_CHARS 200

gchar *fs_file_contains(const char *path, const char *needle_lower,
                        int *out_line, gint *cancelled)
{
    if (!needle_lower) return NULL;
    /* O_NONBLOCK: never hang on a FIFO/device that slipped through */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return NULL; }

    unsigned char probe[512];
    size_t nr = fread(probe, 1, sizeof(probe), fp);
    if (memchr(probe, 0, nr)) { fclose(fp); return NULL; }   /* binary */
    rewind(fp);

    /* A line longer than one chunk is read in pieces; the last nlen-1
     * bytes of the previous piece are kept in front of the next one so
     * a match spanning the boundary is still found. */
    size_t nlen = strlen(needle_lower);
    size_t keep_max = nlen > 0 ? MIN(nlen - 1, (size_t)FC_CHUNK) : 0;
    char  *buf = g_malloc(keep_max + FC_CHUNK + 1);
    gboolean ascii = str_is_ascii(needle_lower);
    gchar *result = NULL;
    size_t keep = 0;
    int line = 0;
    gboolean cont = FALSE;      /* previous piece stopped mid-line */

    while (fgets(buf + keep, FC_CHUNK + 1, fp)) {
        /* every piece, not every 1024th line: one huge line with no
         * newline would otherwise never see the cancel */
        if (cancelled && g_atomic_int_get(cancelled)) break;
        size_t len = keep + strlen(buf + keep);
        gboolean whole = len > 0 && buf[len - 1] == '\n';
        if (!cont) line++;

        gboolean hit;
        if (ascii) {
            hit = ascii_contains(buf, len, needle_lower, nlen);
        } else {
            gchar *valid = g_utf8_make_valid(buf, (gssize)len);
            gchar *lower = g_utf8_strdown(valid, -1);
            hit = strstr(lower, needle_lower) != NULL;
            g_free(lower);
            g_free(valid);
        }
        if (hit) {
            /* validate first, then truncate – never walk invalid UTF-8 */
            gchar *snip = g_utf8_make_valid(buf, (gssize)len);
            g_strstrip(snip);
            if (g_utf8_strlen(snip, -1) > FC_SNIPPET_CHARS)
                *g_utf8_offset_to_pointer(snip, FC_SNIPPET_CHARS) = '\0';
            result = snip;
            if (out_line) *out_line = line;
            break;
        }
        cont = !whole;
        keep = cont ? MIN(keep_max, len) : 0;
        if (keep) memmove(buf, buf + len - keep, keep);
    }
    g_free(buf);
    fclose(fp);
    return result;
}

gint64 fs_parse_size(const char *text, int unit_idx)
{
    if (!text || !text[0]) return -1;
    static const gint64 mult[] = { 1, 1024, 1024 * 1024, (gint64)1024 * 1024 * 1024 };
    if (unit_idx < 0 || unit_idx >= (int)G_N_ELEMENTS(mult)) unit_idx = 0;
    char *end;
    double val = g_ascii_strtod(text, &end);
    if (end == text || val < 0 || val != val) return -1;
    double r = val * (double)mult[unit_idx];
    if (r >= (double)G_MAXINT64) return G_MAXINT64;
    return (gint64)r;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Checksums
 * ─────────────────────────────────────────────────────────────────── */

static gboolean is_hex(const char *s, gsize n)
{
    for (gsize i = 0; i < n; i++)
        if (!g_ascii_isxdigit(s[i])) return FALSE;
    return n > 0;
}

gboolean fs_parse_checksum_line(const char *line, gchar **hash, gchar **name)
{
    *hash = NULL; *name = NULL;
    if (!line) return FALSE;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#') return FALSE;
    gsize len = strlen(line);
    while (len && (line[len-1] == '\n' || line[len-1] == '\r')) len--;
    if (!len) return FALSE;
    gchar *l = g_strndup(line, len);
    gboolean ok = FALSE;

    /* BSD: "SHA256 (name) = hash" */
    const char *open = strstr(l, " (");
    const char *eq   = g_strrstr(l, ") = ");
    if (open && eq && eq > open && is_hex(eq + 4, strlen(eq + 4))) {
        *name = g_strndup(open + 2, eq - (open + 2));
        *hash = g_ascii_strdown(eq + 4, -1);
        ok = TRUE;
    } else {
        /* GNU: "hash  name" (text) or "hash *name" (binary) */
        gsize h = 0;
        while (l[h] && g_ascii_isxdigit(l[h])) h++;
        if (h > 0 && l[h] == ' ' && (l[h+1] == ' ' || l[h+1] == '*') && l[h+2]) {
            *hash = g_ascii_strdown(l, h);
            *name = g_strdup(l + h + 2);
            ok = TRUE;
        }
    }
    if (ok) {
        GChecksumType t;
        if (!fs_checksum_type_for_hex(strlen(*hash), &t) || !**name) ok = FALSE;
    }
    if (!ok) { g_clear_pointer(hash, g_free); g_clear_pointer(name, g_free); }
    g_free(l);
    return ok;
}

gboolean fs_checksum_type_for_hex(gsize len, GChecksumType *type)
{
    switch (len) {
    case 32:  *type = G_CHECKSUM_MD5;    return TRUE;
    case 40:  *type = G_CHECKSUM_SHA1;   return TRUE;
    case 64:  *type = G_CHECKSUM_SHA256; return TRUE;
    case 128: *type = G_CHECKSUM_SHA512; return TRUE;
    default:  return FALSE;
    }
}

const char *fs_checksum_name(GChecksumType type)
{
    switch (type) {
    case G_CHECKSUM_MD5:    return "MD5";
    case G_CHECKSUM_SHA1:   return "SHA1";
    case G_CHECKSUM_SHA512: return "SHA512";
    case G_CHECKSUM_SHA384: return "SHA384";
    default:                return "SHA256";
    }
}

const char *fs_checksum_ext(GChecksumType type)
{
    switch (type) {
    case G_CHECKSUM_MD5:    return ".md5";
    case G_CHECKSUM_SHA1:   return ".sha1";
    case G_CHECKSUM_SHA512: return ".sha512";
    case G_CHECKSUM_SHA384: return ".sha384";
    default:                return ".sha256";
    }
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Directory compare
 * ─────────────────────────────────────────────────────────────────── */

FsCmp fs_compare_files(gboolean other_exists, gint64 size, gint64 mtime,
                       gint64 other_size, gint64 other_mtime, gint64 tolerance)
{
    if (!other_exists) return FS_CMP_ONLY;
    gint64 dt = mtime - other_mtime;
    if (dt >  tolerance) return FS_CMP_NEWER;
    if (dt < -tolerance) return FS_CMP_OLDER;
    return size == other_size ? FS_CMP_SAME : FS_CMP_DIFFERENT;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Network locations
 * ─────────────────────────────────────────────────────────────────── */

gchar *fs_net_normalize_uri(const char *text)
{
    if (!text) return NULL;
    gchar *t = g_strstrip(g_strdup(text));
    if (!t[0]) { g_free(t); return NULL; }
    gchar *scheme = g_uri_parse_scheme(t);
    if (!scheme) {
        gchar *u;
        if (g_str_has_prefix(t, "//") || g_str_has_prefix(t, "\\\\")) {
            g_strdelimit(t, "\\", '/');
            u = g_strconcat("smb:", t, NULL);
        } else {
            u = g_strconcat("smb://", t, NULL);
        }
        g_free(t);
        return u;
    }
    gboolean bad = g_ascii_strcasecmp(scheme, "file") == 0;
    g_free(scheme);
    if (bad) { g_free(t); return NULL; }
    return t;
}
