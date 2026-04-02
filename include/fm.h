#ifndef FM_H
#define FM_H

#include <gtk/gtk.h>
#ifdef HAVE_LIBADWAITA
#  include <adwaita.h>
#endif
#include <sys/stat.h>
#include <dirent.h>
#ifdef HAVE_LIBSSH2
#  include <libssh2.h>
#  include <libssh2_sftp.h>
#endif

#define APP_ID   "sk.km.fm"
#define APP_NAME "FM"

/* ── FileItem GObject – replaces GtkListStore columns ────────────── */

#define FILE_ITEM_TYPE (file_item_get_type())
G_DECLARE_FINAL_TYPE(FileItem, file_item, FILE, ITEM, GObject)

struct _FileItem {
    GObject   parent_instance;
    gchar    *icon_name;
    gchar    *name;
    gchar    *size;
    gchar    *date;
    gboolean  is_dir;
    gchar    *fg_color;    /* CSS colour or NULL              */
    gint      weight;      /* PangoWeight                     */
    gint64    size_raw;    /* for sorting                     */
    gint64    date_raw;    /* for sorting                     */
    gboolean  marked;     /* user-toggled mark for ops       */
    gchar    *dir_path;   /* search results: directory containing this file (NULL otherwise) */
};

FileItem *file_item_new(const char *icon_name, const char *name,
                        const char *size, const char *date,
                        gboolean is_dir, const char *fg_color,
                        gint weight, gint64 size_raw, gint64 date_raw);

/* Optimized version: takes ownership of size and date (caller must not free) */
FileItem *file_item_new_take(const char *icon_name, const char *name,
                             gchar *size, gchar *date,
                             gboolean is_dir, const char *fg_color,
                             gint weight, gint64 size_raw, gint64 date_raw);

/* ── SSH saved connection (bookmark) ─────────────────────────────── */
typedef struct {
    gchar *name;      /* display label                */
    gchar *user;
    gchar *host;
    int    port;
    gchar *key_path;  /* path to private key (NULL = default ~/.ssh/id_rsa) */
} SshBookmark;

/* ── SSH connection state ─────────────────────────────────────────── */
typedef struct _SshConn SshConn;
#ifdef HAVE_LIBSSH2
struct _SshConn {
    int               sock;
    LIBSSH2_SESSION  *session;
    LIBSSH2_SFTP     *sftp;
    gchar            *host;
    gchar            *user;
    gchar            *remote_path;   /* current remote cwd */
    int               port;
};
#else
struct _SshConn {
    int   sock;
    void *session;
    void *sftp;
    gchar *host;
    gchar *user;
    gchar *remote_path;
    int   port;
};
#endif

typedef struct _FM FM;

typedef struct {
    GtkWidget          *frame;          /* GtkBox vertical – outer container  */
    GtkWidget          *path_entry;     /* GtkEntry – current path            */
    GtkWidget          *column_view;    /* GtkColumnView                      */
    GListStore         *store;          /* GListStore<FileItem>               */
    GtkSortListModel   *sort_model;     /* wraps store with sorters           */
    GtkSorter          *default_sorter; /* saved for restoring after search    */
    GtkMultiSelection  *selection;      /* selection model for column view    */
    GtkWidget          *status_label;   /* selection / item count             */
    GtkWidget          *disconnect_btn; /* shown when ssh_conn active         */
    GtkWidget          *terminal_btn;   /* SSH terminal – shown when connected */
    char                cwd[4096];
    FM                 *fm;
    int                 idx;            /* 0 = left, 1 = right                */
    SshConn            *ssh_conn;       /* NULL = local panel                 */
    guint               cursor_pos;     /* current cursor position            */
    gboolean            inhibit_sel;    /* TRUE while we scroll/unselect programmatically */
    GHashTable         *marks;          /* set of marked filenames (survives reload) */
    /* filter (Ctrl+S) */
    GtkWidget          *filter_bar;    /* GtkBox with entry, hidden by default */
    GtkWidget          *filter_entry;
    GtkFilterListModel *filter_model;  /* sits between sort_model and selection */
    GtkCustomFilter    *custom_filter;
    gchar              *filter_text;   /* current filter string (lowercase)    */
    gchar              *mask_pattern;  /* glob mask (e.g. "*.c"), NULL = off   */
    /* directory monitor (auto-refresh) */
    GFileMonitor       *dir_monitor;   /* local: inotify watcher             */
    guint               poll_id;       /* SFTP: periodic poll timer (0=off)  */
    /* search results mode */
    gboolean            search_mode;   /* TRUE when panel shows search results */
    char                search_prev_cwd[4096]; /* cwd to restore on leaving search */
    GtkWidget          *search_btn;    /* "exit search" button – hidden by default */
} Panel;

struct _FM {
    GtkApplication *app;
    GtkWidget      *window;
    GtkWidget      *paned;
    Panel           panels[2];
    int             active;     /* 0 = left, 1 = right                */
    GtkWidget      *statusbar;  /* GtkLabel                           */
    /* CSS providers */
    GtkCssProvider *css_provider;        /* static app CSS              */
    GtkCssProvider *font_css_provider;   /* panel font – reloaded       */
    GtkCssProvider *editor_css_provider; /* textview.fm-editor font     */
    GtkCssProvider *viewer_css_provider; /* textview.fm-viewer font     */
    /* panel font settings */
    gchar          *font_family;
    int             font_size;
    /* GUI font settings */
    gchar          *gui_font_family;
    int             gui_font_size;
    /* column widths */
    int             col_name_width;
    int             col_size_width;
    int             col_date_width;
    /* display options */
    gboolean        show_hidden;
    gboolean        show_hover;     /* show row hover highlight */
    int             icon_size;          /* pixel size for file icons (16–48) */
    /* application settings */
    gchar          *terminal_app;  /* terminal emulator command */
    /* cursor style */
    gchar          *cursor_color;  /* hex color, e.g. "#1A73E8" */
    gboolean        cursor_outline; /* TRUE = outline only, FALSE = filled */
    /* viewer settings */
    gchar          *viewer_font_family;
    int             viewer_font_size;
    /* editor settings */
    gchar          *editor_font_family;    /* text content font              */
    int             editor_font_size;
    gchar          *editor_gui_font_family; /* editor window GUI font         */
    int             editor_gui_font_size;
    gboolean        editor_line_numbers;
    int             editor_linenum_font_size; /* line number gutter font size */
    gboolean        syntax_highlight;         /* editor syntax highlighting   */
    gboolean        viewer_syntax_highlight;  /* viewer syntax highlighting   */
    gboolean        viewer_line_numbers;      /* viewer line numbers          */
    gchar          *editor_style_scheme;      /* GtkSourceView style scheme id */
    /* directory appearance */
    gchar          *dir_color;                /* hex color for directories      */
    gboolean        dir_bold;                 /* bold font for directories      */
    /* mark color */
    gchar          *mark_color;               /* hex color for marked items     */
    /* auto-refresh */
    gboolean        auto_refresh;             /* watch directories for changes  */
    /* theme: 0 = dark, 1 = light */
    int             theme;
    /* viewer: show images as pictures instead of raw bytes */
    gboolean        viewer_image_preview;
    /* cached theme-adjusted mark color (avoids alloc in bind callbacks) */
    gchar          *vis_mark_color;
};

/* ── Dialog helper (replaces gtk_dialog_run with GMainLoop) ───────── */

typedef struct {
    GMainLoop *loop;
    int        response;
} DlgCtx;

static inline void dlg_ctx_init(DlgCtx *ctx) {
    ctx->loop = g_main_loop_new(NULL, FALSE);
    ctx->response = 0;
}

static inline void dlg_ctx_ok(GtkButton *b, DlgCtx *ctx) {
    (void)b; ctx->response = 1; g_main_loop_quit(ctx->loop);
}

static inline void dlg_ctx_cancel(GtkButton *b, DlgCtx *ctx) {
    (void)b; ctx->response = 0; g_main_loop_quit(ctx->loop);
}

static inline gboolean dlg_ctx_close(GtkWindow *w, DlgCtx *ctx) {
    (void)w; ctx->response = 0; g_main_loop_quit(ctx->loop);
    return TRUE;   /* prevent auto-close; caller destroys */
}

/* Also usable as "activate" on entry → same as OK */
static inline void dlg_ctx_entry_ok(GtkEntry *e, DlgCtx *ctx) {
    (void)e; ctx->response = 1; g_main_loop_quit(ctx->loop);
}

static inline int dlg_ctx_run(DlgCtx *ctx) {
    g_main_loop_run(ctx->loop);
    int r = ctx->response;
    g_main_loop_unref(ctx->loop);
    return r;
}

/* ── panel API ────────────────────────────────────────────────────── */
void   panel_setup(Panel *p, FM *fm, int idx);
void   panel_load(Panel *p, const char *path);
void   panel_load_remote(Panel *p, const char *path);
void   panel_reload(Panel *p);
void   panel_go_up(Panel *p);
gchar *panel_cursor_name(Panel *p);          /* caller must g_free()  */
gchar *panel_cursor_fullpath(Panel *p);     /* full path (search-aware), caller must g_free() */
GList *panel_selection(Panel *p);            /* g_list_free_full(l, g_free) */
void   panel_update_status(Panel *p);

/* ── directory monitoring ─────────────────────────────────────────── */
void panel_monitor_start(Panel *p);
void panel_monitor_stop(Panel *p);

/* ── fileops ──────────────────────────────────────────────────────── */
void fo_copy(FM *fm);
void fo_move(FM *fm);
void fo_delete(FM *fm);
void fo_mkdir(FM *fm);
void fo_rename(FM *fm);
void fo_extract(FM *fm);
void fo_pack(FM *fm);

/* ── search ───────────────────────────────────────────────────────── */
void search_run(FM *fm);

/* ── viewer ───────────────────────────────────────────────────────── */
void viewer_show(FM *fm);

/* ── editor ───────────────────────────────────────────────────────── */
void editor_show(FM *fm);

/* ── settings ─────────────────────────────────────────────────────── */
void   settings_load(FM *fm);
void   settings_save(FM *fm);
void   settings_dialog(FM *fm);
void   settings_apply_columns(FM *fm);
void   apply_editor_css(FM *fm);
void   apply_viewer_css(FM *fm);
const char *fm_link_color(FM *fm);
const char *fm_exec_color(FM *fm);
const char *fm_scheme_for_theme(FM *fm);
gchar      *fm_visible_color(FM *fm, const char *hex);  /* caller frees */
void   highlight_apply(GtkTextBuffer *buf, const char *filepath, gboolean enabled);
#ifdef HAVE_GTKSOURCEVIEW
#include <gtksourceview/gtksource.h>
GtkSourceLanguage *source_guess_language(const char *filename);
#endif
void   ssh_bookmark_free            (gpointer p);
GList *settings_load_ssh_bookmarks  (FM *fm);          /* GList<SshBookmark*> */
void   settings_save_ssh_bookmarks  (FM *fm, GList *list);

/* ── ssh ──────────────────────────────────────────────────────────── */
void ssh_connect_dialog(FM *fm);
void ssh_panel_close(Panel *p);
gchar    *ssh_read_file (SshConn *conn, const char *path, gsize *out_len);
gboolean  ssh_write_file(SshConn *conn, const char *path, const gchar *data, gsize len);

/* ── shared callbacks ─────────────────────────────────────────────── */
/* Folder-chooser button – reused by fileops and search */
void on_browse_clicked(GtkButton *btn, gpointer entry_widget);

/* ── helpers ──────────────────────────────────────────────────────── */
static inline Panel *active_panel(FM *fm) { return &fm->panels[fm->active];       }
static inline Panel *other_panel(FM *fm)  { return &fm->panels[fm->active ^ 1];   }

gchar *fmt_size(goffset sz);
gchar *fmt_date(gint64 t);
void   fm_status(FM *fm, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
const char *icon_for_entry(const char *name, gboolean is_dir,
                           gboolean is_link, gboolean is_exec);

#endif /* FM_H */
