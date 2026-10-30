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

void clip_copy(FM *fm, gboolean cut)
{
    Panel *p = active_panel(fm);
    if (arc_panel_readonly_msg(p)) return;      /* F5 extracts instead */
    if (p->ssh_conn) {
        fm_status(fm, "Clipboard works with local files only – use F5/F6 for SFTP");
        return;
    }
    GList *paths = panel_selection_paths(p);
    if (!paths) { fm_status(fm, "Nothing is selected"); return; }

    GString *gnome = g_string_new(cut ? "cut" : "copy");
    GString *uris  = g_string_new(NULL);
    GString *text  = g_string_new(NULL);
    int n = 0;
    for (GList *l = paths; l; l = l->next, n++) {
        gchar *uri = g_filename_to_uri(l->data, NULL, NULL);
        if (!uri) continue;
        g_string_append_printf(gnome, "\n%s", uri);
        g_string_append_printf(uris, "%s\r\n", uri);
        g_string_append_printf(text, "%s%s", n ? "\n" : "", (const char *)l->data);
        g_free(uri);
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
    if (arc_panel_readonly_msg(active_panel(fm))) return;
    gdk_clipboard_read_async(clipboard(fm), mimes, G_PRIORITY_DEFAULT, NULL,
                             on_clipboard_read, fm);
}
