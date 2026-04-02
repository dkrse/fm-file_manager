#include "fm.h"
#include <string.h>
#include <ctype.h>

#ifdef HAVE_GTKSOURCEVIEW
#include <gtksourceview/gtksource.h>

/* Helper: guess language with fallback for special filenames */
GtkSourceLanguage *source_guess_language(const char *filename)
{
    if (!filename) return NULL;

    GtkSourceLanguageManager *lm = gtk_source_language_manager_get_default();

    /* Extract basename */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    /* Try standard guess first */
    GtkSourceLanguage *lang = gtk_source_language_manager_guess_language(lm, base, NULL);
    if (lang) return lang;

    /* Fallback for special filenames without extension */
    static const struct { const char *name; const char *lang_id; } special[] = {
        { "Makefile",       "makefile" },
        { "makefile",       "makefile" },
        { "GNUmakefile",    "makefile" },
        { "CMakeLists.txt", "cmake" },
        { "Dockerfile",     "dockerfile" },
        { "dockerfile",     "dockerfile" },
        { "meson.build",    "meson" },
        { "meson_options.txt", "meson" },
        { "PKGBUILD",       "sh" },
        { "configure",      "sh" },
        { "configure.ac",   "autoconf" },
        { "configure.in",   "autoconf" },
        { ".bashrc",        "sh" },
        { ".bash_profile",  "sh" },
        { ".profile",       "sh" },
        { ".zshrc",         "sh" },
        { ".gitignore",     "gitignore" },
        { ".gitattributes", "gitattributes" },
        { NULL, NULL }
    };

    for (int i = 0; special[i].name; i++) {
        if (strcmp(base, special[i].name) == 0) {
            lang = gtk_source_language_manager_get_language(lm, special[i].lang_id);
            if (lang) return lang;
        }
    }

    return NULL;
}

/* With GtkSourceView: just set language + toggle highlighting on the buffer */
void highlight_apply(GtkTextBuffer *buf, const char *filepath, gboolean enabled)
{
    if (!GTK_SOURCE_IS_BUFFER(buf)) return;
    GtkSourceBuffer *sbuf = GTK_SOURCE_BUFFER(buf);
    gtk_source_buffer_set_highlight_syntax(sbuf, enabled);
    if (enabled && filepath) {
        GtkSourceLanguage *lang = source_guess_language(filepath);
        gtk_source_buffer_set_language(sbuf, lang);
    }
}

#else /* !HAVE_GTKSOURCEVIEW — custom regex-based highlighter */

typedef enum { LANG_NONE, LANG_C, LANG_PYTHON, LANG_SHELL, LANG_JS } Language;

static Language detect_lang(const char *path)
{
    if (!path) return LANG_NONE;
    const char *d = strrchr(path, '.');
    if (!d) return LANG_NONE;
    d++;
    if (!g_ascii_strcasecmp(d,"c")  || !g_ascii_strcasecmp(d,"h")  ||
        !g_ascii_strcasecmp(d,"cpp")|| !g_ascii_strcasecmp(d,"cc") ||
        !g_ascii_strcasecmp(d,"cxx")|| !g_ascii_strcasecmp(d,"hpp"))
        return LANG_C;
    if (!g_ascii_strcasecmp(d,"py"))  return LANG_PYTHON;
    if (!g_ascii_strcasecmp(d,"sh")  || !g_ascii_strcasecmp(d,"bash") ||
        !g_ascii_strcasecmp(d,"zsh") || !g_ascii_strcasecmp(d,"fish"))
        return LANG_SHELL;
    if (!g_ascii_strcasecmp(d,"js")  || !g_ascii_strcasecmp(d,"ts")  ||
        !g_ascii_strcasecmp(d,"jsx") || !g_ascii_strcasecmp(d,"tsx") ||
        !g_ascii_strcasecmp(d,"mjs"))
        return LANG_JS;
    return LANG_NONE;
}

static void ensure_tags(GtkTextBuffer *buf)
{
    GtkTextTagTable *t = gtk_text_buffer_get_tag_table(buf);
#define MAKE_TAG(name, ...) \
    if (!gtk_text_tag_table_lookup(t, name)) \
        gtk_text_buffer_create_tag(buf, name, __VA_ARGS__, NULL)
    MAKE_TAG("hl-keyword", "foreground", "#0055BB", "weight", PANGO_WEIGHT_BOLD);
    MAKE_TAG("hl-string",  "foreground", "#007B00");
    MAKE_TAG("hl-comment", "foreground", "#888888", "style", PANGO_STYLE_ITALIC);
    MAKE_TAG("hl-number",  "foreground", "#B06000");
    MAKE_TAG("hl-preproc", "foreground", "#800080");
#undef MAKE_TAG
}

static const char * const C_KW[] = {
    "auto","break","case","char","const","continue","default","do",
    "double","else","enum","extern","float","for","goto","if",
    "inline","int","long","register","restrict","return","short",
    "signed","sizeof","static","struct","switch","typedef","union",
    "unsigned","void","volatile","while",
    "bool","true","false","nullptr","class","namespace","template",
    "typename","virtual","override","public","private","protected",
    "new","delete","this","throw","try","catch","using","operator",
    NULL
};
static const char * const PY_KW[] = {
    "and","as","assert","async","await","break","class","continue",
    "def","del","elif","else","except","False","finally","for",
    "from","global","if","import","in","is","lambda","None",
    "nonlocal","not","or","pass","raise","return","True","try",
    "while","with","yield","self","super","print","range","len",
    NULL
};
static const char * const SH_KW[] = {
    "if","then","else","elif","fi","case","esac","for","while",
    "until","do","done","in","function","return","exit","break",
    "continue","export","local","readonly","source","true","false",
    NULL
};
static const char * const JS_KW[] = {
    "break","case","catch","class","const","continue","debugger",
    "default","delete","do","else","export","extends","false",
    "finally","for","function","if","import","in","instanceof",
    "let","new","null","return","static","super","switch","this",
    "throw","true","try","typeof","undefined","var","void",
    "while","with","yield","async","await","of",
    NULL
};

static gchar *make_kw_pattern(const char * const *kws)
{
    GString *s = g_string_new("\\b(?:");
    for (int i = 0; kws[i]; i++) {
        if (i) g_string_append_c(s, '|');
        g_string_append(s, kws[i]);
    }
    g_string_append(s, ")\\b");
    return g_string_free(s, FALSE);
}

static GRegex *build_regex(Language lang)
{
    const char * const *kws = NULL;
    const char *comment_pat = NULL;
    const char *preproc_pat = "[^\\s\\S]";

    switch (lang) {
    case LANG_C:
        kws = C_KW;
        comment_pat = "/\\*(?:[^*]|\\*(?!/))*\\*/|//[^\n]*";
        preproc_pat = "^[ \\t]*#[a-zA-Z][^\n]*";
        break;
    case LANG_PYTHON:
        kws = PY_KW;
        comment_pat = "#[^\n]*";
        break;
    case LANG_SHELL:
        kws = SH_KW;
        comment_pat = "#[^\n]*";
        break;
    case LANG_JS:
        kws = JS_KW;
        comment_pat = "/\\*(?:[^*]|\\*(?!/))*\\*/|//[^\n]*";
        break;
    default:
        return NULL;
    }

    gchar *kw_pat = make_kw_pattern(kws);
    gchar *full = g_strdup_printf(
        "(%s)"
        "|(\"(?:[^\"\\\\]|\\\\.)*\"|'(?:[^'\\\\]|\\\\.)*')"
        "|(%s)"
        "|(%s)"
        "|(\\b(?:0[xX][0-9a-fA-F]+|[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)[uUlLfF]*\\b)",
        comment_pat, preproc_pat, kw_pat);
    g_free(kw_pat);

    GError *err = NULL;
    GRegex *re = g_regex_new(full, G_REGEX_MULTILINE | G_REGEX_OPTIMIZE, 0, &err);
    if (err) {
        g_warning("highlight_apply: regex error: %s", err->message);
        g_clear_error(&err);
        re = NULL;
    }
    g_free(full);
    return re;
}

static const char *GROUP_TAGS[] = {
    "hl-comment", "hl-string", "hl-preproc", "hl-keyword", "hl-number"
};

void highlight_apply(GtkTextBuffer *buf, const char *filepath, gboolean enabled)
{
    GtkTextIter ts, te;
    gtk_text_buffer_get_bounds(buf, &ts, &te);
    for (int i = 0; i < 5; i++)
        gtk_text_buffer_remove_tag_by_name(buf, GROUP_TAGS[i], &ts, &te);
    if (!enabled) return;

    Language lang = detect_lang(filepath);
    if (lang == LANG_NONE) return;

    gchar *text = gtk_text_buffer_get_text(buf, &ts, &te, FALSE);
    gsize  tlen = strlen(text);
    if (tlen > 512 * 1024) { g_free(text); return; }

    GRegex *re = build_regex(lang);
    if (!re) { g_free(text); return; }

    ensure_tags(buf);

    GMatchInfo *mi = NULL;
    g_regex_match_full(re, text, (gssize)tlen, 0, 0, &mi, NULL);
    while (mi && g_match_info_matches(mi)) {
        for (int g = 1; g <= 5; g++) {
            gint bs = -1, be = -1;
            if (g_match_info_fetch_pos(mi, g, &bs, &be) && bs >= 0 && be > bs) {
                glong cs = g_utf8_pointer_to_offset(text, text + bs);
                glong ce = g_utf8_pointer_to_offset(text, text + be);
                GtkTextIter i1, i2;
                gtk_text_buffer_get_iter_at_offset(buf, &i1, (gint)cs);
                gtk_text_buffer_get_iter_at_offset(buf, &i2, (gint)ce);
                gtk_text_buffer_apply_tag_by_name(buf, GROUP_TAGS[g - 1], &i1, &i2);
                break;
            }
        }
        g_match_info_next(mi, NULL);
    }
    g_match_info_free(mi);
    g_regex_unref(re);
    g_free(text);
}

#endif /* HAVE_GTKSOURCEVIEW */
