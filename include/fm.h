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
#include "util.h"
#include "fsops.h"

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
    gchar    *snippet;    /* content search: first matching line (NULL otherwise) */
    gint      line;       /* content search: 1-based line of the snippet, 0 = none */
    gchar    *sort_key;   /* lazily built collation key (see file_item_sort_key) */
};

/* Collation key for name sorting, computed once per item (main thread) */
const gchar *file_item_sort_key(FileItem *item);
/* Change the mark and emit "mark-changed" so bound rows re-render in place */
void file_item_set_marked(FileItem *item, gboolean marked);
FileItem *file_item_copy(FileItem *src);   /* unmarked duplicate */

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
    gchar *key_path;  /* private key (NULL = ssh-agent + ~/.ssh/id_{ed25519,ecdsa,rsa}) */
} SshBookmark;

/* ── SSH connection state ─────────────────────────────────────────── *
 * libssh2 sessions are not thread-safe: whoever talks to the server
 * holds `lock`.  File-operation jobs hold it for their whole run, the
 * main thread only try-locks (poll, viewer, editor) and backs off.   */
typedef struct _SshConn SshConn;
#ifdef HAVE_LIBSSH2
struct _SshConn {
    GRecMutex         lock;
    int               sock;
    LIBSSH2_SESSION  *session;
    LIBSSH2_SFTP     *sftp;
    gchar            *host;
    gchar            *user;
    gchar            *remote_path;   /* current remote cwd */
    int               port;
    guint             serial;        /* unique per connection (editor checks it) */
};
#else
struct _SshConn {
    GRecMutex lock;
    int   sock;
    void *session;
    void *sftp;
    gchar *host;
    gchar *user;
    gchar *remote_path;
    guint  serial;
    int   port;
};
#endif

typedef struct _FM FM;

/* Panel tab: a remembered local directory; the panel widgets are shared */
typedef struct {
    gchar *path;
    gchar *cursor_name;   /* item under the cursor when the tab was left */
} PanelTab;

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
    GHashTable         *scroll_pos;     /* dir → scroll offset, restored on going up */
    GHashTable         *marks;          /* marked names; full paths in search mode (see sel_mark_key) */
    gboolean            busy;           /* a search scans into this panel – no navigation */
    /* filter (Ctrl+S) */
    GtkWidget          *filter_bar;    /* GtkBox with entry, hidden by default */
    GtkWidget          *filter_entry;
    GtkFilterListModel *filter_model;  /* sits between sort_model and selection */
    GtkCustomFilter    *custom_filter;
    gchar              *filter_text;   /* current filter string (lowercase)    */
    gchar              *mask_pattern;  /* glob mask (e.g. "*.c"), NULL = off   */
    /* directory monitor (auto-refresh) */
    GFileMonitor       *dir_monitor;   /* local: inotify watcher             */
    GHashTable         *mon_pending;   /* names changed since the last flush */
    guint               mon_flush_id;  /* batches monitor events (150 ms)    */
    guint               poll_id;       /* SFTP: periodic poll timer (0=off)  */
    GThread            *poll_thread;   /* SFTP: listing in progress          */
    guint               poll_gen;      /* bumped on stop – drops stale lists */
    /* search results mode */
    gboolean            search_mode;   /* TRUE when panel shows search results */
    char                search_prev_cwd[4096]; /* cwd to restore on leaving search */
    gchar              *search_prev_name;      /* cursor item there (owned)        */
    GtkWidget          *search_btn;    /* "exit search" button – hidden by default */
    GtkWidget          *results_btn;   /* "back to search results" (Alt+Left)  */
    gpointer            cur_search;    /* SearchHist* shown in the panel        */
    gpointer            last_search;   /* SearchHist* to return to after a jump */
    /* tabs (Ctrl+T / Ctrl+W / Ctrl+Tab) */
    GPtrArray          *tabs;          /* PanelTab*, always at least one     */
    guint               tab_active;    /* index into tabs                    */
    GtkWidget          *tab_scroll;    /* tab strip – shown with 2+ tabs     */
    GtkWidget          *tab_box;
    /* archive browsing (arcpanel.c) – cwd stays the archive's directory */
    gchar              *arc_file;      /* local archive path, NULL = not in one */
    gchar              *arc_dir;       /* inner directory, "" = archive root  */
    GPtrArray          *arc_entries;   /* ArcEntry* table of contents         */
    gint64              arc_stamp;     /* archive mtime/size when listed      */
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
    /* theme: 0 = dark, 1 = light, 2 = follow the system (see fm_dark) */
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
void   panel_scroll_remember(Panel *p);       /* before leaving the current dir */
void   panel_go_up(Panel *p);
void   panel_tab_new(Panel *p, const char *path); /* NULL = current dir */
void   panel_tab_close(Panel *p);
void   panel_tab_cycle(Panel *p, int delta);
void   panel_tabs_restore(Panel *p, gchar **paths, int active);
void   panel_tabs_free(Panel *p);
gchar *panel_cursor_name(Panel *p);          /* caller must g_free()  */
gchar *panel_cursor_fullpath(Panel *p);     /* full path (search-aware), caller must g_free() */
GList *panel_selection(Panel *p);            /* names; g_list_free_full(l, g_free) */
GList *panel_selection_paths(Panel *p);      /* full local/remote paths, search-aware */
const char *panel_dir(Panel *p);             /* cwd or remote cwd */
void   panel_update_status(Panel *p);
void   fm_refocus_if_lost(FM *fm);                  /* after a modal window closed */
void   panel_focus(Panel *p);                      /* make active + keyboard cursor */
void   panel_select_name(Panel *p, const char *name);  /* move cursor to item */
FileItem *file_item_from_stat(FM *fm, const char *name, const struct stat *st);
/* Run fn(arg) in a worker; after 150 ms a modal "Reading <what>…" window
 * with Cancel appears.  Cancelled → NULL + *cancelled, cancel(arg) (if set)
 * asks the worker to stop, its result is then dropped with free_result;
 * arg is freed with free_arg at the end. */
gpointer fm_wait_work(FM *fm, const char *title, const char *what,
                      GThreadFunc fn, gpointer arg, GDestroyNotify free_arg,
                      GDestroyNotify free_result, void (*cancel)(gpointer arg),
                      gboolean *cancelled);

/* ── archives as directories (arcpanel.c, read-only) ──────────────── */
gboolean arc_panel_enter(Panel *p, const char *archive); /* FALSE = not browsable */
void     arc_panel_leave(Panel *p);          /* forget archive state (panel_load) */
void     arc_panel_activate(Panel *p);       /* Enter inside an archive          */
void     arc_panel_go_up(Panel *p);          /* ".." / Backspace                 */
void     arc_panel_reload(Panel *p);         /* re-list the archive              */
gboolean arc_panel_readonly_msg(Panel *p);   /* TRUE (+ status) when in an archive */
void     arc_panel_copy_out(FM *fm);         /* F5 – extract the selection       */
void     arc_panel_open(FM *fm, int how);    /* temp copy: 0 default app, 1 F3, 2 F4 */
void     arc_panel_cleanup(void);            /* exit: remove the temp copies     */

/* ── marking (selection.c) ────────────────────────────────────────── */
void sel_toggle_cursor(Panel *p, gboolean advance);
void sel_mark_all(Panel *p, gboolean mark);
void sel_invert(Panel *p);
void sel_group_dialog(FM *fm, gboolean mark);   /* Num+ / Num- */
gchar *sel_mark_key(Panel *p, FileItem *it);   /* key into Panel.marks, g_free() */
void sel_refresh(Panel *p);                     /* re-render after mark change */

/* ── directory monitoring ─────────────────────────────────────────── */
void panel_monitor_start(Panel *p);
void panel_monitor_stop(Panel *p);

/* ── fileops ──────────────────────────────────────────────────────── */
void fo_copy(FM *fm);
void fo_copy_here(FM *fm);                   /* Shift+F5 – duplicate in same dir */
void fo_move(FM *fm);
void fo_multi_rename(FM *fm);              /* Ctrl+M (multirename.c) */
void fo_delete(FM *fm);                      /* local: trash (asks), SFTP: delete */
void fo_delete_permanent(FM *fm);            /* Shift+Del / Shift+F8 */
void fo_mkdir(FM *fm);
void fo_newfile(FM *fm);                     /* Shift+F4 */
void fo_rename(FM *fm);
void fo_properties(FM *fm);                  /* Alt+Enter */
void fo_extract(FM *fm);
void fo_pack(FM *fm);
/* Generic transfer: `paths` are full source paths (on sconn if non-NULL) */
int  fo_transfer(FM *fm, GList *paths, SshConn *sconn,   /* → worst FS_* */
                 const char *dest_dir, SshConn *dconn, gboolean move);

/* ── clipboard (clipboard.c) ──────────────────────────────────────── */
void clip_copy(FM *fm, gboolean cut);        /* Ctrl+C / Ctrl+X */
void clip_paste(FM *fm);                     /* Ctrl+V */
void clip_copy_paths(FM *fm);                /* Ctrl+Shift+C – paths as text */

/* ── jobs (job.c) ─────────────────────────────────────────────────── */
/* Run fn(arg) on the main thread and wait for the result (worker side) */
gpointer fm_call_on_main(gpointer (*fn)(gpointer), gpointer arg);

/* ── search ───────────────────────────────────────────────────────── */
void search_run(FM *fm);
void search_detach(Panel *p);              /* leaving results: remember them   */
gboolean search_restore(Panel *p);         /* Alt+Left – back to last results  */
void search_goto_file(Panel *p);           /* Ctrl+PgDn – jump to file's dir   */
void search_history_dialog(FM *fm);        /* Shift+F2 – earlier searches      */
const char *search_needle(Panel *p);       /* text searched for, or NULL       */
void search_update_buttons(Panel *p);

/* ── viewer ───────────────────────────────────────────────────────── */
void viewer_show(FM *fm);
void viewer_show_path(FM *fm, const char *path, const char *name); /* local file */

/* ── editor ───────────────────────────────────────────────────────── */
void editor_show(FM *fm);
void editor_show_path(FM *fm, const char *path);   /* local file */

/* ── settings ─────────────────────────────────────────────────────── */
void   settings_load(FM *fm);
void   settings_save(FM *fm);
void   settings_save_tabs(FM *fm);
gchar **settings_load_tabs(int idx, int *active);   /* g_strfreev() */
void   settings_dialog(FM *fm);
void   settings_apply_columns(FM *fm);
void   apply_editor_css(FM *fm);
void   apply_viewer_css(FM *fm);
const char *fm_link_color(FM *fm);
const char *fm_exec_color(FM *fm);
const char *fm_scheme_for_theme(FM *fm);
gboolean    fm_dark(FM *fm);                 /* effective appearance is dark */
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
gboolean  ssh_conn_trylock(SshConn *conn);   /* main thread: FALSE = job busy */
void      ssh_conn_lock   (SshConn *conn);
void      ssh_conn_unlock (SshConn *conn);
gchar    *ssh_read_file (SshConn *conn, const char *path, gsize *out_len);
gboolean  ssh_write_file(SshConn *conn, const char *path, const gchar *data, gsize len);

/* ── shared callbacks ─────────────────────────────────────────────── */
/* Folder-chooser button – reused by fileops and search */
void on_browse_clicked(GtkButton *btn, gpointer entry_widget);

/* ── helpers ──────────────────────────────────────────────────────── */
static inline Panel *active_panel(FM *fm) { return &fm->panels[fm->active];       }
static inline Panel *other_panel(FM *fm)  { return &fm->panels[fm->active ^ 1];   }

void   fm_status(FM *fm, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
const char *icon_for_entry(const char *name, gboolean is_dir,
                           gboolean is_link, gboolean is_exec);

#endif /* FM_H */
