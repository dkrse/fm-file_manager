#include "fm.h"
#include <string.h>
#ifdef HAVE_GTKSOURCEVIEW
#  include <gtksourceview/gtksource.h>
#endif

/* ─────────────────────────────────────────────────────────────────── *
 *  Settings – persistent font, GUI font, column widths, SSH bookmarks
 *  Storage: ~/.config/fm/settings.ini
 * ─────────────────────────────────────────────────────────────────── */

#define SETTINGS_GROUP_DISPLAY "display"
#define SETTINGS_GROUP_SSH     "ssh"

#define KEY_FAMILY      "font_family"
#define KEY_SIZE        "font_size"
#define KEY_GUI_FAMILY  "gui_font_family"
#define KEY_GUI_SIZE    "gui_font_size"
#define KEY_COL_NAME_W  "col_name_width"
#define KEY_COL_SIZE_W  "col_size_width"
#define KEY_COL_DATE_W  "col_date_width"
#define KEY_BOOKMARKS      "bookmarks"
#define KEY_SHOW_HIDDEN    "show_hidden"
#define KEY_SHOW_HOVER     "show_hover"
#define KEY_TERMINAL       "terminal_app"
#define KEY_CURSOR_COLOR    "cursor_color"
#define KEY_CURSOR_OUTLINE  "cursor_outline"
#define KEY_VIEWER_FAMILY   "viewer_font_family"
#define KEY_VIEWER_SIZE     "viewer_font_size"
#define KEY_EDITOR_FAMILY   "editor_font_family"
#define KEY_EDITOR_SIZE     "editor_font_size"
#define KEY_EDITOR_GUI_FAMILY "editor_gui_font_family"
#define KEY_EDITOR_GUI_SIZE   "editor_gui_font_size"
#define KEY_EDITOR_LINENUM      "editor_line_numbers"
#define KEY_EDITOR_LINENUM_SIZE  "editor_linenum_font_size"
#define KEY_SYNTAX_HIGHLIGHT         "syntax_highlight"
#define KEY_VIEWER_SYNTAX_HIGHLIGHT  "viewer_syntax_highlight"
#define KEY_VIEWER_LINE_NUMBERS      "viewer_line_numbers"
#define KEY_STYLE_SCHEME         "editor_style_scheme"
#define DEFAULT_STYLE_SCHEME     "classic"
#define KEY_DIR_COLOR            "dir_color"
#define KEY_DIR_BOLD             "dir_bold"
#define DEFAULT_DIR_COLOR        "#1565C0"
#define KEY_MARK_COLOR           "mark_color"
#define DEFAULT_MARK_COLOR       "#D32F2F"
#define KEY_ICON_SIZE            "icon_size"
#define DEFAULT_ICON_SIZE        16
#define KEY_AUTO_REFRESH         "auto_refresh"

#define DEFAULT_TERMINAL       "gnome-terminal"
#define DEFAULT_CURSOR_COLOR   "#1A73E8"
#define DEFAULT_VIEWER_FAMILY  "Monospace"
#define DEFAULT_VIEWER_SIZE    13
#define DEFAULT_EDITOR_FAMILY      "Monospace"
#define DEFAULT_EDITOR_SIZE        13
#define DEFAULT_EDITOR_GUI_FAMILY    "Sans"
#define DEFAULT_EDITOR_GUI_SIZE      11
#define DEFAULT_EDITOR_LINENUM_SIZE  10

#define DEFAULT_FONT_FAMILY    "Monospace"
#define DEFAULT_FONT_SIZE      12
#define DEFAULT_GUI_FAMILY     "Sans"
#define DEFAULT_GUI_SIZE       11
#define DEFAULT_COL_NAME_W     250
#define DEFAULT_COL_SIZE_W     100
#define DEFAULT_COL_DATE_W     145

/* Returns TRUE if the hex colour is perceptually light (→ use dark text) */
static gboolean color_is_light(const char *hex)
{
    if (!hex || hex[0] != '#' || strlen(hex) < 7) return FALSE;
    int r = 0, g = 0, b = 0;
    sscanf(hex + 1, "%02x%02x%02x", &r, &g, &b);
    return (0.299 * r + 0.587 * g + 0.114 * b) > 128.0;
}

static gchar *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "fm", "settings.ini", NULL);
}

/* Apply editor font CSS – text content + editor window GUI font */
void apply_editor_css(FM *fm)
{
    if (!fm->editor_css_provider) return;
    const char *fam = fm->editor_font_family && fm->editor_font_family[0]
                      ? fm->editor_font_family : DEFAULT_EDITOR_FAMILY;
    int fsz = fm->editor_font_size > 0 ? fm->editor_font_size : DEFAULT_EDITOR_SIZE;
    const char *gfam = fm->editor_gui_font_family && fm->editor_gui_font_family[0]
                       ? fm->editor_gui_font_family : DEFAULT_EDITOR_GUI_FAMILY;
    int gfsz = fm->editor_gui_font_size > 0 ? fm->editor_gui_font_size : DEFAULT_EDITOR_GUI_SIZE;
    gchar *css = g_strdup_printf(
        "textview.fm-editor, textview.fm-editor > text {"
        "  font-family: %s; font-size: %dpx; }"
        "label.fm-editor-status {"
        "  font-family: %s; font-size: %dpx; }",
        fam, fsz, gfam, gfsz);
    gtk_css_provider_load_from_string(fm->editor_css_provider, css);
    g_free(css);
}

/* Apply viewer font CSS – targets textview.fm-viewer */
void apply_viewer_css(FM *fm)
{
    if (!fm->viewer_css_provider) return;
    const char *fam = fm->viewer_font_family && fm->viewer_font_family[0]
                      ? fm->viewer_font_family : DEFAULT_VIEWER_FAMILY;
    int fsz = fm->viewer_font_size > 0 ? fm->viewer_font_size : DEFAULT_VIEWER_SIZE;
    gchar *css = g_strdup_printf(
        "textview.fm-viewer, textview.fm-viewer > text {"
        "  font-family: %s; font-size: %dpx; }",
        fam, fsz);
    gtk_css_provider_load_from_string(fm->viewer_css_provider, css);
    g_free(css);
}

/* Apply panel font CSS */
static void apply_font_css(FM *fm)
{
    if (!fm->font_css_provider) return;
    gchar *css = g_strdup_printf(
        "columnview { font-family: %s; font-size: %dpx; }",
        fm->font_family ? fm->font_family : DEFAULT_FONT_FAMILY,
        fm->font_size   > 0 ? fm->font_size : DEFAULT_FONT_SIZE);
    gtk_css_provider_load_from_string(fm->font_css_provider, css);
    g_free(css);
}

/* Apply GUI font CSS (buttons, labels, entries, dialogs) */
static void apply_gui_css(FM *fm)
{
    if (!fm->css_provider) return;

    const char *gui_fam = fm->gui_font_family ? fm->gui_font_family : DEFAULT_GUI_FAMILY;
    int gui_sz = fm->gui_font_size > 0 ? fm->gui_font_size : DEFAULT_GUI_SIZE;

    const char *cur_col = (fm->cursor_color && fm->cursor_color[0])
                          ? fm->cursor_color : DEFAULT_CURSOR_COLOR;

    gchar *cursor_css;
    if (fm->cursor_outline) {
        cursor_css = g_strdup_printf(
            "columnview listview row:selected {"
            "  background-color: transparent;"
            "  background-image: none;"
            "  color: @theme_fg_color;"
            "  box-shadow: inset 0 0 0 2px %s;"
            "}", cur_col);
    } else {
        const char *txt = color_is_light(cur_col) ? "#000000" : "#ffffff";
        cursor_css = g_strdup_printf(
            "columnview listview row:selected {"
            "  background-color: %s;"
            "  color: %s;"
            "}", cur_col, txt);
    }

    gchar *css = g_strdup_printf(
        "* { font-family: %s; font-size: %dpx; }"
        "entry.active-panel {"
        "  background-color: #E3F2FD;"
        "  color: #0D47A1;"
        "  font-weight: bold;"
        "}"
        "entry.inactive-panel {"
        "  background-color: #F5F5F5;"
        "  color: #555;"
        "}"
        "button.fm-fkey {"
        "  padding: 2px 4px;"
        "  min-height: 0;"
        "}"
        "paned > separator {"
        "  background-color: #90CAF9;"
        "  min-width: 3px;"
        "  min-height: 3px;"
        "}"
        "columnview:focus, columnview:focus-visible,"
        "columnview > *:focus, columnview > *:focus-visible,"
        "columnview listview:focus, columnview listview:focus-visible,"
        "columnview listview row:focus, columnview listview row:focus-visible {"
        "  outline: none;"
        "  outline-width: 0;"
        "}"
        "%s"
        "%s",
        gui_fam, gui_sz, cursor_css,
        fm->show_hover ? "" :
            "columnview listview row:hover:not(:selected) {"
            "  background-color: transparent;"
            "  background-image: none;"
            "}");
    g_free(cursor_css);
    gtk_css_provider_load_from_string(fm->css_provider, css);
    g_free(css);
}

/* Apply column widths to both panels */
void settings_apply_columns(FM *fm)
{
    int nw = fm->col_name_width > 0 ? fm->col_name_width : DEFAULT_COL_NAME_W;
    int sw = fm->col_size_width > 0 ? fm->col_size_width : DEFAULT_COL_SIZE_W;
    int dw = fm->col_date_width > 0 ? fm->col_date_width : DEFAULT_COL_DATE_W;

    for (int i = 0; i < 2; i++) {
        GtkColumnView *cv = GTK_COLUMN_VIEW(fm->panels[i].column_view);
        if (!cv) continue;
        GListModel *cols = gtk_column_view_get_columns(cv);
        GtkColumnViewColumn *c;

        c = g_list_model_get_item(cols, 0);  /* Name */
        if (c) { gtk_column_view_column_set_fixed_width(c, nw); g_object_unref(c); }
        c = g_list_model_get_item(cols, 1);  /* Size */
        if (c) { gtk_column_view_column_set_fixed_width(c, sw); g_object_unref(c); }
        c = g_list_model_get_item(cols, 2);  /* Date */
        if (c) { gtk_column_view_column_set_fixed_width(c, dw); g_object_unref(c); }
    }
}

static int load_int(GKeyFile *kf, const char *key, int def)
{
    GError *err = NULL;
    int val = g_key_file_get_integer(kf, SETTINGS_GROUP_DISPLAY, key, &err);
    if (err || val <= 0) { g_clear_error(&err); return def; }
    return val;
}

void settings_load(FM *fm)
{
    gchar *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    g_free(fm->font_family);
    fm->font_family = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_FAMILY, NULL);
    if (!fm->font_family || !fm->font_family[0]) {
        g_free(fm->font_family);
        fm->font_family = g_strdup(DEFAULT_FONT_FAMILY);
    }
    fm->font_size = load_int(kf, KEY_SIZE, DEFAULT_FONT_SIZE);

    g_free(fm->gui_font_family);
    fm->gui_font_family = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_GUI_FAMILY, NULL);
    if (!fm->gui_font_family || !fm->gui_font_family[0]) {
        g_free(fm->gui_font_family);
        fm->gui_font_family = g_strdup(DEFAULT_GUI_FAMILY);
    }
    fm->gui_font_size = load_int(kf, KEY_GUI_SIZE, DEFAULT_GUI_SIZE);

    fm->col_name_width = load_int(kf, KEY_COL_NAME_W, DEFAULT_COL_NAME_W);
    fm->col_size_width = load_int(kf, KEY_COL_SIZE_W, DEFAULT_COL_SIZE_W);
    fm->col_date_width = load_int(kf, KEY_COL_DATE_W, DEFAULT_COL_DATE_W);

    GError *sherr = NULL;
    fm->show_hidden = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_SHOW_HIDDEN, &sherr);
    if (sherr) { g_clear_error(&sherr); fm->show_hidden = FALSE; }

    GError *hoverr = NULL;
    fm->show_hover = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_SHOW_HOVER, &hoverr);
    if (hoverr) { g_clear_error(&hoverr); fm->show_hover = TRUE; }

    g_free(fm->terminal_app);
    fm->terminal_app = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_TERMINAL, NULL);
    if (!fm->terminal_app || !fm->terminal_app[0]) {
        g_free(fm->terminal_app);
        /* Auto-detect available terminal */
        const char *terms[] = { "ptyxis", "gnome-terminal", "konsole",
                                "xfce4-terminal", "mate-terminal", "xterm", NULL };
        fm->terminal_app = NULL;
        for (int i = 0; terms[i]; i++) {
            gchar *bin = g_find_program_in_path(terms[i]);
            if (bin) { g_free(bin); fm->terminal_app = g_strdup(terms[i]); break; }
        }
        if (!fm->terminal_app) fm->terminal_app = g_strdup(DEFAULT_TERMINAL);
    }

    g_free(fm->cursor_color);
    fm->cursor_color = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_CURSOR_COLOR, NULL);
    if (!fm->cursor_color || !fm->cursor_color[0]) {
        g_free(fm->cursor_color);
        fm->cursor_color = g_strdup(DEFAULT_CURSOR_COLOR);
    }

    GError *cuerr = NULL;
    fm->cursor_outline = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_CURSOR_OUTLINE, &cuerr);
    if (cuerr) { g_clear_error(&cuerr); fm->cursor_outline = FALSE; }

    g_free(fm->viewer_font_family);
    fm->viewer_font_family = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_FAMILY, NULL);
    if (!fm->viewer_font_family || !fm->viewer_font_family[0]) {
        g_free(fm->viewer_font_family);
        fm->viewer_font_family = g_strdup(DEFAULT_VIEWER_FAMILY);
    }
    fm->viewer_font_size = load_int(kf, KEY_VIEWER_SIZE, DEFAULT_VIEWER_SIZE);

    g_free(fm->editor_font_family);
    fm->editor_font_family = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_FAMILY, NULL);
    if (!fm->editor_font_family || !fm->editor_font_family[0]) {
        g_free(fm->editor_font_family);
        fm->editor_font_family = g_strdup(DEFAULT_EDITOR_FAMILY);
    }
    fm->editor_font_size = load_int(kf, KEY_EDITOR_SIZE, DEFAULT_EDITOR_SIZE);

    g_free(fm->editor_gui_font_family);
    fm->editor_gui_font_family = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_GUI_FAMILY, NULL);
    if (!fm->editor_gui_font_family || !fm->editor_gui_font_family[0]) {
        g_free(fm->editor_gui_font_family);
        fm->editor_gui_font_family = g_strdup(DEFAULT_EDITOR_GUI_FAMILY);
    }
    fm->editor_gui_font_size = load_int(kf, KEY_EDITOR_GUI_SIZE, DEFAULT_EDITOR_GUI_SIZE);

    GError *lnerr = NULL;
    fm->editor_line_numbers = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_LINENUM, &lnerr);
    if (lnerr) { g_clear_error(&lnerr); fm->editor_line_numbers = TRUE; }
    fm->editor_linenum_font_size = load_int(kf, KEY_EDITOR_LINENUM_SIZE, DEFAULT_EDITOR_LINENUM_SIZE);

    GError *sherr2 = NULL;
    fm->syntax_highlight = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_SYNTAX_HIGHLIGHT, &sherr2);
    if (sherr2) { g_clear_error(&sherr2); fm->syntax_highlight = TRUE; }

    GError *vsherr = NULL;
    fm->viewer_syntax_highlight = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_SYNTAX_HIGHLIGHT, &vsherr);
    if (vsherr) { g_clear_error(&vsherr); fm->viewer_syntax_highlight = TRUE; }

    GError *vlnerr = NULL;
    fm->viewer_line_numbers = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_LINE_NUMBERS, &vlnerr);
    if (vlnerr) { g_clear_error(&vlnerr); fm->viewer_line_numbers = FALSE; }

    g_free(fm->editor_style_scheme);
    fm->editor_style_scheme = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_STYLE_SCHEME, NULL);
    if (!fm->editor_style_scheme || !fm->editor_style_scheme[0]) {
        g_free(fm->editor_style_scheme);
        fm->editor_style_scheme = g_strdup(DEFAULT_STYLE_SCHEME);
    }

    g_free(fm->dir_color);
    fm->dir_color = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_DIR_COLOR, NULL);
    if (!fm->dir_color || !fm->dir_color[0]) {
        g_free(fm->dir_color);
        fm->dir_color = g_strdup(DEFAULT_DIR_COLOR);
    }

    GError *dberr = NULL;
    fm->dir_bold = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_DIR_BOLD, &dberr);
    if (dberr) { g_clear_error(&dberr); fm->dir_bold = TRUE; }

    g_free(fm->mark_color);
    fm->mark_color = g_key_file_get_string(kf, SETTINGS_GROUP_DISPLAY, KEY_MARK_COLOR, NULL);
    if (!fm->mark_color || !fm->mark_color[0]) {
        g_free(fm->mark_color);
        fm->mark_color = g_strdup(DEFAULT_MARK_COLOR);
    }

    fm->icon_size = load_int(kf, KEY_ICON_SIZE, DEFAULT_ICON_SIZE);
    if (fm->icon_size < 8)  fm->icon_size = 8;
    if (fm->icon_size > 64) fm->icon_size = 64;

    GError *arerr = NULL;
    fm->auto_refresh = g_key_file_get_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_AUTO_REFRESH, &arerr);
    if (arerr) { g_clear_error(&arerr); fm->auto_refresh = FALSE; }

    g_key_file_free(kf);

    apply_font_css(fm);
    apply_gui_css(fm);
    apply_editor_css(fm);
    apply_viewer_css(fm);
}

void settings_save(FM *fm)
{
    gchar *path = settings_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);

    g_key_file_set_string (kf, SETTINGS_GROUP_DISPLAY, KEY_FAMILY,
                           fm->font_family ? fm->font_family : DEFAULT_FONT_FAMILY);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_SIZE,
                           fm->font_size > 0 ? fm->font_size : DEFAULT_FONT_SIZE);
    g_key_file_set_string (kf, SETTINGS_GROUP_DISPLAY, KEY_GUI_FAMILY,
                           fm->gui_font_family ? fm->gui_font_family : DEFAULT_GUI_FAMILY);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_GUI_SIZE,
                           fm->gui_font_size > 0 ? fm->gui_font_size : DEFAULT_GUI_SIZE);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_COL_NAME_W,
                           fm->col_name_width > 0 ? fm->col_name_width : DEFAULT_COL_NAME_W);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_COL_SIZE_W,
                           fm->col_size_width > 0 ? fm->col_size_width : DEFAULT_COL_SIZE_W);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_COL_DATE_W,
                           fm->col_date_width > 0 ? fm->col_date_width : DEFAULT_COL_DATE_W);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_SHOW_HIDDEN, fm->show_hidden);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_SHOW_HOVER, fm->show_hover);
    g_key_file_set_string(kf, SETTINGS_GROUP_DISPLAY, KEY_TERMINAL,
                          fm->terminal_app ? fm->terminal_app : DEFAULT_TERMINAL);
    g_key_file_set_string(kf, SETTINGS_GROUP_DISPLAY, KEY_CURSOR_COLOR,
                          fm->cursor_color ? fm->cursor_color : DEFAULT_CURSOR_COLOR);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_CURSOR_OUTLINE, fm->cursor_outline);
    g_key_file_set_string (kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_FAMILY,
                           fm->viewer_font_family ? fm->viewer_font_family : DEFAULT_VIEWER_FAMILY);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_SIZE,
                           fm->viewer_font_size > 0 ? fm->viewer_font_size : DEFAULT_VIEWER_SIZE);
    g_key_file_set_string (kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_FAMILY,
                           fm->editor_font_family ? fm->editor_font_family : DEFAULT_EDITOR_FAMILY);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_SIZE,
                           fm->editor_font_size > 0 ? fm->editor_font_size : DEFAULT_EDITOR_SIZE);
    g_key_file_set_string (kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_GUI_FAMILY,
                           fm->editor_gui_font_family ? fm->editor_gui_font_family : DEFAULT_EDITOR_GUI_FAMILY);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_GUI_SIZE,
                           fm->editor_gui_font_size > 0 ? fm->editor_gui_font_size : DEFAULT_EDITOR_GUI_SIZE);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_LINENUM, fm->editor_line_numbers);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_EDITOR_LINENUM_SIZE,
                           fm->editor_linenum_font_size > 0 ? fm->editor_linenum_font_size : DEFAULT_EDITOR_LINENUM_SIZE);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_SYNTAX_HIGHLIGHT, fm->syntax_highlight);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_SYNTAX_HIGHLIGHT, fm->viewer_syntax_highlight);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_VIEWER_LINE_NUMBERS, fm->viewer_line_numbers);
    g_key_file_set_string(kf, SETTINGS_GROUP_DISPLAY, KEY_STYLE_SCHEME,
                          fm->editor_style_scheme ? fm->editor_style_scheme : DEFAULT_STYLE_SCHEME);
    g_key_file_set_string(kf, SETTINGS_GROUP_DISPLAY, KEY_DIR_COLOR,
                          fm->dir_color ? fm->dir_color : DEFAULT_DIR_COLOR);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_DIR_BOLD, fm->dir_bold);
    g_key_file_set_string(kf, SETTINGS_GROUP_DISPLAY, KEY_MARK_COLOR,
                          fm->mark_color ? fm->mark_color : DEFAULT_MARK_COLOR);
    g_key_file_set_integer(kf, SETTINGS_GROUP_DISPLAY, KEY_ICON_SIZE,
                           fm->icon_size > 0 ? fm->icon_size : DEFAULT_ICON_SIZE);
    g_key_file_set_boolean(kf, SETTINGS_GROUP_DISPLAY, KEY_AUTO_REFRESH, fm->auto_refresh);

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_warning("settings_save: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(path);
}

/* ── SSH bookmark helpers ─────────────────────────────────────────── */

#define KEY_SSH_CONNECTIONS "connections"

void ssh_bookmark_free(gpointer p)
{
    SshBookmark *bm = p;
    if (!bm) return;
    g_free(bm->name);
    g_free(bm->user);
    g_free(bm->host);
    g_free(bm->key_path);
    g_free(bm);
}

/* Format: "name|user|host|port" separated by ";" .
 * Also reads old "user@host" entries for backward compatibility. */
GList *settings_load_ssh_bookmarks(FM *fm)
{
    (void)fm;
    gchar *path = settings_path();
    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);

    /* Try new key first, fall back to old key */
    gchar *raw = g_key_file_get_string(kf, SETTINGS_GROUP_SSH,
                                        KEY_SSH_CONNECTIONS, NULL);
    if (!raw || !raw[0]) {
        g_free(raw);
        raw = g_key_file_get_string(kf, SETTINGS_GROUP_SSH, KEY_BOOKMARKS, NULL);
    }
    g_key_file_free(kf);

    if (!raw || !raw[0]) { g_free(raw); return NULL; }

    gchar **parts = g_strsplit(raw, ";", -1);
    g_free(raw);

    GList *list = NULL;
    for (int i = 0; parts[i]; i++) {
        if (!parts[i][0]) continue;
        gchar **f = g_strsplit(parts[i], "|", 5);
        int nf = g_strv_length(f);
        SshBookmark *bm = g_new0(SshBookmark, 1);
        if (nf >= 4) {
            bm->name = g_strdup(f[0]);
            bm->user = g_strdup(f[1]);
            bm->host = g_strdup(f[2]);
            bm->port = atoi(f[3]);
            if (nf >= 5 && f[4][0])
                bm->key_path = g_strdup(f[4]);
        } else {
            /* old "user@host" format */
            const gchar *at = strchr(parts[i], '@');
            if (at) {
                bm->user = g_strndup(parts[i], at - parts[i]);
                bm->host = g_strdup(at + 1);
            } else {
                bm->user = g_strdup("");
                bm->host = g_strdup(parts[i]);
            }
            bm->name = g_strdup_printf("%s@%s", bm->user, bm->host);
            bm->port = 22;
        }
        if (bm->port <= 0 || bm->port > 65535) bm->port = 22;
        g_strfreev(f);
        list = g_list_append(list, bm);
    }
    g_strfreev(parts);
    return list;
}

void settings_save_ssh_bookmarks(FM *fm, GList *list)
{
    (void)fm;
    gchar *path = settings_path();
    gchar *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    GKeyFile *kf = g_key_file_new();
    g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);

    GString *s = g_string_new(NULL);
    for (GList *l = list; l; l = l->next) {
        SshBookmark *bm = l->data;
        if (s->len) g_string_append_c(s, ';');
        g_string_append_printf(s, "%s|%s|%s|%d|%s",
            bm->name ? bm->name : "",
            bm->user ? bm->user : "",
            bm->host ? bm->host : "",
            bm->port > 0 ? bm->port : 22,
            bm->key_path ? bm->key_path : "");
    }
    g_key_file_set_string(kf, SETTINGS_GROUP_SSH, KEY_SSH_CONNECTIONS, s->str);
    g_string_free(s, TRUE);

    GError *err = NULL;
    if (!g_key_file_save_to_file(kf, path, &err)) {
        g_warning("settings_save_ssh_bookmarks: %s", err ? err->message : "?");
        g_clear_error(&err);
    }
    g_key_file_free(kf);
    g_free(path);
}

/* ── Settings dialog ─────────────────────────────────────────────── */

static void on_cursor_outline_toggled(GtkCheckButton *rb, gpointer color_btn)
{
    gtk_widget_set_sensitive(GTK_WIDGET(color_btn),
                             !gtk_check_button_get_active(rb));
}

void settings_dialog(FM *fm)
{
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "Settings");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 520, -1);

    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_start(outer, 16);
    gtk_widget_set_margin_end(outer, 16);
    gtk_widget_set_margin_top(outer, 12);
    gtk_widget_set_margin_bottom(outer, 12);

    GtkWidget *notebook = gtk_notebook_new();
    gtk_widget_set_vexpand(notebook, TRUE);
    gtk_box_append(GTK_BOX(outer), notebook);

    /* helper macro: create a padded grid for a tab */
#define MAKE_TAB_GRID(g) \
    GtkWidget *(g) = gtk_grid_new(); \
    gtk_grid_set_row_spacing(GTK_GRID(g), 10); \
    gtk_grid_set_column_spacing(GTK_GRID(g), 12); \
    gtk_widget_set_margin_start(g, 16); \
    gtk_widget_set_margin_end(g, 16); \
    gtk_widget_set_margin_top(g, 16); \
    gtk_widget_set_margin_bottom(g, 16);

    /* ═══════════════════════════════════════════════════════
     *  Tab 1 – Panely
     * ═══════════════════════════════════════════════════════ */
    MAKE_TAB_GRID(g1)
    int row = 0;

    GtkWidget *hdr1 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr1), "<b>Panel font (file list)</b>");
    gtk_widget_set_halign(hdr1, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g1), hdr1, 0, row++, 2, 1);

    GtkWidget *lbl_pfont = gtk_label_new("Font:");
    gtk_widget_set_halign(lbl_pfont, GTK_ALIGN_END);

    gchar *pfont_str = g_strdup_printf("%s %d",
        fm->font_family ? fm->font_family : DEFAULT_FONT_FAMILY,
        fm->font_size > 0 ? fm->font_size : DEFAULT_FONT_SIZE);
    PangoFontDescription *pfont_desc = pango_font_description_from_string(pfont_str);
    g_free(pfont_str);

    GtkFontDialog *pfd = gtk_font_dialog_new();
    GtkWidget *panel_font_btn = gtk_font_dialog_button_new(pfd);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(panel_font_btn), pfont_desc);
    pango_font_description_free(pfont_desc);
    gtk_widget_set_hexpand(panel_font_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g1), lbl_pfont,      0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), panel_font_btn, 1, row++, 1, 1);

    GtkWidget *hdr2 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr2), "<b>GUI font (buttons, dialogs)</b>");
    gtk_widget_set_halign(hdr2, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr2, 10);
    gtk_grid_attach(GTK_GRID(g1), hdr2, 0, row++, 2, 1);

    GtkWidget *lbl_gfont = gtk_label_new("Font:");
    gtk_widget_set_halign(lbl_gfont, GTK_ALIGN_END);

    gchar *gfont_str = g_strdup_printf("%s %d",
        fm->gui_font_family ? fm->gui_font_family : DEFAULT_GUI_FAMILY,
        fm->gui_font_size > 0 ? fm->gui_font_size : DEFAULT_GUI_SIZE);
    PangoFontDescription *gfont_desc = pango_font_description_from_string(gfont_str);
    g_free(gfont_str);

    GtkFontDialog *gfd = gtk_font_dialog_new();
    GtkWidget *gui_font_btn = gtk_font_dialog_button_new(gfd);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(gui_font_btn), gfont_desc);
    pango_font_description_free(gfont_desc);
    gtk_widget_set_hexpand(gui_font_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g1), lbl_gfont,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), gui_font_btn, 1, row++, 1, 1);

    GtkWidget *hdr3 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr3), "<b>Column widths (px)</b>");
    gtk_widget_set_halign(hdr3, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr3, 10);
    gtk_grid_attach(GTK_GRID(g1), hdr3, 0, row++, 2, 1);

    GtkWidget *lbl_cname = gtk_label_new("Name:");
    gtk_widget_set_halign(lbl_cname, GTK_ALIGN_END);
    GtkWidget *spin_name = gtk_spin_button_new_with_range(100, 800, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_name),
        fm->col_name_width > 0 ? fm->col_name_width : DEFAULT_COL_NAME_W);
    gtk_grid_attach(GTK_GRID(g1), lbl_cname, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), spin_name, 1, row++, 1, 1);

    GtkWidget *lbl_csize = gtk_label_new("Size:");
    gtk_widget_set_halign(lbl_csize, GTK_ALIGN_END);
    GtkWidget *spin_size = gtk_spin_button_new_with_range(60, 300, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_size),
        fm->col_size_width > 0 ? fm->col_size_width : DEFAULT_COL_SIZE_W);
    gtk_grid_attach(GTK_GRID(g1), lbl_csize, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), spin_size, 1, row++, 1, 1);

    GtkWidget *lbl_cdate = gtk_label_new("Modified:");
    gtk_widget_set_halign(lbl_cdate, GTK_ALIGN_END);
    GtkWidget *spin_date = gtk_spin_button_new_with_range(80, 300, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_date),
        fm->col_date_width > 0 ? fm->col_date_width : DEFAULT_COL_DATE_W);
    gtk_grid_attach(GTK_GRID(g1), lbl_cdate, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), spin_date, 1, row++, 1, 1);

    GtkWidget *hdr4 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr4), "<b>Display</b>");
    gtk_widget_set_halign(hdr4, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr4, 10);
    gtk_grid_attach(GTK_GRID(g1), hdr4, 0, row++, 2, 1);

    GtkWidget *chk_hidden = gtk_check_button_new_with_label("Show hidden files");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_hidden), fm->show_hidden);
    gtk_grid_attach(GTK_GRID(g1), chk_hidden, 0, row++, 2, 1);

    GtkWidget *chk_hover = gtk_check_button_new_with_label("Show row hover highlight");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_hover), fm->show_hover);
    gtk_grid_attach(GTK_GRID(g1), chk_hover, 0, row++, 2, 1);

    GtkWidget *chk_auto_refresh = gtk_check_button_new_with_label("Auto-refresh panels (watch for file changes)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_auto_refresh), fm->auto_refresh);
    gtk_grid_attach(GTK_GRID(g1), chk_auto_refresh, 0, row++, 2, 1);

    /* ── Directory appearance ── */
    GtkWidget *hdr_dir = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr_dir), "<b>Directories</b>");
    gtk_widget_set_halign(hdr_dir, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr_dir, 10);
    gtk_grid_attach(GTK_GRID(g1), hdr_dir, 0, row++, 2, 1);

    GtkWidget *lbl_dcolor = gtk_label_new("Color:");
    gtk_widget_set_halign(lbl_dcolor, GTK_ALIGN_END);
    GdkRGBA dir_rgba = {0};
    gdk_rgba_parse(&dir_rgba, fm->dir_color ? fm->dir_color : DEFAULT_DIR_COLOR);
    GtkColorDialog *dir_cdiag = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(dir_cdiag, FALSE);
    GtkWidget *dir_color_btn = gtk_color_dialog_button_new(dir_cdiag);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(dir_color_btn), &dir_rgba);
    gtk_widget_set_hexpand(dir_color_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g1), lbl_dcolor,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), dir_color_btn, 1, row++, 1, 1);

    GtkWidget *chk_dir_bold = gtk_check_button_new_with_label("Bold font");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_dir_bold), fm->dir_bold);
    gtk_grid_attach(GTK_GRID(g1), chk_dir_bold, 0, row++, 2, 1);

    /* ── Mark color ── */
    GtkWidget *hdr_mark = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr_mark), "<b>Marked files (Insert/Space)</b>");
    gtk_widget_set_halign(hdr_mark, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr_mark, 10);
    gtk_grid_attach(GTK_GRID(g1), hdr_mark, 0, row++, 2, 1);

    GtkWidget *lbl_mcolor = gtk_label_new("Color:");
    gtk_widget_set_halign(lbl_mcolor, GTK_ALIGN_END);
    GdkRGBA mark_rgba = {0};
    gdk_rgba_parse(&mark_rgba, fm->mark_color ? fm->mark_color : DEFAULT_MARK_COLOR);
    GtkColorDialog *mark_cdiag = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(mark_cdiag, FALSE);
    GtkWidget *mark_color_btn = gtk_color_dialog_button_new(mark_cdiag);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(mark_color_btn), &mark_rgba);
    gtk_widget_set_hexpand(mark_color_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g1), lbl_mcolor,     0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), mark_color_btn,  1, row++, 1, 1);

    /* ── Icon size ── */
    GtkWidget *hdr_icon = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr_icon), "<b>Icons</b>");
    gtk_widget_set_halign(hdr_icon, GTK_ALIGN_START);
    gtk_widget_set_margin_top(hdr_icon, 10);
    gtk_grid_attach(GTK_GRID(g1), hdr_icon, 0, row++, 2, 1);

    GtkWidget *lbl_iconsz = gtk_label_new("Icon size (px):");
    gtk_widget_set_halign(lbl_iconsz, GTK_ALIGN_END);
    GtkWidget *spin_iconsz = gtk_spin_button_new_with_range(8, 64, 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_iconsz),
        fm->icon_size > 0 ? fm->icon_size : DEFAULT_ICON_SIZE);
    gtk_grid_attach(GTK_GRID(g1), lbl_iconsz,  0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g1), spin_iconsz, 1, row++, 1, 1);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g1,
                             gtk_label_new("Panels"));

    /* ═══════════════════════════════════════════════════════
     *  Tab 2 – Kurzor
     * ═══════════════════════════════════════════════════════ */
    MAKE_TAB_GRID(g2)
    row = 0;

    GtkWidget *hdr6 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr6), "<b>Panel cursor style</b>");
    gtk_widget_set_halign(hdr6, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g2), hdr6, 0, row++, 2, 1);

    GtkWidget *radio_outline = gtk_check_button_new_with_label("Outline only (transparent)");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(radio_outline), fm->cursor_outline);
    gtk_grid_attach(GTK_GRID(g2), radio_outline, 0, row++, 2, 1);

    GtkWidget *radio_filled = gtk_check_button_new_with_label("Filled color");
    gtk_check_button_set_group(GTK_CHECK_BUTTON(radio_filled),
                               GTK_CHECK_BUTTON(radio_outline));
    gtk_check_button_set_active(GTK_CHECK_BUTTON(radio_filled), !fm->cursor_outline);
    gtk_grid_attach(GTK_GRID(g2), radio_filled, 0, row++, 2, 1);

    GtkWidget *lbl_cur = gtk_label_new("Color:");
    gtk_widget_set_halign(lbl_cur, GTK_ALIGN_END);

    GdkRGBA cur_rgba = { 0.102, 0.451, 0.910, 1.0 };
    {
        const char *cc = fm->cursor_color ? fm->cursor_color : DEFAULT_CURSOR_COLOR;
        gdk_rgba_parse(&cur_rgba, cc);
    }
    GtkColorDialog *cdiag = gtk_color_dialog_new();
    gtk_color_dialog_set_with_alpha(cdiag, FALSE);
    GtkWidget *cur_color_btn = gtk_color_dialog_button_new(cdiag);
    gtk_color_dialog_button_set_rgba(GTK_COLOR_DIALOG_BUTTON(cur_color_btn), &cur_rgba);
    gtk_widget_set_sensitive(cur_color_btn, !fm->cursor_outline);
    gtk_widget_set_hexpand(cur_color_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g2), lbl_cur,       0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g2), cur_color_btn, 1, row++, 1, 1);

    g_signal_connect(radio_outline, "toggled",
        G_CALLBACK(on_cursor_outline_toggled), cur_color_btn);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g2,
                             gtk_label_new("Cursor"));

    /* ═══════════════════════════════════════════════════════
     *  Tab 3 – Viewer
     * ═══════════════════════════════════════════════════════ */
    MAKE_TAB_GRID(g_viewer)
    row = 0;

    GtkWidget *hdr_v = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr_v), "<b>File viewer (F3)</b>");
    gtk_widget_set_halign(hdr_v, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g_viewer), hdr_v, 0, row++, 2, 1);

    GtkWidget *lbl_vfont = gtk_label_new("Font:");
    gtk_widget_set_halign(lbl_vfont, GTK_ALIGN_END);

    gchar *vfont_str = g_strdup_printf("%s %d",
        fm->viewer_font_family ? fm->viewer_font_family : DEFAULT_VIEWER_FAMILY,
        fm->viewer_font_size > 0 ? fm->viewer_font_size : DEFAULT_VIEWER_SIZE);
    PangoFontDescription *vfont_desc = pango_font_description_from_string(vfont_str);
    g_free(vfont_str);

    GtkFontDialog *vfd = gtk_font_dialog_new();
    GtkWidget *viewer_font_btn = gtk_font_dialog_button_new(vfd);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(viewer_font_btn), vfont_desc);
    pango_font_description_free(vfont_desc);
    gtk_widget_set_hexpand(viewer_font_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g_viewer), lbl_vfont,      0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g_viewer), viewer_font_btn, 1, row++, 1, 1);

    GtkWidget *chk_viewer_synhl = gtk_check_button_new_with_label(
#ifdef HAVE_GTKSOURCEVIEW
        "Syntax highlighting"
#else
        "Syntax highlighting (C, Python, Shell, JS)"
#endif
    );
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_viewer_synhl), fm->viewer_syntax_highlight);
    gtk_grid_attach(GTK_GRID(g_viewer), chk_viewer_synhl, 0, row++, 2, 1);

#ifdef HAVE_GTKSOURCEVIEW
    GtkWidget *chk_viewer_linenum = gtk_check_button_new_with_label("Show line numbers");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_viewer_linenum), fm->viewer_line_numbers);
    gtk_grid_attach(GTK_GRID(g_viewer), chk_viewer_linenum, 0, row++, 2, 1);
#endif

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g_viewer,
                             gtk_label_new("Viewer"));

    /* ═══════════════════════════════════════════════════════
     *  Tab 4 – Editor
     * ═══════════════════════════════════════════════════════ */
    MAKE_TAB_GRID(g3)
    row = 0;

    GtkWidget *hdr7 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr7), "<b>Text editor (F4)</b>");
    gtk_widget_set_halign(hdr7, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g3), hdr7, 0, row++, 2, 1);

    /* text content font */
    GtkWidget *lbl_efont = gtk_label_new("Text font:");
    gtk_widget_set_halign(lbl_efont, GTK_ALIGN_END);

    gchar *efont_str = g_strdup_printf("%s %d",
        fm->editor_font_family ? fm->editor_font_family : DEFAULT_EDITOR_FAMILY,
        fm->editor_font_size > 0 ? fm->editor_font_size : DEFAULT_EDITOR_SIZE);
    PangoFontDescription *efont_desc = pango_font_description_from_string(efont_str);
    g_free(efont_str);

    GtkFontDialog *efd = gtk_font_dialog_new();
    GtkWidget *editor_font_btn = gtk_font_dialog_button_new(efd);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(editor_font_btn), efont_desc);
    pango_font_description_free(efont_desc);
    gtk_widget_set_hexpand(editor_font_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g3), lbl_efont,       0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g3), editor_font_btn, 1, row++, 1, 1);

    /* editor GUI font (status bar, window chrome) */
    GtkWidget *lbl_egfont = gtk_label_new("Editor GUI font:");
    gtk_widget_set_halign(lbl_egfont, GTK_ALIGN_END);

    gchar *egfont_str = g_strdup_printf("%s %d",
        fm->editor_gui_font_family ? fm->editor_gui_font_family : DEFAULT_EDITOR_GUI_FAMILY,
        fm->editor_gui_font_size > 0 ? fm->editor_gui_font_size : DEFAULT_EDITOR_GUI_SIZE);
    PangoFontDescription *egfont_desc = pango_font_description_from_string(egfont_str);
    g_free(egfont_str);

    GtkFontDialog *egfd = gtk_font_dialog_new();
    GtkWidget *editor_gui_font_btn = gtk_font_dialog_button_new(egfd);
    gtk_font_dialog_button_set_font_desc(GTK_FONT_DIALOG_BUTTON(editor_gui_font_btn), egfont_desc);
    pango_font_description_free(egfont_desc);
    gtk_widget_set_hexpand(editor_gui_font_btn, TRUE);
    gtk_grid_attach(GTK_GRID(g3), lbl_egfont,        0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g3), editor_gui_font_btn, 1, row++, 1, 1);

    GtkWidget *chk_linenum = gtk_check_button_new_with_label("Show line numbers");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_linenum), fm->editor_line_numbers);
    gtk_grid_attach(GTK_GRID(g3), chk_linenum, 0, row++, 2, 1);

    GtkWidget *lbl_lnsz = gtk_label_new("Line number font size:");
    gtk_widget_set_halign(lbl_lnsz, GTK_ALIGN_END);
    GtkWidget *spin_lnsz = gtk_spin_button_new_with_range(6, 32, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin_lnsz),
        fm->editor_linenum_font_size > 0 ? fm->editor_linenum_font_size : DEFAULT_EDITOR_LINENUM_SIZE);
    gtk_grid_attach(GTK_GRID(g3), lbl_lnsz,  0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g3), spin_lnsz, 1, row++, 1, 1);

    GtkWidget *chk_synhl = gtk_check_button_new_with_label(
#ifdef HAVE_GTKSOURCEVIEW
        "Syntax highlighting (GtkSourceView)"
#else
        "Syntax highlighting (C, Python, Shell, JS)"
#endif
    );
    gtk_check_button_set_active(GTK_CHECK_BUTTON(chk_synhl), fm->syntax_highlight);
    gtk_grid_attach(GTK_GRID(g3), chk_synhl, 0, row++, 2, 1);

#ifdef HAVE_GTKSOURCEVIEW
    /* Style scheme combo */
    GtkWidget *lbl_scheme = gtk_label_new("Color scheme:");
    gtk_widget_set_halign(lbl_scheme, GTK_ALIGN_END);

    GtkStringList *scheme_list = gtk_string_list_new(NULL);
    GtkSourceStyleSchemeManager *_ssm = gtk_source_style_scheme_manager_get_default();
    const gchar * const *scheme_ids = gtk_source_style_scheme_manager_get_scheme_ids(_ssm);
    int scheme_sel = 0, scheme_idx = 0;
    const gchar *cur_scheme = fm->editor_style_scheme ? fm->editor_style_scheme : DEFAULT_STYLE_SCHEME;
    if (scheme_ids) {
        for (int i = 0; scheme_ids[i]; i++) {
            gtk_string_list_append(scheme_list, scheme_ids[i]);
            if (!g_strcmp0(scheme_ids[i], cur_scheme)) scheme_sel = scheme_idx;
            scheme_idx++;
        }
    }
    GtkWidget *combo_scheme = gtk_drop_down_new(G_LIST_MODEL(scheme_list), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(combo_scheme), (guint)scheme_sel);
    gtk_widget_set_hexpand(combo_scheme, TRUE);
    gtk_grid_attach(GTK_GRID(g3), lbl_scheme,    0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g3), combo_scheme,  1, row++, 1, 1);
#endif

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g3,
                             gtk_label_new("Editor"));

    /* ═══════════════════════════════════════════════════════
     *  Tab 4 – System
     * ═══════════════════════════════════════════════════════ */
    MAKE_TAB_GRID(g4)
    row = 0;

    GtkWidget *hdr5 = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr5), "<b>Applications</b>");
    gtk_widget_set_halign(hdr5, GTK_ALIGN_START);
    gtk_grid_attach(GTK_GRID(g4), hdr5, 0, row++, 2, 1);

    GtkWidget *lbl_term = gtk_label_new("Terminal:");
    gtk_widget_set_halign(lbl_term, GTK_ALIGN_END);
    GtkWidget *entry_term = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry_term),
        fm->terminal_app ? fm->terminal_app : DEFAULT_TERMINAL);
    gtk_widget_set_hexpand(entry_term, TRUE);
    gtk_widget_set_tooltip_text(entry_term,
        "E.g. gnome-terminal, konsole, xfce4-terminal, xterm");
    gtk_grid_attach(GTK_GRID(g4), lbl_term,   0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(g4), entry_term, 1, row++, 1, 1);

    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), g4,
                             gtk_label_new("System"));

#undef MAKE_TAB_GRID

    /* ── Buttons row ── */
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 10);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn     = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(outer), btn_box);

    gtk_window_set_child(GTK_WINDOW(dlg), outer);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),     &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),  &ctx);

    gtk_window_present(GTK_WINDOW(dlg));

    if (dlg_ctx_run(&ctx) == 1) {
        /* panel font */
        PangoFontDescription *pfd_result =
            gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(panel_font_btn));
        if (pfd_result) {
            const gchar *family = pango_font_description_get_family(pfd_result);
            gint size = pango_font_description_get_size(pfd_result) / PANGO_SCALE;
            if (size <= 0) size = DEFAULT_FONT_SIZE;
            g_free(fm->font_family);
            fm->font_family = g_strdup(family ? family : DEFAULT_FONT_FAMILY);
            fm->font_size = size;
        }
        apply_font_css(fm);

        /* GUI font */
        PangoFontDescription *gfd_result =
            gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(gui_font_btn));
        if (gfd_result) {
            const gchar *family = pango_font_description_get_family(gfd_result);
            gint size = pango_font_description_get_size(gfd_result) / PANGO_SCALE;
            if (size <= 0) size = DEFAULT_GUI_SIZE;
            g_free(fm->gui_font_family);
            fm->gui_font_family = g_strdup(family ? family : DEFAULT_GUI_FAMILY);
            fm->gui_font_size = size;
        }
        apply_gui_css(fm);

        /* column widths */
        fm->col_name_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_name));
        fm->col_size_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_size));
        fm->col_date_width = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_date));
        settings_apply_columns(fm);

        /* show hidden */
        fm->show_hidden = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_hidden));
        fm->show_hover = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_hover));

        /* auto-refresh */
        gboolean new_ar = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_auto_refresh));
        if (new_ar != fm->auto_refresh) {
            fm->auto_refresh = new_ar;
            for (int i = 0; i < 2; i++) {
                if (fm->auto_refresh)
                    panel_monitor_start(&fm->panels[i]);
                else
                    panel_monitor_stop(&fm->panels[i]);
            }
        }

        /* directory appearance */
        {
            const GdkRGBA *drgba =
                gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(dir_color_btn));
            g_free(fm->dir_color);
            if (drgba)
                fm->dir_color = g_strdup_printf("#%02x%02x%02x",
                    (int)(drgba->red   * 255 + 0.5),
                    (int)(drgba->green * 255 + 0.5),
                    (int)(drgba->blue  * 255 + 0.5));
            else
                fm->dir_color = g_strdup(DEFAULT_DIR_COLOR);
        }
        fm->dir_bold = gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_dir_bold));

        /* mark color */
        {
            const GdkRGBA *mrgba =
                gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(mark_color_btn));
            g_free(fm->mark_color);
            if (mrgba)
                fm->mark_color = g_strdup_printf("#%02x%02x%02x",
                    (int)(mrgba->red   * 255 + 0.5),
                    (int)(mrgba->green * 255 + 0.5),
                    (int)(mrgba->blue  * 255 + 0.5));
            else
                fm->mark_color = g_strdup(DEFAULT_MARK_COLOR);
        }

        /* icon size */
        fm->icon_size = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_iconsz));

        /* terminal */
        const gchar *term_txt = gtk_editable_get_text(GTK_EDITABLE(entry_term));
        g_free(fm->terminal_app);
        fm->terminal_app = g_strdup(term_txt && term_txt[0] ? term_txt : DEFAULT_TERMINAL);

        /* editor */
        PangoFontDescription *efd_result =
            gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(editor_font_btn));
        if (efd_result) {
            const gchar *efamily = pango_font_description_get_family(efd_result);
            gint esize = pango_font_description_get_size(efd_result) / PANGO_SCALE;
            if (esize <= 0) esize = DEFAULT_EDITOR_SIZE;
            g_free(fm->editor_font_family);
            fm->editor_font_family = g_strdup(efamily ? efamily : DEFAULT_EDITOR_FAMILY);
            fm->editor_font_size = esize;
        }
        PangoFontDescription *egfd_result =
            gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(editor_gui_font_btn));
        if (egfd_result) {
            const gchar *egfamily = pango_font_description_get_family(egfd_result);
            gint egsize = pango_font_description_get_size(egfd_result) / PANGO_SCALE;
            if (egsize <= 0) egsize = DEFAULT_EDITOR_GUI_SIZE;
            g_free(fm->editor_gui_font_family);
            fm->editor_gui_font_family = g_strdup(egfamily ? egfamily : DEFAULT_EDITOR_GUI_FAMILY);
            fm->editor_gui_font_size = egsize;
        }
        fm->editor_line_numbers =
            gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_linenum));
        fm->editor_linenum_font_size =
            gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(spin_lnsz));
        fm->syntax_highlight =
            gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_synhl));
#ifdef HAVE_GTKSOURCEVIEW
        {
            guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(combo_scheme));
            GtkSourceStyleSchemeManager *_sm2 = gtk_source_style_scheme_manager_get_default();
            const gchar * const *sids = gtk_source_style_scheme_manager_get_scheme_ids(_sm2);
            g_free(fm->editor_style_scheme);
            if (sids && sids[sel])
                fm->editor_style_scheme = g_strdup(sids[sel]);
            else
                fm->editor_style_scheme = g_strdup(DEFAULT_STYLE_SCHEME);
        }
#endif

        /* cursor style */
        fm->cursor_outline = gtk_check_button_get_active(GTK_CHECK_BUTTON(radio_outline));
        {
            const GdkRGBA *rgba =
                gtk_color_dialog_button_get_rgba(GTK_COLOR_DIALOG_BUTTON(cur_color_btn));
            g_free(fm->cursor_color);
            if (rgba)
                fm->cursor_color = g_strdup_printf("#%02x%02x%02x",
                    (int)(rgba->red   * 255 + 0.5),
                    (int)(rgba->green * 255 + 0.5),
                    (int)(rgba->blue  * 255 + 0.5));
            else
                fm->cursor_color = g_strdup(DEFAULT_CURSOR_COLOR);
        }
        /* viewer font */
        PangoFontDescription *vfd_result =
            gtk_font_dialog_button_get_font_desc(GTK_FONT_DIALOG_BUTTON(viewer_font_btn));
        if (vfd_result) {
            const gchar *vfamily = pango_font_description_get_family(vfd_result);
            gint vsize = pango_font_description_get_size(vfd_result) / PANGO_SCALE;
            if (vsize <= 0) vsize = DEFAULT_VIEWER_SIZE;
            g_free(fm->viewer_font_family);
            fm->viewer_font_family = g_strdup(vfamily ? vfamily : DEFAULT_VIEWER_FAMILY);
            fm->viewer_font_size = vsize;
        }
        fm->viewer_syntax_highlight =
            gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_viewer_synhl));
#ifdef HAVE_GTKSOURCEVIEW
        fm->viewer_line_numbers =
            gtk_check_button_get_active(GTK_CHECK_BUTTON(chk_viewer_linenum));
#endif
        apply_viewer_css(fm);

        apply_gui_css(fm);
        apply_editor_css(fm);

        settings_save(fm);
        panel_reload(&fm->panels[0]);
        panel_reload(&fm->panels[1]);
        fm_status(fm, "Settings saved");
    }
    gtk_window_destroy(GTK_WINDOW(dlg));

    /* Restore focus to active panel */
    Panel *ap = &fm->panels[fm->active];
    ap->inhibit_sel = TRUE;
    gtk_widget_grab_focus(ap->column_view);
}
