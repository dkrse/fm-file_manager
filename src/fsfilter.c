/* ─────────────────────────────────────────────────────────────────── *
 *  One-shot operation filter with .gitignore semantics.
 *
 *  Rules (same as git, see gitignore(5)):
 *    - one pattern per line; blank lines and "#…" are ignored
 *      ("\#" / "\!" start a literal # / !); trailing spaces are
 *      dropped unless escaped with "\"
 *    - "!pat"   re-includes what an earlier pattern excluded
 *    - "pat/"   matches directories only
 *    - a "/" at the start or in the middle anchors the pattern to the
 *      operation root (the directory the selected items live in);
 *      otherwise it matches the name at any depth
 *    - "*" and "?" never match "/", "[a-z]" / "[!a-z]" are classes,
 *      "**" between slashes matches any number of directories
 *    - the last matching pattern decides; once a directory is
 *      excluded nothing inside it can be re-included (it is not
 *      entered at all)
 * ─────────────────────────────────────────────────────────────────── */

#include "fsops.h"
#include <string.h>

typedef struct {
    gchar   *pat;
    gboolean negate;
    gboolean dir_only;
    gboolean anchored;     /* match the whole relative path */
} Rule;

struct FsFilter {
    GArray  *rules;        /* of Rule */
    gboolean invert;       /* process only what the rules match      */
};

/* [...] at *pp (just after '[') against c.  Returns 1 match, 0 no
 * match, -1 not a valid class (then '[' is a literal). */
static int match_class(const char **pp, char c)
{
    const char *p = *pp;
    gboolean neg = FALSE, hit = FALSE;
    if (*p == '!' || *p == '^') { neg = TRUE; p++; }
    gboolean first = TRUE;
    while (*p && (*p != ']' || first)) {
        first = FALSE;
        char lo = *p;
        if (lo == '\\' && p[1]) lo = *++p;
        char hi = lo;
        if (p[1] == '-' && p[2] && p[2] != ']') {
            p += 2;
            hi = *p;
            if (hi == '\\' && p[1]) hi = *++p;
        }
        if ((unsigned char)c >= (unsigned char)lo &&
            (unsigned char)c <= (unsigned char)hi) hit = TRUE;
        p++;
    }
    if (*p != ']') return -1;
    *pp = p + 1;
    return (hit != neg) && c != '/';
}

static gboolean wildmatch(const char *start, const char *p, const char *t)
{
    for (;;) {
        switch (*p) {
        case '\0':
            return *t == '\0';
        case '?':
            if (!*t || *t == '/') return FALSE;
            p++; t++;
            break;
        case '*': {
            const char *q = p;
            while (*q == '*') q++;
            if (q - p >= 2 && (p == start || p[-1] == '/') &&
                (*q == '/' || *q == '\0')) {
                if (*q == '\0') return TRUE;          /* trailing double star */
                const char *rest = q + 1;             /* after the slash */
                for (const char *s = t;;) {           /* 0..n dirs */
                    if (wildmatch(start, rest, s)) return TRUE;
                    s = strchr(s, '/');
                    if (!s) return FALSE;
                    s++;
                }
            }
            for (const char *s = t;; s++) {           /* within one name */
                if (wildmatch(start, q, s)) return TRUE;
                if (!*s || *s == '/') return FALSE;
            }
        }
        case '[': {
            if (!*t) return FALSE;
            const char *q = p + 1;
            int m = match_class(&q, *t);
            if (m < 0) {                              /* literal '[' */
                if (*t != '[') return FALSE;
                p++; t++;
                break;
            }
            if (!m) return FALSE;
            p = q; t++;
            break;
        }
        case '\\':
            if (p[1]) p++;
            /* fall through */
        default:
            if (*p != *t) return FALSE;
            p++; t++;
            break;
        }
    }
}

static gboolean parse_line(const char *line, Rule *r)
{
    gchar *s = g_strdup(line);
    gsize n = strlen(s);
    if (n && s[n - 1] == '\r') s[--n] = '\0';
    /* trailing spaces, unless the last one is escaped */
    while (n && s[n - 1] == ' ' && !(n >= 2 && s[n - 2] == '\\')) s[--n] = '\0';

    const char *p = s;
    if (!*p || *p == '#') { g_free(s); return FALSE; }
    r->negate = FALSE;
    if (*p == '!') { r->negate = TRUE; p++; }
    else if (*p == '\\' && (p[1] == '!' || p[1] == '#')) p++;

    gchar *pat = g_strdup(p);
    g_free(s);
    n = strlen(pat);
    r->dir_only = FALSE;
    while (n && pat[n - 1] == '/') { pat[--n] = '\0'; r->dir_only = TRUE; }
    if (!n) { g_free(pat); return FALSE; }               /* "/" or "!" */

    r->anchored = strchr(pat, '/') != NULL;
    if (pat[0] == '/') memmove(pat, pat + 1, n);         /* incl. NUL */
    if (!pat[0]) { g_free(pat); return FALSE; }
    r->pat = pat;
    return TRUE;
}

FsFilter *fs_filter_new(const char *text)
{
    if (!text) return NULL;
    FsFilter *f = g_new0(FsFilter, 1);
    f->rules = g_array_new(FALSE, FALSE, sizeof(Rule));
    gchar **lines = g_strsplit(text, "\n", -1);
    for (gchar **l = lines; *l; l++) {
        Rule r;
        if (parse_line(*l, &r)) g_array_append_val(f->rules, r);
    }
    g_strfreev(lines);
    if (f->rules->len == 0) { fs_filter_free(f); return NULL; }
    return f;
}

void fs_filter_free(FsFilter *f)
{
    if (!f) return;
    for (guint i = 0; i < f->rules->len; i++)
        g_free(g_array_index(f->rules, Rule, i).pat);
    g_array_free(f->rules, TRUE);
    g_free(f);
}

void fs_filter_set_invert(FsFilter *f, gboolean invert)
{
    if (f) f->invert = invert;
}

gboolean fs_filter_inverted(const FsFilter *f)
{
    return f && f->invert;
}

guint fs_filter_rule_count(const FsFilter *f)
{
    return f ? f->rules->len : 0;
}

/* gitignore verdict for one path: TRUE = the rules match ("ignored") */
static gboolean rules_match(const FsFilter *f, const char *rel, gboolean is_dir)
{
    const char *slash = strrchr(rel, '/');
    const char *base = slash ? slash + 1 : rel;
    for (guint i = f->rules->len; i-- > 0; ) {          /* last match wins */
        const Rule *r = &g_array_index(f->rules, Rule, i);
        if (r->dir_only && !is_dir) continue;
        const char *subject = r->anchored ? rel : base;
        if (wildmatch(r->pat, r->pat, subject)) return !r->negate;
    }
    return FALSE;
}

gboolean fs_filter_covers(const FsFilter *f, const char *rel)
{
    if (!f || !rel) return FALSE;
    while (*rel == '/') rel++;
    if (!*rel) return FALSE;
    gchar *p = g_strdup(rel);
    gboolean in = rules_match(f, p, TRUE);
    for (char *s = strrchr(p, '/'); s && !in; s = strrchr(p, '/')) {
        *s = '\0';
        in = rules_match(f, p, TRUE);
    }
    g_free(p);
    return in;
}

/* Inverted ("only matching"): a matched directory comes with all its
 * content; any other directory is entered (never excluded itself – an
 * engine walk removes / does not create the ones left empty) and of the
 * files only the matched ones are processed. */
gboolean fs_filter_excluded(const FsFilter *f, const char *rel, gboolean is_dir)
{
    if (!f || !rel) return FALSE;
    while (*rel == '/') rel++;
    if (!*rel) return FALSE;
    if (!f->invert) return rules_match(f, rel, is_dir);

    if (is_dir) return FALSE;
    if (rules_match(f, rel, FALSE)) return FALSE;
    /* inside a matched directory? */
    gchar *anc = g_strdup(rel);
    gboolean in = FALSE;
    for (char *s = strrchr(anc, '/'); s && !in; s = strrchr(anc, '/')) {
        *s = '\0';
        in = rules_match(f, anc, TRUE);
    }
    g_free(anc);
    return !in;
}
