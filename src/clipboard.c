#include "fm.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  System clipboard – interoperable with Nautilus/Nemo/Dolphin/Thunar:
 *    x-special/gnome-copied-files  "copy|cut\nfile:///…"
 *    application/x-kde-cutselection "1" for cut
 *    text/uri-list                 "file:///…\r\n"
 *    text/plain                    paths, one per line
 * ─────────────────────────────────────────────────────────────────── */

static GdkClipboard *clipboard(FM *fm)
{
    return gtk_widget_get_clipboard(fm->window);
}

/* SFTP items cannot go through the system clipboard as file:// URIs –
 * keep them here; the system clipboard gets sftp:// text, and the remote
 * copy stays valid only while that text is still our clipboard content. */
static struct {
    guint               serial;     /* SshConn->serial of the source */
    GList              *paths;      /* remote paths */
    gboolean            cut;
    GdkContentProvider *owner;      /* weak: content we set */
} rclip;

static void rclip_clear(void)
{
    g_list_free_full(rclip.paths, g_free);
    rclip.paths = NULL;
    rclip.serial = 0;
    if (rclip.owner)
        g_object_remove_weak_pointer(G_OBJECT(rclip.owner), (gpointer *)&rclip.owner);
    rclip.owner = NULL;
}

/* the source connection, if some tab still uses it */
static SshConn *rclip_conn(FM *fm)
{
    for (int i = 0; i < 2; i++) {
        Panel *p = &fm->panels[i];
        if (p->ssh_conn && p->ssh_conn->serial == rclip.serial) return p->ssh_conn;
        for (guint j = 0; p->tabs && j < p->tabs->len; j++) {
            PanelTab *t = g_ptr_array_index(p->tabs, j);
            if (t->conn && t->conn->serial == rclip.serial) return t->conn;
        }
    }
    return NULL;
}

static gpointer dup_str(gconstpointer s, gpointer u) { (void)u; return g_strdup(s); }

static void clip_copy_remote(FM *fm, Panel *p, gboolean cut)
{
    GList *paths = panel_selection_paths(p);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }
    rclip_clear();
    GString *s = g_string_new(NULL);
    int n = 0;
    for (GList *l = paths; l; l = l->next, n++) {
        if (s->len) g_string_append_c(s, '\n');
        g_string_append_printf(s, "sftp://%s@%s%s", p->ssh_conn->user,
                               p->ssh_conn->host, (const char *)l->data);
    }
    GdkContentProvider *cp = gdk_content_provider_new_typed(G_TYPE_STRING, s->str);
    gdk_clipboard_set_content(clipboard(fm), cp);
    rclip.serial = p->ssh_conn->serial;
    rclip.paths  = paths;
    rclip.cut    = cut;
    /* weak pointer: NULLed when another app takes the clipboard and the
     * provider dies, so a reused address can never match */
    rclip.owner  = cp;
    g_object_add_weak_pointer(G_OBJECT(cp), (gpointer *)&rclip.owner);
    g_object_unref(cp);
    g_string_free(s, TRUE);
    fm_status(fm, "%s %d item%s to clipboard", cut ? "Cut" : "Copied",
              n, n == 1 ? "" : "s");
}

void clip_copy(FM *fm, gboolean cut)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;      /* F5 extracts instead */
    if (p->ssh_conn) { clip_copy_remote(fm, p, cut); return; }
    GList *paths = panel_selection_paths(p);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }

    GString *gnome = g_string_new(cut ? "cut" : "copy");
    GString *uris  = g_string_new(NULL);
    GString *text  = g_string_new(NULL);
    int n = 0;
    for (GList *l = paths; l; l = l->next) {
        gchar *uri = g_filename_to_uri(l->data, NULL, NULL);
        if (!uri) continue;
        g_string_append_printf(gnome, "\n%s", uri);
        g_string_append_printf(uris, "%s\r\n", uri);
        g_string_append_printf(text, "%s%s", n ? "\n" : "", (const char *)l->data);
        g_free(uri);
        n++;
    }

    GBytes *b_gnome = g_string_free_to_bytes(gnome);
    GBytes *b_uris  = g_string_free_to_bytes(uris);
    GBytes *b_kde   = g_bytes_new_static(cut ? "1" : "0", 1);
    gchar  *s_text  = g_string_free(text, FALSE);

    GdkContentProvider *provs[] = {
        gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", b_gnome),
        gdk_content_provider_new_for_bytes("application/x-kde-cutselection", b_kde),
        gdk_content_provider_new_for_bytes("text/uri-list", b_uris),
        gdk_content_provider_new_typed(G_TYPE_STRING, s_text),
    };
    GdkContentProvider *u = gdk_content_provider_new_union(provs, G_N_ELEMENTS(provs));
    rclip_clear();
    gdk_clipboard_set_content(clipboard(fm), u);
    g_object_unref(u);
    g_bytes_unref(b_gnome); g_bytes_unref(b_uris); g_bytes_unref(b_kde);
    g_free(s_text);

    fm_status(fm, "%s %d item%s to clipboard", cut ? "Cut" : "Copied",
              n, n == 1 ? "" : "s");
    g_list_free_full(paths, g_free);
}

void clip_copy_paths(FM *fm)
{
    Panel *p = active_panel(fm);
    GList *paths = panel_selection_paths(p);
    if (!paths) {
        paths = g_list_prepend(NULL, g_strdup(panel_dir(p)));
    }
    GString *s = g_string_new(NULL);
    for (GList *l = paths; l; l = l->next) {
        if (s->len) g_string_append_c(s, '\n');
        if (p->ssh_conn)
            g_string_append_printf(s, "sftp://%s@%s%s", p->ssh_conn->user,
                                   p->ssh_conn->host, (const char *)l->data);
        else
            g_string_append(s, l->data);
    }
    gdk_clipboard_set_text(clipboard(fm), s->str);
    fm_status(fm, "Path copied: %s", s->str);
    g_string_free(s, TRUE);
    g_list_free_full(paths, g_free);
}

/* ── paste ────────────────────────────────────────────────────────── */

typedef struct {
    FM    *fm;
    gchar *mime;
    GOutputStream *mem;
} PasteCtx;

static void paste_ctx_free(PasteCtx *pc)
{
    g_free(pc->mime);
    g_clear_object(&pc->mem);
    g_free(pc);
}

static void on_paste_spliced(GObject *src, GAsyncResult *res, gpointer d)
{
    PasteCtx *pc = d;
    FM *fm = pc->fm;
    GError *err = NULL;
    if (g_output_stream_splice_finish(G_OUTPUT_STREAM(src), res, &err) < 0) {
        fm_status(fm, "Paste failed: %s", err->message);
        g_error_free(err);
        paste_ctx_free(pc);
        return;
    }

    gsize len = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(pc->mem));
    const char *data = g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(pc->mem));
    gboolean cut = FALSE;
    GList *paths = fs_parse_uri_list(data, len, &cut);
    if (strcmp(pc->mime, "text/uri-list") == 0) cut = FALSE;  /* no cut info */

    Panel *p = active_panel(fm);
    if (!paths) {
        fm_status(fm, "Clipboard contains no local files");
    } else if (p->search_mode) {
        fm_status(fm, "Leave search results first");
    } else if (arc_panel_readonly_msg(p)) {
        /* nothing is pasted into an archive */
    } else {
        int r = fo_transfer(fm, paths, NULL, panel_dir(p), p->ssh_conn, cut);
        /* a cut can be pasted only once – unless something was left behind */
        if (cut && r == FS_OK) gdk_clipboard_set_content(clipboard(fm), NULL);
        panel_focus(p);
    }
    g_list_free_full(paths, g_free);
    paste_ctx_free(pc);
}

static void on_clipboard_read(GObject *src, GAsyncResult *res, gpointer d)
{
    FM *fm = d;
    const char *mime = NULL;
    GError *err = NULL;
    GInputStream *in = gdk_clipboard_read_finish(GDK_CLIPBOARD(src), res, &mime, &err);
    if (!in) {
        fm_status(fm, "Clipboard contains no files");
        g_clear_error(&err);
        return;
    }
    PasteCtx *pc = g_new0(PasteCtx, 1);
    pc->fm   = fm;
    pc->mime = g_strdup(mime);
    pc->mem  = g_memory_output_stream_new_resizable();
    g_output_stream_splice_async(pc->mem, in,
        G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET,
        G_PRIORITY_DEFAULT, NULL, on_paste_spliced, pc);
    g_object_unref(in);
}

void clip_paste(FM *fm)
{
    static const char *mimes[] = {
        "x-special/gnome-copied-files", "text/uri-list", NULL
    };
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;
    GdkClipboard *cb = clipboard(fm);
    if (rclip.paths && gdk_clipboard_is_local(cb) &&
        gdk_clipboard_get_content(cb) == rclip.owner) {
        SshConn *src = rclip_conn(fm);
        if (!src) {
            fm_status(fm, "The SFTP connection of the copied files is closed");
        } else if (p->search_mode) {
            fm_status(fm, "Leave search results first");
        } else {
            gboolean cut = rclip.cut;
            GList *paths = g_list_copy_deep(rclip.paths, dup_str, NULL);
            int r = fo_transfer(fm, paths, src, panel_dir(p), p->ssh_conn, cut);
            if (cut && r == FS_OK) {
                gdk_clipboard_set_content(cb, NULL);
                rclip_clear();
            }
            g_list_free_full(paths, g_free);
            panel_focus(p);
        }
        return;
    }
    if (rclip.paths) rclip_clear();     /* someone else owns the clipboard */
    gdk_clipboard_read_async(cb, mimes, G_PRIORITY_DEFAULT, NULL,
                             on_clipboard_read, fm);
}
