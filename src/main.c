#include "fm.h"
#include <string.h>
#include <stdlib.h>
#include <locale.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>

static FM g_fm;

/* ─────────────────────────────────────────────────────────────────── *
 *  Utility
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
    struct tm *tm = localtime(&tt);
    char buf[32] = "";
    if (tm) strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    return g_strdup(buf);
}

void fm_status(FM *fm, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gtk_label_set_text(GTK_LABEL(fm->statusbar), msg);
    g_free(msg);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Icon name helpers — freedesktop standard content-type icons
 * ─────────────────────────────────────────────────────────────────── */

/* Well-known directory names → themed folder icons */
static const char *icon_for_dir(const char *name)
{
    static const struct { const char *name; const char *icon; } dirs[] = {
        { "Desktop",       "folder-desktop"       },
        { "Documents",     "folder-documents"     },
        { "Downloads",     "folder-download"      },
        { "Music",         "folder-music"         },
        { "Pictures",      "folder-pictures"      },
        { "Videos",        "folder-videos"        },
        { "Templates",     "folder-templates"     },
        { "Public",        "folder-publicshare"   },
        { ".git",          "folder-git"           },
        { NULL, NULL }
    };
    if (!name) return "folder";
    for (int i = 0; dirs[i].name; i++)
        if (strcmp(name, dirs[i].name) == 0) return dirs[i].icon;
    return "folder";
}

/* File extension → standard freedesktop icon name */
static const char *icon_for_file(const char *name)
{
    if (!name) return "text-x-generic";

    const char *dot = strrchr(name, '.');
    if (!dot || dot == name) {
        /* No extension — check special filenames */
        if (g_str_has_prefix(name, "Makefile") ||
            g_str_has_prefix(name, "CMakeLists") ||
            g_str_has_prefix(name, "Dockerfile") ||
            g_str_has_prefix(name, "Vagrantfile"))
            return "text-x-script";
        if (strcmp(name, "LICENSE") == 0 || strcmp(name, "COPYING") == 0)
            return "text-x-copying";
        if (strcmp(name, "README") == 0)
            return "text-x-readme";
        return "text-x-generic";
    }

    const char *ext = dot + 1;

    /* Images */
    if (!g_ascii_strcasecmp(ext, "png")  || !g_ascii_strcasecmp(ext, "jpg") ||
        !g_ascii_strcasecmp(ext, "jpeg") || !g_ascii_strcasecmp(ext, "gif") ||
        !g_ascii_strcasecmp(ext, "bmp")  || !g_ascii_strcasecmp(ext, "svg") ||
        !g_ascii_strcasecmp(ext, "webp") || !g_ascii_strcasecmp(ext, "ico") ||
        !g_ascii_strcasecmp(ext, "tiff") || !g_ascii_strcasecmp(ext, "tif") ||
        !g_ascii_strcasecmp(ext, "raw")  || !g_ascii_strcasecmp(ext, "psd"))
        return "image-x-generic";

    /* Audio */
    if (!g_ascii_strcasecmp(ext, "mp3")  || !g_ascii_strcasecmp(ext, "ogg") ||
        !g_ascii_strcasecmp(ext, "flac") || !g_ascii_strcasecmp(ext, "wav") ||
        !g_ascii_strcasecmp(ext, "aac")  || !g_ascii_strcasecmp(ext, "wma") ||
        !g_ascii_strcasecmp(ext, "m4a")  || !g_ascii_strcasecmp(ext, "opus"))
        return "audio-x-generic";

    /* Video */
    if (!g_ascii_strcasecmp(ext, "mp4")  || !g_ascii_strcasecmp(ext, "mkv") ||
        !g_ascii_strcasecmp(ext, "avi")  || !g_ascii_strcasecmp(ext, "mov") ||
        !g_ascii_strcasecmp(ext, "wmv")  || !g_ascii_strcasecmp(ext, "flv") ||
        !g_ascii_strcasecmp(ext, "webm") || !g_ascii_strcasecmp(ext, "m4v"))
        return "video-x-generic";

    /* Archives */
    if (!g_ascii_strcasecmp(ext, "zip")  || !g_ascii_strcasecmp(ext, "tar") ||
        !g_ascii_strcasecmp(ext, "gz")   || !g_ascii_strcasecmp(ext, "bz2") ||
        !g_ascii_strcasecmp(ext, "xz")   || !g_ascii_strcasecmp(ext, "7z")  ||
        !g_ascii_strcasecmp(ext, "rar")  || !g_ascii_strcasecmp(ext, "zst") ||
        !g_ascii_strcasecmp(ext, "tgz")  || !g_ascii_strcasecmp(ext, "tbz2") ||
        !g_ascii_strcasecmp(ext, "txz")  || !g_ascii_strcasecmp(ext, "deb") ||
        !g_ascii_strcasecmp(ext, "rpm"))
        return "package-x-generic";

    /* PDF */
    if (!g_ascii_strcasecmp(ext, "pdf"))
        return "application-pdf";

    /* Office documents */
    if (!g_ascii_strcasecmp(ext, "doc")  || !g_ascii_strcasecmp(ext, "docx") ||
        !g_ascii_strcasecmp(ext, "odt")  || !g_ascii_strcasecmp(ext, "rtf"))
        return "x-office-document";
    if (!g_ascii_strcasecmp(ext, "xls")  || !g_ascii_strcasecmp(ext, "xlsx") ||
        !g_ascii_strcasecmp(ext, "ods")  || !g_ascii_strcasecmp(ext, "csv"))
        return "x-office-spreadsheet";
    if (!g_ascii_strcasecmp(ext, "ppt")  || !g_ascii_strcasecmp(ext, "pptx") ||
        !g_ascii_strcasecmp(ext, "odp"))
        return "x-office-presentation";

    /* Web / markup */
    if (!g_ascii_strcasecmp(ext, "html") || !g_ascii_strcasecmp(ext, "htm") ||
        !g_ascii_strcasecmp(ext, "xhtml"))
        return "text-html";
    if (!g_ascii_strcasecmp(ext, "css"))
        return "text-css";
    if (!g_ascii_strcasecmp(ext, "xml")  || !g_ascii_strcasecmp(ext, "xsl") ||
        !g_ascii_strcasecmp(ext, "xslt"))
        return "text-xml";
    if (!g_ascii_strcasecmp(ext, "json"))
        return "application-json";

    /* Source code */
    if (!g_ascii_strcasecmp(ext, "c")    || !g_ascii_strcasecmp(ext, "h") ||
        !g_ascii_strcasecmp(ext, "cpp")  || !g_ascii_strcasecmp(ext, "cc") ||
        !g_ascii_strcasecmp(ext, "cxx")  || !g_ascii_strcasecmp(ext, "hpp"))
        return "text-x-csrc";
    if (!g_ascii_strcasecmp(ext, "py")   || !g_ascii_strcasecmp(ext, "pyw"))
        return "text-x-python";
    if (!g_ascii_strcasecmp(ext, "java")  || !g_ascii_strcasecmp(ext, "class") ||
        !g_ascii_strcasecmp(ext, "jar"))
        return "application-x-java";
    if (!g_ascii_strcasecmp(ext, "js")   || !g_ascii_strcasecmp(ext, "mjs") ||
        !g_ascii_strcasecmp(ext, "ts")   || !g_ascii_strcasecmp(ext, "jsx") ||
        !g_ascii_strcasecmp(ext, "tsx"))
        return "application-javascript";
    if (!g_ascii_strcasecmp(ext, "rs"))
        return "text-x-rust";
    if (!g_ascii_strcasecmp(ext, "go"))
        return "text-x-go";
    if (!g_ascii_strcasecmp(ext, "rb"))
        return "application-x-ruby";
    if (!g_ascii_strcasecmp(ext, "php"))
        return "application-x-php";
    if (!g_ascii_strcasecmp(ext, "sql"))
        return "text-x-sql";

    /* Shell / scripts */
    if (!g_ascii_strcasecmp(ext, "sh")   || !g_ascii_strcasecmp(ext, "bash") ||
        !g_ascii_strcasecmp(ext, "zsh")  || !g_ascii_strcasecmp(ext, "fish") ||
        !g_ascii_strcasecmp(ext, "pl")   || !g_ascii_strcasecmp(ext, "lua"))
        return "text-x-script";

    /* Config / data */
    if (!g_ascii_strcasecmp(ext, "conf") || !g_ascii_strcasecmp(ext, "cfg") ||
        !g_ascii_strcasecmp(ext, "ini")  || !g_ascii_strcasecmp(ext, "toml") ||
        !g_ascii_strcasecmp(ext, "yaml") || !g_ascii_strcasecmp(ext, "yml"))
        return "text-x-generic";

    /* Markdown / docs */
    if (!g_ascii_strcasecmp(ext, "md")   || !g_ascii_strcasecmp(ext, "rst") ||
        !g_ascii_strcasecmp(ext, "txt")  || !g_ascii_strcasecmp(ext, "log"))
        return "text-x-generic";

    /* Fonts */
    if (!g_ascii_strcasecmp(ext, "ttf")  || !g_ascii_strcasecmp(ext, "otf") ||
        !g_ascii_strcasecmp(ext, "woff") || !g_ascii_strcasecmp(ext, "woff2"))
        return "font-x-generic";

    /* Shared libraries */
    if (!g_ascii_strcasecmp(ext, "so")   || !g_ascii_strcasecmp(ext, "dll") ||
        !g_ascii_strcasecmp(ext, "dylib"))
        return "application-x-sharedlib";

    /* Disk images */
    if (!g_ascii_strcasecmp(ext, "iso")  || !g_ascii_strcasecmp(ext, "img"))
        return "application-x-cd-image";

    /* Database */
    if (!g_ascii_strcasecmp(ext, "db")   || !g_ascii_strcasecmp(ext, "sqlite") ||
        !g_ascii_strcasecmp(ext, "sqlite3"))
        return "application-vnd.oasis.opendocument.database";

    return "text-x-generic";
}

/* Combined: pick icon based on entry type + name */
const char *icon_for_entry(const char *name, gboolean is_dir,
                           gboolean is_link, gboolean is_exec)
{
    if (is_link) return "emblem-symbolic-link";
    if (is_dir)  return icon_for_dir(name);
    if (is_exec) return "application-x-executable";
    return icon_for_file(name);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel helpers
 * ─────────────────────────────────────────────────────────────────── */

void panel_update_status(Panel *p)
{
    guint n_marks = p->marks ? g_hash_table_size(p->marks) : 0;
    guint total = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));

    gchar *msg;
    if (n_marks > 0)
        msg = g_strdup_printf("%u marked  |  %u items",
                              n_marks, total > 0 ? total - 1 : 0);
    else
        msg = g_strdup_printf("%u items", total > 0 ? total - 1 : 0);
    gtk_label_set_text(GTK_LABEL(p->status_label), msg);
    g_free(msg);
}

gchar *panel_cursor_name(Panel *p)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (p->cursor_pos >= n) return NULL;
    FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                           p->cursor_pos);
    if (!item) return NULL;
    gchar *name = g_strdup(item->name);
    g_object_unref(item);
    return name;
}

GList *panel_selection(Panel *p)
{
    /* Collect marked items */
    GList *result = NULL;
    if (p->marks && g_hash_table_size(p->marks) > 0) {
        guint count = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
        for (guint i = 0; i < count; i++) {
            FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
            if (item) {
                if (item->marked && strcmp(item->name, "..") != 0)
                    result = g_list_prepend(result, g_strdup(item->name));
                g_object_unref(item);
            }
        }
        if (result) return g_list_reverse(result);
    }

    /* No marks → fall back to cursor item */
    gchar *name = panel_cursor_name(p);
    if (name && strcmp(name, "..") != 0)
        return g_list_prepend(NULL, name);
    g_free(name);
    return NULL;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel: load directory
 * ─────────────────────────────────────────────────────────────────── */

void panel_load(Panel *p, const char *path)
{
    if (p->ssh_conn) { panel_load_remote(p, path); return; }

    struct stat st;
    if (!path || stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;

    /* Clear marks when entering a different directory */
    if (strcmp(p->cwd, path) != 0 && p->marks)
        g_hash_table_remove_all(p->marks);

    /* Suppress selection events during entire load */
    p->inhibit_sel = TRUE;

    g_strlcpy(p->cwd, path, sizeof(p->cwd));
    gtk_editable_set_text(GTK_EDITABLE(p->path_entry), path);

    DIR *dir = opendir(path);

    /* Collect all items into a GPtrArray first, then splice once.
     * This emits a single items-changed on the store so GtkSortListModel
     * sorts only once (O(n log n)) instead of once per append (O(n² log n)). */
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);

    /* ".." entry always first */
    g_ptr_array_add(items,
        file_item_new("go-up-symbolic", "..", "", "",
                      TRUE, p->fm->dir_color, p->fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL,
                      (gint64)-2, (gint64)0));

    if (dir) {
        int dfd = dirfd(dir);   /* for fstatat – avoids building full path */
        struct dirent *de;
        struct stat fst;

        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                continue;
            if (!p->fm->show_hidden && de->d_name[0] == '.')
                continue;

            if (fstatat(dfd, de->d_name, &fst, AT_SYMLINK_NOFOLLOW) != 0)
                continue;

            gboolean is_dir  = S_ISDIR(fst.st_mode);
            gboolean is_link = S_ISLNK(fst.st_mode);
            gboolean is_exec = !is_dir && (fst.st_mode & (S_IXUSR|S_IXGRP|S_IXOTH));

            const char *icon = icon_for_entry(de->d_name, is_dir, is_link, is_exec);

            const gchar *fg = NULL;
            gint         wt = PANGO_WEIGHT_NORMAL;
            if      (is_dir)  { fg = p->fm->dir_color; wt = p->fm->dir_bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL; }
            else if (is_link) { fg = "#7B1FA2"; }
            else if (is_exec) { fg = "#2E7D32"; }

            /* file_item_new_take takes ownership of size/date strings */
            FileItem *fi = file_item_new_take(icon, de->d_name,
                              is_dir ? g_strdup("<DIR>") : fmt_size(fst.st_size),
                              fmt_date(fst.st_mtime),
                              is_dir, fg, wt,
                              (gint64)fst.st_size, (gint64)fst.st_mtime);
            if (p->marks && g_hash_table_contains(p->marks, de->d_name))
                fi->marked = TRUE;
            g_ptr_array_add(items, fi);
        }
        closedir(dir);
    }

    /* Single splice: one items-changed signal → one sort pass */
    g_list_store_remove_all(p->store);
    g_list_store_splice(p->store, 0, 0,
                        (gpointer *)items->pdata, items->len);
    g_ptr_array_unref(items);

    panel_update_status(p);

    /* move cursor to first row */
    p->cursor_pos = 0;
    if (p->column_view && g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0) {
        /* Only select (show cursor) on active panel */
        gboolean is_active = (p->fm->active == p->idx);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL,
                                  is_active ? GTK_LIST_SCROLL_SELECT
                                            : GTK_LIST_SCROLL_NONE,
                                  NULL);
    }

    p->inhibit_sel = FALSE;
}

void panel_reload(Panel *p)
{
    if (p->ssh_conn) {
        panel_load_remote(p, p->ssh_conn->remote_path);
        return;
    }

    gchar *saved = panel_cursor_name(p);
    panel_load(p, p->cwd);  /* inhibit_sel is handled inside panel_load */

    if (!saved) return;

    /* Only select (show cursor) on active panel */
    gboolean is_active = (p->fm->active == p->idx);

    p->inhibit_sel = TRUE;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (item && strcmp(item->name, saved) == 0) {
            g_object_unref(item);
            p->cursor_pos = i;
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                      NULL,
                                      is_active ? GTK_LIST_SCROLL_SELECT
                                                : GTK_LIST_SCROLL_NONE,
                                      NULL);
            break;
        }
        if (item) g_object_unref(item);
    }
    p->inhibit_sel = FALSE;
    g_free(saved);
}

void panel_go_up(Panel *p)
{
    if (p->ssh_conn) {
        if (strcmp(p->ssh_conn->remote_path, "/") == 0) return;
        gchar *parent   = g_path_get_dirname(p->ssh_conn->remote_path);
        gchar *basename = g_path_get_basename(p->ssh_conn->remote_path);
        panel_load_remote(p, parent);
        /* restore cursor */
        guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
        for (guint i = 0; i < n; i++) {
            FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
            if (item && strcmp(item->name, basename) == 0) {
                g_object_unref(item);
                p->cursor_pos = i;
                p->inhibit_sel = TRUE;
                gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                          NULL, GTK_LIST_SCROLL_SELECT, NULL);
                p->inhibit_sel = FALSE;
                break;
            }
            if (item) g_object_unref(item);
        }
        g_free(parent); g_free(basename);
        return;
    }

    if (strcmp(p->cwd, "/") == 0) return;
    gchar *parent   = g_path_get_dirname(p->cwd);
    gchar *basename = g_path_get_basename(p->cwd);
    panel_load(p, parent);

    /* restore cursor to dir we came from */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    for (guint i = 0; i < n; i++) {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
        if (item && strcmp(item->name, basename) == 0) {
            g_object_unref(item);
            p->cursor_pos = i;
            p->inhibit_sel = TRUE;
            gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                      NULL, GTK_LIST_SCROLL_SELECT, NULL);
            p->inhibit_sel = FALSE;
            break;
        }
        if (item) g_object_unref(item);
    }
    g_free(parent);
    g_free(basename);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Sort comparators
 * ─────────────────────────────────────────────────────────────────── */

/* Directories-first sorter – used as primary in GtkMultiSorter so it is
 * never affected by the column view's ascending/descending toggle. */
static int dirs_first_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    if (strcmp(fa->name, "..") == 0) return -1;
    if (strcmp(fb->name, "..") == 0) return  1;
    if (fa->is_dir && !fb->is_dir)  return -1;
    if (!fa->is_dir && fb->is_dir)  return  1;
    return 0;
}

static int name_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    return g_utf8_collate(fa->name, fb->name);
}

static int size_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    return (fa->size_raw < fb->size_raw) ? -1 : (fa->size_raw > fb->size_raw) ? 1 : 0;
}

static int date_sort_func(gconstpointer a, gconstpointer b, gpointer data)
{
    (void)data;
    FileItem *fa = FILE_ITEM((gpointer)a);
    FileItem *fb = FILE_ITEM((gpointer)b);
    return (fa->date_raw < fb->date_raw) ? -1 : (fa->date_raw > fb->date_raw) ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Column factories
 * ─────────────────────────────────────────────────────────────────── */

/* Name column: icon + label */
static void name_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *box   = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    GtkWidget *image  = gtk_image_new();
    GtkWidget *label  = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), image);
    gtk_box_append(GTK_BOX(box), label);
    gtk_list_item_set_child(li, box);
}

static void name_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f;
    Panel     *p     = d;
    GtkWidget *box   = gtk_list_item_get_child(li);
    GtkWidget *image = gtk_widget_get_first_child(box);
    GtkWidget *label = gtk_widget_get_next_sibling(image);
    FileItem  *item  = gtk_list_item_get_item(li);

    gtk_image_set_from_icon_name(GTK_IMAGE(image), item->icon_name);
    gtk_image_set_pixel_size(GTK_IMAGE(image),
        p->fm->icon_size > 0 ? p->fm->icon_size : 16);

    const gchar *fg = item->marked ? (p->fm->mark_color ? p->fm->mark_color : "#D32F2F") : item->fg_color;
    gint wt = item->marked ? PANGO_WEIGHT_BOLD : item->weight;
    if (fg) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground='%s' weight='%d'>%s</span>",
            fg, wt, item->name);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), item->name);
    }
}

/* Size column */
static void size_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 1.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_item_set_child(li, label);
}

static void size_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f;
    Panel     *p     = d;
    GtkWidget *label = gtk_list_item_get_child(li);
    FileItem  *item  = gtk_list_item_get_item(li);

    const gchar *fg = item->marked ? (p->fm->mark_color ? p->fm->mark_color : "#D32F2F") : item->fg_color;
    if (fg) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground='%s'>%s</span>", fg, item->size);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), item->size);
    }
}

/* Date column */
static void date_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f; (void)d;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_list_item_set_child(li, label);
}

static void date_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer d)
{
    (void)f;
    Panel     *p     = d;
    GtkWidget *label = gtk_list_item_get_child(li);
    FileItem  *item  = gtk_list_item_get_item(li);

    const gchar *fg = item->marked ? (p->fm->mark_color ? p->fm->mark_color : "#D32F2F") : item->fg_color;
    if (fg) {
        gchar *markup = g_markup_printf_escaped(
            "<span foreground='%s'>%s</span>", fg, item->date);
        gtk_label_set_markup(GTK_LABEL(label), markup);
        g_free(markup);
    } else {
        gtk_label_set_text(GTK_LABEL(label), item->date);
    }
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Callbacks – column view
 * ─────────────────────────────────────────────────────────────────── */

static void activate_item(Panel *p)
{
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (p->cursor_pos >= n) return;

    FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                           p->cursor_pos);
    if (!item) return;

    gchar    *name   = g_strdup(item->name);
    gboolean  is_dir = item->is_dir;
    g_object_unref(item);

    if (p->ssh_conn) {
        if (strcmp(name, "..") == 0) {
            panel_go_up(p);
        } else if (is_dir) {
            gchar *np = g_strdup_printf("%s/%s", p->ssh_conn->remote_path, name);
            panel_load_remote(p, np);
            g_free(np);
        } else {
#ifdef HAVE_LIBSSH2
            fm_status(p->fm, "Downloading '%s'...", name);
            while (g_main_context_pending(NULL)) g_main_context_iteration(NULL, FALSE);
            gchar *remote_path = g_strdup_printf("%s/%s",
                p->ssh_conn->remote_path, name);
            gsize len = 0;
            gchar *data = ssh_read_file(p->ssh_conn, remote_path, &len);
            g_free(remote_path);
            if (data && len > 0) {
                /* Create unique temp dir to prevent symlink attacks */
                gchar *tmpdir = g_dir_make_tmp("fm-view-XXXXXX", NULL);
                if (tmpdir) {
                    gchar *tmp = g_build_filename(tmpdir, name, NULL);
                    if (g_file_set_contents(tmp, data, (gssize)len, NULL)) {
                        gchar *uri = g_filename_to_uri(tmp, NULL, NULL);
                        if (uri) {
                            gtk_show_uri(GTK_WINDOW(p->fm->window),
                                uri, GDK_CURRENT_TIME);
                            g_free(uri);
                        }
                        fm_status(p->fm, "Opened: %s", name);
                    } else {
                        fm_status(p->fm, "Error writing to temp directory");
                    }
                    g_free(tmp);
                    g_free(tmpdir);
                } else {
                    fm_status(p->fm, "Error creating temp directory");
                }
            } else {
                fm_status(p->fm, "Error downloading '%s'", name);
            }
            g_free(data);
#else
            fm_status(p->fm, "Opening remote files is not supported");
#endif
        }
        g_free(name);
        return;
    }

    if (strcmp(name, "..") == 0) {
        panel_go_up(p);
    } else if (is_dir) {
        gchar *newpath = g_build_filename(p->cwd, name, NULL);
        panel_load(p, newpath);
        g_free(newpath);
    } else {
        gchar *fullpath = g_build_filename(p->cwd, name, NULL);
        gchar *uri = g_filename_to_uri(fullpath, NULL, NULL);
        if (uri) {
            gtk_show_uri(GTK_WINDOW(p->fm->window), uri, GDK_CURRENT_TIME);
            g_free(uri);
        }
        g_free(fullpath);
    }
    g_free(name);
}

static void on_cv_activate(GtkColumnView *cv, guint pos, Panel *p)
{
    (void)cv;
    p->cursor_pos = pos;
    activate_item(p);
}

static gboolean on_tree_key_press(GtkEventControllerKey *ctrl,
                                   guint keyval, guint keycode,
                                   GdkModifierType state, Panel *p)
{
    (void)ctrl; (void)keycode; (void)state;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (n == 0) return FALSE;

    switch (keyval) {

    case GDK_KEY_Up:
        if (p->cursor_pos > 0) p->cursor_pos--;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Down:
        if (p->cursor_pos < n - 1) p->cursor_pos++;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Page_Up:
        p->cursor_pos = (p->cursor_pos > 20) ? p->cursor_pos - 20 : 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Page_Down:
        p->cursor_pos = MIN(p->cursor_pos + 20, n - 1);
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_Home:
        p->cursor_pos = 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            0, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_End:
        p->cursor_pos = n > 0 ? n - 1 : 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        return TRUE;

    case GDK_KEY_space:
    case GDK_KEY_Insert: {
        FileItem *item = g_list_model_get_item(G_LIST_MODEL(p->filter_model),
                                                p->cursor_pos);
        if (item && strcmp(item->name, "..") != 0) {
            item->marked = !item->marked;
            if (!p->marks)
                p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
            if (item->marked)
                g_hash_table_add(p->marks, g_strdup(item->name));
            else
                g_hash_table_remove(p->marks, item->name);
            /* Force re-render: remove and re-insert in store */
            guint sp;
            if (g_list_store_find(p->store, item, &sp)) {
                g_object_ref(item);
                g_list_store_remove(p->store, sp);
                g_list_store_insert(p->store, sp, item);
                g_object_unref(item);
            }
        }
        if (item) g_object_unref(item);
        if (p->cursor_pos < n - 1) p->cursor_pos++;
        p->inhibit_sel = TRUE;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
            p->cursor_pos, NULL, GTK_LIST_SCROLL_SELECT, NULL);
        p->inhibit_sel = FALSE;
        panel_update_status(p);
        return TRUE;
    }

    case GDK_KEY_BackSpace:
        panel_go_up(p);
        return TRUE;

    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        activate_item(p);
        return TRUE;

    case GDK_KEY_plus:
    case GDK_KEY_KP_Add: {
        /* Toggle mark all: if any are marked, unmark all; otherwise mark all */
        if (!p->marks)
            p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        gboolean has_marks = g_hash_table_size(p->marks) > 0;
        if (has_marks) {
            g_hash_table_remove_all(p->marks);
        } else {
            guint count = g_list_model_get_n_items(G_LIST_MODEL(p->store));
            for (guint i = 0; i < count; i++) {
                FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->store), i);
                if (it && strcmp(it->name, "..") != 0)
                    g_hash_table_add(p->marks, g_strdup(it->name));
                if (it) g_object_unref(it);
            }
        }
        panel_reload(p);
        return TRUE;
    }

    default:
        break;
    }
    return FALSE;
}

/* Helper: update active-panel CSS on both path entries */
static void set_active_panel(FM *fm, int idx)
{
    fm->active = idx;
    for (int i = 0; i < 2; i++) {
        Panel *pp = &fm->panels[i];
        if (i == idx) {
            gtk_widget_add_css_class(pp->path_entry, "active-panel");
            gtk_widget_remove_css_class(pp->path_entry, "inactive-panel");
        } else {
            gtk_widget_remove_css_class(pp->path_entry, "active-panel");
            gtk_widget_add_css_class(pp->path_entry, "inactive-panel");
        }
    }
}

/* Selection changed: fired on mouse click (and our own scroll_to).
 * When exactly ONE item ends up selected, treat that as the new cursor. */
static void on_selection_changed(GtkSelectionModel *model, guint position,
                                  guint n_items, Panel *p)
{
    (void)position; (void)n_items;

    if (p->inhibit_sel) return;

    GtkBitset *bs = gtk_selection_model_get_selection(model);
    guint64 n_sel = gtk_bitset_get_size(bs);

    if (n_sel == 1) {
        GtkBitsetIter iter;
        guint pos;
        if (gtk_bitset_iter_init_first(&iter, bs, &pos)) {
            p->cursor_pos = pos;
            set_active_panel(p->fm, p->idx);
        }
    }
    gtk_bitset_unref(bs);
}

static void on_focus_leave(GtkEventControllerFocus *ctrl, Panel *p)
{
    (void)ctrl;
    p->inhibit_sel = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(p->selection));
    p->inhibit_sel = FALSE;
}

static void on_focus_enter(GtkEventControllerFocus *ctrl, Panel *p)
{
    (void)ctrl;
    set_active_panel(p->fm, p->idx);

    /* inhibit_sel is pre-set to TRUE only on Tab-switch (keyboard).
     * For mouse clicks it is FALSE — the click's on_selection_changed
     * will set the cursor, so don't restore the old position here. */
    if (!p->inhibit_sel) return;

    /* Keyboard focus: restore saved cursor position. */
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (n > 0) {
        if (p->cursor_pos >= n) p->cursor_pos = 0;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view),
                                  p->cursor_pos, NULL,
                                  GTK_LIST_SCROLL_SELECT, NULL);
    }
    p->inhibit_sel = FALSE;
}

static void do_path_activate(Panel *p)
{
    /* IMPORTANT: copy text because gtk_editable_get_text returns internal buffer
     * that becomes invalid when we modify the entry */
    gchar *text = g_strdup(gtk_editable_get_text(GTK_EDITABLE(p->path_entry)));

    if (!text || !text[0]) {
        g_free(text);
        gtk_editable_set_text(GTK_EDITABLE(p->path_entry), p->cwd);
        return;
    }

    if (p->ssh_conn) {
        /* SSH connected – parse path from entry or use as-is */
        const char *remote_path = text;

        /* If user typed sftp://user@host/path, extract just the path */
        if (g_str_has_prefix(text, "sftp://")) {
            const char *host_start = text + 7;
            const char *path_start = strchr(host_start, '/');
            if (path_start)
                remote_path = path_start;
            else
                remote_path = "/";
        }

        /* Handle relative paths for SSH */
        gchar *full_path = NULL;
        if (remote_path[0] != '/') {
            /* Relative path – prepend current remote directory */
            const char *base = p->ssh_conn->remote_path;
            if (base[strlen(base)-1] == '/')
                full_path = g_strdup_printf("%s%s", base, remote_path);
            else
                full_path = g_strdup_printf("%s/%s", base, remote_path);
            remote_path = full_path;
        }

        panel_load_remote(p, remote_path);
        g_free(full_path);
    } else {
        /* Local panel – build full path */
        gchar *full_path = NULL;

        /* Handle ~ expansion */
        if (text[0] == '~') {
            if (text[1] == '\0' || text[1] == '/')
                full_path = g_strdup_printf("%s%s", g_get_home_dir(), text + 1);
            else
                full_path = g_strdup(text);  /* ~user not supported, try as-is */
        }
        /* Handle relative paths */
        else if (text[0] != '/') {
            if (p->cwd[strlen(p->cwd)-1] == '/')
                full_path = g_strdup_printf("%s%s", p->cwd, text);
            else
                full_path = g_strdup_printf("%s/%s", p->cwd, text);
        }

        const char *path_to_check = full_path ? full_path : text;
        struct stat st;
        if (stat(path_to_check, &st) == 0 && S_ISDIR(st.st_mode))
            panel_load(p, path_to_check);
        else
            gtk_editable_set_text(GTK_EDITABLE(p->path_entry), p->cwd);

        g_free(full_path);
    }
    g_free(text);
}

static gboolean on_path_key_press(GtkEventControllerKey *ctrl,
                                   guint keyval, guint keycode,
                                   GdkModifierType state, Panel *p)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
        do_path_activate(p);
        gtk_widget_grab_focus(p->column_view);
        return TRUE;
    }
    if (keyval == GDK_KEY_Escape) {
        gtk_editable_set_text(GTK_EDITABLE(p->path_entry), p->cwd);
        gtk_widget_grab_focus(p->column_view);
        return TRUE;
    }
    return FALSE;
}

static void on_path_activate(GtkEntry *entry, Panel *p)
{
    (void)entry;
    do_path_activate(p);
}

static void on_up_clicked(GtkButton *btn, Panel *p) { (void)btn; panel_go_up(p); }

static void on_disconnect_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    ssh_panel_close(p);
    gtk_widget_set_visible(p->disconnect_btn, FALSE);
}

static void on_terminal_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    const char *term = p->fm->terminal_app;
    if (!term || !term[0]) term = "gnome-terminal";

    if (p->ssh_conn) {
        const char *rpath = p->ssh_conn->remote_path;

        /* Shell-quote remote path to prevent command injection */
        gchar *quoted_path = g_shell_quote(rpath);
        gchar *sh_cmd = (p->ssh_conn->port != 22)
            ? g_strdup_printf("ssh -t -p %d %s@%s 'cd %s && exec $SHELL -l'",
                              p->ssh_conn->port,
                              p->ssh_conn->user, p->ssh_conn->host, quoted_path)
            : g_strdup_printf("ssh -t %s@%s 'cd %s && exec $SHELL -l'",
                              p->ssh_conn->user, p->ssh_conn->host, quoted_path);
        g_free(quoted_path);

        /* Use argv array — no shell parsing, no injection */
        const gchar *argv[7];
        int a = 0;
        argv[a++] = term;
        if (strstr(term, "ptyxis")) {
            argv[a++] = "--new-window";
        }
        argv[a++] = "--";
        argv[a++] = "bash";
        argv[a++] = "-c";
        argv[a++] = sh_cmd;
        argv[a] = NULL;

        GError *err = NULL;
        if (!g_spawn_async(NULL, (gchar **)argv, NULL,
                           G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                           NULL, NULL, NULL, &err)) {
            fm_status(p->fm, "Failed to open terminal: %s",
                      err ? err->message : "?");
            g_clear_error(&err);
        }
        g_free(sh_cmd);
        return;
    }

    /* Local terminal — use g_spawn_async with argv to prevent injection */
    {
        const gchar *argv[5];
        int a = 0;
        argv[a++] = term;
        if (strstr(term, "ptyxis")) {
            argv[a++] = "--new-window";
            argv[a++] = "-d";
            argv[a++] = p->cwd;
        } else if (strstr(term, "gnome-terminal") || strstr(term, "mate-terminal")) {
            argv[a++] = "--working-directory";
            argv[a++] = p->cwd;
        } else if (strstr(term, "konsole")) {
            argv[a++] = "--workdir";
            argv[a++] = p->cwd;
        } else if (strstr(term, "xfce4-terminal")) {
            argv[a++] = "--default-working-directory";
            argv[a++] = p->cwd;
        }
        argv[a] = NULL;

        GError *err = NULL;
        if (!g_spawn_async(p->cwd, (gchar **)argv, NULL,
                           G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                           NULL, NULL, NULL, &err)) {
            fm_status(p->fm, "Failed to open terminal: %s",
                      err ? err->message : "?");
            g_clear_error(&err);
        }
    }
}

static void on_home_clicked(GtkButton *btn, Panel *p)
{
    (void)btn;
    panel_load(p, g_get_home_dir());
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel filter (Ctrl+S)
 * ─────────────────────────────────────────────────────────────────── */

static gboolean filter_match_func(gpointer item, gpointer user_data)
{
    Panel *p = user_data;
    if (!p->filter_text || !p->filter_text[0]) return TRUE;
    FileItem *fi = FILE_ITEM(item);
    if (strcmp(fi->name, "..") == 0) return TRUE;  /* always show ".." */

    gchar *lower = g_utf8_strdown(fi->name, -1);
    gboolean match = (strstr(lower, p->filter_text) != NULL);
    g_free(lower);
    return match;
}

static void on_filter_changed(GtkEditable *editable, Panel *p)
{
    g_free(p->filter_text);
    const gchar *text = gtk_editable_get_text(editable);
    p->filter_text = (text && text[0]) ? g_utf8_strdown(text, -1) : NULL;

    gtk_filter_changed(GTK_FILTER(p->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);

    /* Reset cursor to top */
    p->cursor_pos = 0;
    guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
    if (n > 0) {
        p->inhibit_sel = TRUE;
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL, GTK_LIST_SCROLL_SELECT, NULL);
        p->inhibit_sel = FALSE;
    }
    panel_update_status(p);
}

static gboolean on_filter_key(GtkEventControllerKey *ctrl,
                               guint keyval, guint keycode,
                               GdkModifierType state, Panel *p)
{
    (void)ctrl; (void)keycode; (void)state;
    if (keyval == GDK_KEY_Escape || keyval == GDK_KEY_Return ||
        keyval == GDK_KEY_KP_Enter) {
        /* Close filter bar */
        gtk_widget_set_visible(p->filter_bar, FALSE);
        g_free(p->filter_text);
        p->filter_text = NULL;
        gtk_editable_set_text(GTK_EDITABLE(p->filter_entry), "");
        gtk_filter_changed(GTK_FILTER(p->custom_filter), GTK_FILTER_CHANGE_DIFFERENT);
        gtk_widget_grab_focus(p->column_view);
        panel_update_status(p);
        return TRUE;
    }
    return FALSE;
}

static void panel_show_filter(Panel *p)
{
    gtk_widget_set_visible(p->filter_bar, TRUE);
    gtk_widget_grab_focus(p->filter_entry);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Panel: widget setup
 * ─────────────────────────────────────────────────────────────────── */

void panel_setup(Panel *p, FM *fm, int idx)
{
    p->fm  = fm;
    p->idx = idx;
    p->marks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    /* outer vertical box */
    p->frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_margin_start(p->frame, 2);
    gtk_widget_set_margin_end(p->frame, 2);

    /* ── path bar ── */
    GtkWidget *path_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    p->path_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->path_entry), "Path...");
    gtk_widget_set_hexpand(p->path_entry, TRUE);

    GtkWidget *up_btn   = gtk_button_new_from_icon_name("go-up-symbolic");
    GtkWidget *home_btn = gtk_button_new_from_icon_name("go-home-symbolic");
    gtk_widget_set_tooltip_text(up_btn,   "Go up (Backspace)");
    gtk_widget_set_tooltip_text(home_btn, "Home directory");

    /* SSH disconnect button – initially hidden */
    p->disconnect_btn = gtk_button_new_from_icon_name("network-offline-symbolic");
    gtk_widget_set_tooltip_text(p->disconnect_btn, "Disconnect SSH");
    gtk_widget_set_visible(p->disconnect_btn, FALSE);

    /* Terminal button – always visible */
    p->terminal_btn = gtk_button_new_from_icon_name("utilities-terminal-symbolic");
    gtk_widget_set_tooltip_text(p->terminal_btn, "Open terminal");

    gtk_box_append(GTK_BOX(path_box), p->path_entry);
    gtk_box_append(GTK_BOX(path_box), p->terminal_btn);
    gtk_box_append(GTK_BOX(path_box), p->disconnect_btn);
    gtk_box_append(GTK_BOX(path_box), up_btn);
    gtk_box_append(GTK_BOX(path_box), home_btn);

    /* ── GListStore ── */
    p->store = g_list_store_new(FILE_ITEM_TYPE);

    /* ── Column view ── */
    p->column_view = gtk_column_view_new(NULL);
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(p->column_view), TRUE);
    gtk_column_view_set_show_row_separators(GTK_COLUMN_VIEW(p->column_view), FALSE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(p->column_view), FALSE);

    /* Name column */
    GtkListItemFactory *name_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(name_factory, "setup", G_CALLBACK(name_setup), NULL);
    g_signal_connect(name_factory, "bind",  G_CALLBACK(name_bind),  p);
    GtkColumnViewColumn *col_name = gtk_column_view_column_new("Name", name_factory);
    gtk_column_view_column_set_resizable(col_name, TRUE);
    gtk_column_view_column_set_expand(col_name, TRUE);
    gtk_column_view_column_set_fixed_width(col_name,
        fm->col_name_width > 0 ? fm->col_name_width : 250);
    GtkSorter *ns = GTK_SORTER(gtk_custom_sorter_new(name_sort_func, NULL, NULL));
    gtk_column_view_column_set_sorter(col_name, ns);
    g_object_unref(ns);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(p->column_view), col_name);

    /* Size column */
    GtkListItemFactory *size_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(size_factory, "setup", G_CALLBACK(size_setup), NULL);
    g_signal_connect(size_factory, "bind",  G_CALLBACK(size_bind),  p);
    GtkColumnViewColumn *col_size = gtk_column_view_column_new("Size", size_factory);
    gtk_column_view_column_set_resizable(col_size, TRUE);
    gtk_column_view_column_set_fixed_width(col_size,
        fm->col_size_width > 0 ? fm->col_size_width : 100);
    GtkSorter *ss = GTK_SORTER(gtk_custom_sorter_new(size_sort_func, NULL, NULL));
    gtk_column_view_column_set_sorter(col_size, ss);
    g_object_unref(ss);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(p->column_view), col_size);

    /* Date column */
    GtkListItemFactory *date_factory = gtk_signal_list_item_factory_new();
    g_signal_connect(date_factory, "setup", G_CALLBACK(date_setup), NULL);
    g_signal_connect(date_factory, "bind",  G_CALLBACK(date_bind),  p);
    GtkColumnViewColumn *col_date = gtk_column_view_column_new("Modified", date_factory);
    gtk_column_view_column_set_resizable(col_date, TRUE);
    gtk_column_view_column_set_fixed_width(col_date,
        fm->col_date_width > 0 ? fm->col_date_width : 145);
    GtkSorter *ds = GTK_SORTER(gtk_custom_sorter_new(date_sort_func, NULL, NULL));
    gtk_column_view_column_set_sorter(col_date, ds);
    g_object_unref(ds);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(p->column_view), col_date);

    /* Sort model: dirs-first (always) + column sort (respects asc/desc) */
    GtkSorter *cv_sorter = gtk_column_view_get_sorter(
                               GTK_COLUMN_VIEW(p->column_view));
    GtkMultiSorter *multi = gtk_multi_sorter_new();
    gtk_multi_sorter_append(multi,
        GTK_SORTER(gtk_custom_sorter_new(dirs_first_func, NULL, NULL)));
    gtk_multi_sorter_append(multi, g_object_ref(cv_sorter));
    p->sort_model = gtk_sort_list_model_new(
        g_object_ref(G_LIST_MODEL(p->store)),
        GTK_SORTER(multi));
    gtk_sort_list_model_set_incremental(p->sort_model, FALSE);

    /* Filter model (Ctrl+S) — between sort and selection */
    p->custom_filter = gtk_custom_filter_new(filter_match_func, p, NULL);
    p->filter_model = gtk_filter_list_model_new(
        g_object_ref(G_LIST_MODEL(p->sort_model)),
        GTK_FILTER(g_object_ref(p->custom_filter)));
    p->filter_text = NULL;

    p->selection = gtk_multi_selection_new(
        g_object_ref(G_LIST_MODEL(p->filter_model)));
    gtk_column_view_set_model(GTK_COLUMN_VIEW(p->column_view),
                              GTK_SELECTION_MODEL(p->selection));

    /* Default sort by name ascending */
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(p->column_view),
                                   col_name, GTK_SORT_ASCENDING);

    g_object_unref(col_name);
    g_object_unref(col_size);
    g_object_unref(col_date);

    /* Scrolled window */
    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), p->column_view);
    gtk_widget_set_vexpand(scroll, TRUE);

    /* status label */
    p->status_label = gtk_label_new("");
    gtk_widget_set_halign(p->status_label, GTK_ALIGN_START);
    gtk_widget_set_margin_start(p->status_label, 4);

    /* filter bar (Ctrl+S) — initially hidden */
    p->filter_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_margin_start(p->filter_bar, 4);
    gtk_widget_set_margin_end(p->filter_bar, 4);
    gtk_widget_set_margin_top(p->filter_bar, 2);
    gtk_widget_set_visible(p->filter_bar, FALSE);
    GtkWidget *filter_lbl = gtk_label_new("Filter:");
    p->filter_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(p->filter_entry), "type to filter…");
    gtk_widget_set_hexpand(p->filter_entry, TRUE);
    gtk_box_append(GTK_BOX(p->filter_bar), filter_lbl);
    gtk_box_append(GTK_BOX(p->filter_bar), p->filter_entry);

    g_signal_connect(p->filter_entry, "changed", G_CALLBACK(on_filter_changed), p);
    GtkEventController *filt_key = gtk_event_controller_key_new();
    g_signal_connect(filt_key, "key-pressed", G_CALLBACK(on_filter_key), p);
    gtk_widget_add_controller(p->filter_entry, filt_key);

    /* pack */
    gtk_box_append(GTK_BOX(p->frame), path_box);
    gtk_box_append(GTK_BOX(p->frame), scroll);
    gtk_box_append(GTK_BOX(p->frame), p->filter_bar);
    gtk_box_append(GTK_BOX(p->frame), p->status_label);

    /* signals */
    g_signal_connect(p->selection, "selection-changed",
                     G_CALLBACK(on_selection_changed), p);
    g_signal_connect(p->column_view, "activate", G_CALLBACK(on_cv_activate), p);
    g_signal_connect(p->path_entry, "activate", G_CALLBACK(on_path_activate), p);
    g_signal_connect(up_btn,            "clicked", G_CALLBACK(on_up_clicked), p);
    g_signal_connect(home_btn,          "clicked", G_CALLBACK(on_home_clicked), p);
    g_signal_connect(p->disconnect_btn, "clicked", G_CALLBACK(on_disconnect_clicked), p);
    g_signal_connect(p->terminal_btn, "clicked", G_CALLBACK(on_terminal_clicked), p);

    /* Key controller on column view (CAPTURE to intercept before default) */
    GtkEventController *key_ctrl = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect(key_ctrl, "key-pressed", G_CALLBACK(on_tree_key_press), p);
    gtk_widget_add_controller(p->column_view, key_ctrl);

    /* Key controller on path entry (for Enter/Escape) */
    GtkEventController *path_key = gtk_event_controller_key_new();
    g_signal_connect(path_key, "key-pressed", G_CALLBACK(on_path_key_press), p);
    gtk_widget_add_controller(p->path_entry, path_key);

    /* Focus controller */
    GtkEventController *focus_ctrl = gtk_event_controller_focus_new();
    g_signal_connect(focus_ctrl, "enter", G_CALLBACK(on_focus_enter), p);
    g_signal_connect(focus_ctrl, "leave", G_CALLBACK(on_focus_leave), p);
    gtk_widget_add_controller(p->column_view, focus_ctrl);

    gtk_widget_set_focusable(p->column_view, TRUE);
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Bottom toolbar – F-key buttons
 * ─────────────────────────────────────────────────────────────────── */

static void btn_copy(GtkButton *b, gpointer d)    { (void)b; fo_copy(d);   }
static void btn_move(GtkButton *b, gpointer d)    { (void)b; fo_move(d);   }
static void btn_mkdir(GtkButton *b, gpointer d)   { (void)b; fo_mkdir(d);  }
static void btn_delete(GtkButton *b, gpointer d)  { (void)b; fo_delete(d); }
static void btn_rename(GtkButton *b, gpointer d)  { (void)b; fo_rename(d); }
static void btn_find(GtkButton *b, gpointer d)    { (void)b; search_run(d); }
static void btn_view(GtkButton *b, gpointer d)    { (void)b; viewer_show(d); }
static void btn_editor(GtkButton *b, gpointer d)  { (void)b; editor_show(d); }
static void btn_extract(GtkButton *b, gpointer d) { (void)b; fo_extract(d); }
static void btn_pack(GtkButton *b, gpointer d)    { (void)b; fo_pack(d);  }
static void btn_ssh(GtkButton *b, gpointer d)     { (void)b; ssh_connect_dialog(d); }
static void btn_quit(GtkButton *b, gpointer d)    { (void)b; g_application_quit(G_APPLICATION(((FM*)d)->app)); }

static GtkWidget *create_toolbar(FM *fm)
{
    static const struct {
        const char *label;
        const char *tooltip;
        void (*cb)(GtkButton *, gpointer);
    } btns[] = {
        { "F5 Copy",    "Copy to other panel",      btn_copy    },
        { "F6 Move",    "Move to other panel",       btn_move    },
        { "F7 MkDir",   "Create new directory",      btn_mkdir   },
        { "F8 Delete",  "Delete selected files",     btn_delete  },
        { "F9 Rename",  "Rename current file",       btn_rename  },
        { "F2 Search",  "Search files",              btn_find    },
        { "F3 View",    "View file contents",        btn_view    },
        { "F4 Editor",  "Open file in editor",       btn_editor  },
        { "Extract",    "Extract archive (Alt+E)",   btn_extract },
        { "Pack",       "Create archive (Alt+P)",    btn_pack    },
        { "SSH",        "Connect via SSH/SFTP",      btn_ssh     },
        { "F10 Quit",   "Quit application",          btn_quit    },
    };

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_margin_top(bar, 2);
    gtk_widget_set_margin_bottom(bar, 2);

    for (gsize i = 0; i < G_N_ELEMENTS(btns); i++) {
        GtkWidget *btn = gtk_button_new_with_label(btns[i].label);
        gtk_widget_set_tooltip_text(btn, btns[i].tooltip);
        gtk_widget_set_focusable(btn, FALSE);
        gtk_widget_add_css_class(btn, "fm-fkey");
        gtk_widget_set_hexpand(btn, TRUE);
        g_signal_connect(btn, "clicked", G_CALLBACK(btns[i].cb), fm);
        gtk_box_append(GTK_BOX(bar), btn);
    }
    return bar;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Global key handler
 * ─────────────────────────────────────────────────────────────────── */

static gboolean on_window_key_press(GtkEventControllerKey *ctrl,
                                     guint keyval, guint keycode,
                                     GdkModifierType state, FM *fm)
{
    (void)ctrl; (void)keycode;

    /* Tab – switch active panel */
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        int next = fm->active ^ 1;
        /* Inhibit before grab_focus so GTK's internal row-0 auto-select
         * (fired before on_focus_enter) does not overwrite cursor_pos. */
        fm->panels[next].inhibit_sel = TRUE;
        gtk_widget_grab_focus(fm->panels[next].column_view);
        /* on_focus_enter resets inhibit_sel after its own scroll_to */
        return TRUE;
    }

    /* F-keys – only when not typing in path entry */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(fm->window));
    if (focus && GTK_IS_ENTRY(focus)) return FALSE;

    switch (keyval) {
    case GDK_KEY_F5:  fo_copy(fm);            return TRUE;
    case GDK_KEY_F6:  fo_move(fm);            return TRUE;
    case GDK_KEY_F7:  fo_mkdir(fm);           return TRUE;
    case GDK_KEY_F8:  fo_delete(fm);          return TRUE;
    case GDK_KEY_F9:  fo_rename(fm);          return TRUE;
    case GDK_KEY_F2:  search_run(fm);         return TRUE;
    case GDK_KEY_F3:  viewer_show(fm);        return TRUE;
    case GDK_KEY_F4:  editor_show(fm);        return TRUE;
    case GDK_KEY_F10: g_application_quit(G_APPLICATION(fm->app)); return TRUE;

    /* Alt+E – extract archive */
    case GDK_KEY_e:
    case GDK_KEY_E:
        if (state & GDK_ALT_MASK) { fo_extract(fm); return TRUE; }
        break;

    /* Alt+P – pack (create archive) */
    case GDK_KEY_p:
    case GDK_KEY_P:
        if (state & GDK_ALT_MASK) { fo_pack(fm); return TRUE; }
        break;

    /* Ctrl+S – filter panel */
    case GDK_KEY_s:
    case GDK_KEY_S:
        if (state & GDK_CONTROL_MASK) {
            panel_show_filter(active_panel(fm));
            return TRUE;
        }
        break;

    /* Ctrl+R – reload both panels */
    case GDK_KEY_r:
    case GDK_KEY_R:
        if (state & GDK_CONTROL_MASK) {
            panel_reload(&fm->panels[0]);
            panel_reload(&fm->panels[1]);
            fm_status(fm, "Panels refreshed");
            return TRUE;
        }
        break;

    /* Ctrl+H – toggle hidden files */
    case GDK_KEY_h:
    case GDK_KEY_H:
        if (state & GDK_CONTROL_MASK) {
            fm->show_hidden = !fm->show_hidden;
            settings_save(fm);
            panel_reload(&fm->panels[0]);
            panel_reload(&fm->panels[1]);
            fm_status(fm, fm->show_hidden ? "Hidden files: shown" : "Hidden files: hidden");
            return TRUE;
        }
        break;

    /* Ctrl+= – synchronize panels */
    case GDK_KEY_equal:
        if (state & GDK_CONTROL_MASK) {
            Panel *o = other_panel(fm);
            panel_load(o, active_panel(fm)->cwd);
            fm_status(fm, "Panels synchronized");
            return TRUE;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Application activate
 * ─────────────────────────────────────────────────────────────────── */

/* ── Hamburger menu action callbacks ──────────────────────────────── */

static void menu_extract(GSimpleAction *a, GVariant *v, gpointer d)
    { (void)a; (void)v; fo_extract(d); }
static void menu_pack(GSimpleAction *a, GVariant *v, gpointer d)
    { (void)a; (void)v; fo_pack(d); }
static void menu_filter(GSimpleAction *a, GVariant *v, gpointer d)
    { (void)a; (void)v; panel_show_filter(active_panel(d)); }
static void menu_ssh(GSimpleAction *a, GVariant *v, gpointer d)
    { (void)a; (void)v; ssh_connect_dialog(d); }

/* ─────────────────────────────────────────────────────────────────── */

static void on_activate(GtkApplication *app, gpointer data)
{
    FM *fm = data;
    fm->app = app;

    /* CSS – app styles */
    fm->css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    /* CSS – font (managed by settings, higher priority) */
    fm->font_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->font_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);

    /* CSS – editor font (highest priority, targets textview.fm-editor) */
    fm->editor_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->editor_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

    /* CSS – viewer font (targets textview.fm-viewer) */
    fm->viewer_css_provider = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
        GTK_STYLE_PROVIDER(fm->viewer_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);

    /* load settings (applies font CSS) */
    settings_load(fm);

    /* window */
    fm->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(fm->window), APP_NAME " – File Manager");
    gtk_window_set_default_size(GTK_WINDOW(fm->window), 1200, 700);

    /* header bar */
    GtkWidget *hbar = gtk_header_bar_new();
    gtk_header_bar_set_show_title_buttons(GTK_HEADER_BAR(hbar), TRUE);

    /* reload button in header */
    GtkWidget *reload_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(reload_btn, "Refresh both panels (Ctrl+R)");
    g_signal_connect_swapped(reload_btn, "clicked", G_CALLBACK(panel_reload), &fm->panels[0]);
    g_signal_connect_swapped(reload_btn, "clicked", G_CALLBACK(panel_reload), &fm->panels[1]);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), reload_btn);

    /* settings gear button in header */
    GtkWidget *settings_btn = gtk_button_new_from_icon_name("preferences-system-symbolic");
    gtk_widget_set_tooltip_text(settings_btn, "Settings");
    g_signal_connect_swapped(settings_btn, "clicked",
        G_CALLBACK(settings_dialog), fm);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), settings_btn);

    /* ── Hamburger menu ── */
    {
        GSimpleActionGroup *win_actions = g_simple_action_group_new();

        GSimpleAction *a_extract = g_simple_action_new("extract", NULL);
        g_signal_connect(a_extract, "activate", G_CALLBACK(menu_extract), fm);
        g_action_map_add_action(G_ACTION_MAP(win_actions), G_ACTION(a_extract));

        GSimpleAction *a_pack = g_simple_action_new("pack", NULL);
        g_signal_connect(a_pack, "activate", G_CALLBACK(menu_pack), fm);
        g_action_map_add_action(G_ACTION_MAP(win_actions), G_ACTION(a_pack));

        GSimpleAction *a_filter = g_simple_action_new("filter", NULL);
        g_signal_connect(a_filter, "activate", G_CALLBACK(menu_filter), fm);
        g_action_map_add_action(G_ACTION_MAP(win_actions), G_ACTION(a_filter));

        GSimpleAction *a_ssh = g_simple_action_new("ssh", NULL);
        g_signal_connect(a_ssh, "activate", G_CALLBACK(menu_ssh), fm);
        g_action_map_add_action(G_ACTION_MAP(win_actions), G_ACTION(a_ssh));

        gtk_widget_insert_action_group(fm->window, "fm", G_ACTION_GROUP(win_actions));

        GMenu *menu = g_menu_new();

        GMenu *archive_section = g_menu_new();
        g_menu_append(archive_section, "Extract archive…    Alt+E", "fm.extract");
        g_menu_append(archive_section, "Create archive…     Alt+P", "fm.pack");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(archive_section));

        GMenu *tools_section = g_menu_new();
        g_menu_append(tools_section, "Filter files…        Ctrl+S", "fm.filter");
        g_menu_append(tools_section, "SSH / SFTP…", "fm.ssh");
        g_menu_append_section(menu, NULL, G_MENU_MODEL(tools_section));

        GtkWidget *hamburger = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(hamburger), "open-menu-symbolic");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(hamburger), G_MENU_MODEL(menu));
        gtk_widget_set_tooltip_text(hamburger, "Menu");
        gtk_header_bar_pack_end(GTK_HEADER_BAR(hbar), hamburger);
    }

    gtk_window_set_titlebar(GTK_WINDOW(fm->window), hbar);

    /* main layout */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(fm->window), vbox);

    /* paned */
    fm->paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_wide_handle(GTK_PANED(fm->paned), TRUE);

    panel_setup(&fm->panels[0], fm, 0);
    panel_setup(&fm->panels[1], fm, 1);

    gtk_paned_set_start_child(GTK_PANED(fm->paned), fm->panels[0].frame);
    gtk_paned_set_end_child(GTK_PANED(fm->paned), fm->panels[1].frame);
    gtk_paned_set_resize_start_child(GTK_PANED(fm->paned), TRUE);
    gtk_paned_set_resize_end_child(GTK_PANED(fm->paned), TRUE);

    /* toolbar */
    GtkWidget *toolbar = create_toolbar(fm);

    /* statusbar (GtkLabel) */
    fm->statusbar = gtk_label_new("");
    gtk_widget_set_halign(fm->statusbar, GTK_ALIGN_START);
    gtk_widget_set_margin_start(fm->statusbar, 4);
    gtk_widget_set_margin_top(fm->statusbar, 2);
    gtk_widget_set_margin_bottom(fm->statusbar, 2);

    gtk_widget_set_vexpand(fm->paned, TRUE);
    gtk_box_append(GTK_BOX(vbox), fm->paned);
    gtk_box_append(GTK_BOX(vbox), toolbar);
    gtk_box_append(GTK_BOX(vbox), fm->statusbar);

    /* global key handler (CAPTURE phase for Tab + F-keys) */
    GtkEventController *win_key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(win_key, GTK_PHASE_CAPTURE);
    g_signal_connect(win_key, "key-pressed", G_CALLBACK(on_window_key_press), fm);
    gtk_widget_add_controller(fm->window, win_key);

    /* load directories */
    const char *home = g_get_home_dir();
    panel_load(&fm->panels[0], home);
    panel_load(&fm->panels[1], home);

    /* set initial active panel – deselect the inactive one */
    set_active_panel(fm, 0);
    fm->panels[1].inhibit_sel = TRUE;
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(fm->panels[1].selection));
    fm->panels[1].inhibit_sel = FALSE;

    gtk_window_present(GTK_WINDOW(fm->window));
    gtk_widget_grab_focus(fm->panels[0].column_view);

    fm_status(fm, "Welcome to " APP_NAME " – File Manager");
}

/* ─────────────────────────────────────────────────────────────────── *
 *  main
 * ─────────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");
    memset(&g_fm, 0, sizeof(g_fm));

    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(on_activate), &g_fm);

    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
