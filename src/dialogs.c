#include "dialogs.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Shared pieces
 * ─────────────────────────────────────────────────────────────────── */

static GtkWidget *dlg_window(FM *fm, const char *title, int width)
{
    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), width, -1);
    return dlg;
}

static GtkWidget *dlg_vbox(void)
{
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);
    return vbox;
}

GtkWidget *dlg_file_list(GList *files)
{
    int n = g_list_length(files);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
        GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    if (n > 6) gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sw), 120);
    else       gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);

    GtkWidget *fbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    int shown = 0;
    for (GList *l = files; l; l = l->next) {
        if (++shown > 500) {            /* keep huge selections responsive */
            gchar *more = g_strdup_printf("… and %d more", n - 500);
            GtkWidget *row = gtk_label_new(more);
            g_free(more);
            gtk_widget_set_halign(row, GTK_ALIGN_START);
            gtk_box_append(GTK_BOX(fbox), row);
            break;
        }
        gchar *base = g_path_get_basename((const char *)l->data);
        GtkWidget *row = gtk_label_new(base);
        g_free(base);
        gtk_widget_set_halign(row, GTK_ALIGN_START);
        gtk_label_set_ellipsize(GTK_LABEL(row), PANGO_ELLIPSIZE_MIDDLE);
        gtk_widget_add_css_class(row, "dim-label");
        gtk_box_append(GTK_BOX(fbox), row);
    }
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), fbox);
    return sw;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Destination dialog (copy / move / extract) – Total Commander style
 * ─────────────────────────────────────────────────────────────────── */

gchar *dlg_count_prompt(const char *verb, GList *paths, const char *suffix)
{
    guint n = g_list_length(paths);
    /* no space before punctuation: "delete \"x\"?" */
    const char *sep = g_ascii_isalnum(suffix[0]) ? " " : "";
    if (n == 1) {
        gchar *base = g_path_get_basename(paths->data);
        gchar *r = g_markup_printf_escaped("%s \"<b>%s</b>\"%s%s", verb, base, sep, suffix);
        g_free(base);
        return r;
    }
    return g_markup_printf_escaped("%s <b>%u</b> files/directories%s%s", verb, n, sep, suffix);
}

static gboolean on_dlg_escape(GtkEventControllerKey *k, guint keyval, guint code,
                              GdkModifierType state, DlgCtx *ctx)
{
    (void)k; (void)code; (void)state;
    if (keyval != GDK_KEY_Escape) return FALSE;
    dlg_ctx_cancel(NULL, ctx);
    return TRUE;
}

static void add_escape(GtkWidget *dlg, DlgCtx *ctx)
{
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_dlg_escape), ctx);
    gtk_widget_add_controller(dlg, key);
}

gchar *ask_destination(FM *fm, const char *title, const char *prompt,
                       const char *default_dst)
{
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg  = dlg_window(fm, title, 520);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *vbox = dlg_vbox();
    gtk_box_set_spacing(GTK_BOX(vbox), 8);

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), prompt);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    /* target directory shown with a trailing "/" like TC */
    GtkWidget *entry = gtk_entry_new();
    if (default_dst) {
        gchar *t = g_str_has_suffix(default_dst, "/") ? g_strdup(default_dst)
                                                      : g_strconcat(default_dst, "/", NULL);
        gtk_editable_set_text(GTK_EDITABLE(entry), t);
        g_free(t);
    }
    gtk_widget_set_hexpand(entry, TRUE);

    GtkWidget *browse = gtk_button_new_from_icon_name("folder-open-symbolic");
    gtk_widget_set_tooltip_text(browse, "Select directory");
    gtk_widget_set_focusable(browse, FALSE);
    g_signal_connect(browse, "clicked", G_CALLBACK(on_browse_clicked), entry);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(hbox), entry);
    gtk_box_append(GTK_BOX(hbox), browse);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(btn_box), TRUE);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 4);
    GtkWidget *ok_btn     = gtk_button_new_with_label("OK");
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    gtk_widget_set_size_request(ok_btn, 90, -1);
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), ok_btn);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), hbox);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);
    gtk_window_set_default_widget(GTK_WINDOW(dlg), ok_btn);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);
    add_escape(dlg, &ctx);

    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(entry);
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);

    gchar *result = NULL;
    if (dlg_ctx_run(&ctx) == 1) {
        const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (txt && txt[0]) {
            if (txt[0] == '~' && (txt[1] == '\0' || txt[1] == '/'))
                result = g_build_filename(g_get_home_dir(), txt + 1, NULL);
            else
                result = g_strdup(txt);
            gsize l = strlen(result);
            while (l > 1 && result[l - 1] == '/') result[--l] = '\0';
        }
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    return result;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Yes / No
 * ─────────────────────────────────────────────────────────────────── */

static gboolean on_yes_no_key(GtkEventControllerKey *k, guint keyval, guint code,
                              GdkModifierType state, DlgCtx *ctx)
{
    (void)k; (void)code; (void)state;
    switch (keyval) {
    case GDK_KEY_y: case GDK_KEY_Y: dlg_ctx_ok(NULL, ctx);     return TRUE;
    case GDK_KEY_n: case GDK_KEY_N:
    case GDK_KEY_Escape:            dlg_ctx_cancel(NULL, ctx); return TRUE;
    default:                        return FALSE;
    }
}

gboolean ask_yes_no(FM *fm, const char *title, const char *markup)
{
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg  = dlg_window(fm, title, 420);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    GtkWidget *vbox = dlg_vbox();
    gtk_box_set_spacing(GTK_BOX(vbox), 12);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *icon = gtk_image_new_from_icon_name("dialog-question-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 32);
    GtkWidget *q = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(q), markup);
    gtk_label_set_wrap(GTK_LABEL(q), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(q), 50);
    gtk_label_set_xalign(GTK_LABEL(q), 0.0f);
    gtk_box_append(GTK_BOX(row), icon);
    gtk_box_append(GTK_BOX(row), q);

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_set_homogeneous(GTK_BOX(bb), TRUE);
    gtk_widget_set_halign(bb, GTK_ALIGN_CENTER);
    GtkWidget *yes = gtk_button_new_with_mnemonic("_Yes");
    GtkWidget *no  = gtk_button_new_with_mnemonic("_No");
    gtk_widget_set_size_request(yes, 90, -1);
    gtk_widget_add_css_class(yes, "suggested-action");
    gtk_box_append(GTK_BOX(bb), yes);
    gtk_box_append(GTK_BOX(bb), no);

    gtk_box_append(GTK_BOX(vbox), row);
    gtk_box_append(GTK_BOX(vbox), bb);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);
    gtk_window_set_default_widget(GTK_WINDOW(dlg), yes);

    g_signal_connect(yes, "clicked",       G_CALLBACK(dlg_ctx_ok),     &ctx);
    g_signal_connect(no,  "clicked",       G_CALLBACK(dlg_ctx_cancel), &ctx);
    g_signal_connect(dlg, "close-request", G_CALLBACK(dlg_ctx_close),  &ctx);
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_yes_no_key), &ctx);
    gtk_widget_add_controller(dlg, key);

    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(yes);
    gboolean r = dlg_ctx_run(&ctx) == 1;
    gtk_window_destroy(GTK_WINDOW(dlg));
    return r;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Text entry
 * ─────────────────────────────────────────────────────────────────── */

gchar *ask_text(FM *fm, const char *title, const char *prompt,
                const char *initial, const char *ok_label, gboolean select_stem)
{
    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg  = dlg_window(fm, title, 420);
    GtkWidget *vbox = dlg_vbox();

    GtkWidget *lbl = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl), prompt);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);

    GtkWidget *entry = gtk_entry_new();
    if (initial) gtk_editable_set_text(GTK_EDITABLE(entry), initial);
    gtk_widget_set_size_request(entry, 360, -1);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    gtk_widget_set_margin_top(btn_box, 8);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn     = gtk_button_new_with_label(ok_label);
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(ok_btn,     "clicked",       G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(cancel_btn, "clicked",       G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate",      G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg,        "close-request", G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));
    gtk_widget_grab_focus(entry);
    if (initial && initial[0]) {
        int end = -1;
        if (select_stem) {
            const char *dot = strrchr(initial, '.');
            if (dot && dot != initial)
                end = (int)g_utf8_pointer_to_offset(initial, dot);
        }
        gtk_editable_select_region(GTK_EDITABLE(entry), 0, end);
    }

    gchar *result = NULL;
    if (dlg_ctx_run(&ctx) == 1) {
        const gchar *txt = gtk_editable_get_text(GTK_EDITABLE(entry));
        if (txt && txt[0]) result = g_strdup(txt);
    }
    gtk_window_destroy(GTK_WINDOW(dlg));
    return result;
}

/* ─────────────────────────────────────────────────────────────────── *
 *  Multi-button choice
 * ─────────────────────────────────────────────────────────────────── */

typedef struct {
    GMainLoop *loop;
    int        response;
} ChoiceCtx;

static void on_choice_btn(GtkButton *b, ChoiceCtx *cc)
{
    cc->response = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "idx"));
    g_main_loop_quit(cc->loop);
}

static gboolean on_choice_close(GtkWindow *w, ChoiceCtx *cc)
{
    (void)w;
    cc->response = -1;
    g_main_loop_quit(cc->loop);
    return TRUE;
}

static gboolean on_choice_key(GtkEventControllerKey *k, guint keyval, guint code,
                              GdkModifierType state, ChoiceCtx *cc)
{
    (void)k; (void)code; (void)state;
    if (keyval == GDK_KEY_Escape) {
        cc->response = -1;
        g_main_loop_quit(cc->loop);
        return TRUE;
    }
    return FALSE;
}

int ask_choice(FM *fm, const char *title, const char *markup, GList *files,
               const char *const *buttons, int destructive)
{
    ChoiceCtx cc = { g_main_loop_new(NULL, FALSE), -1 };

    GtkWidget *dlg  = dlg_window(fm, title, 420);
    GtkWidget *vbox = dlg_vbox();
    gtk_box_set_spacing(GTK_BOX(vbox), 8);

    GtkWidget *q = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(q), markup);
    gtk_label_set_wrap(GTK_LABEL(q), TRUE);
    gtk_widget_set_halign(q, GTK_ALIGN_START);
    gtk_box_append(GTK_BOX(vbox), q);
    if (files) gtk_box_append(GTK_BOX(vbox), dlg_file_list(files));

    GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(bb, GTK_ALIGN_END);
    gtk_widget_set_margin_top(bb, 8);
    GtkWidget *last = NULL;
    for (int i = 0; buttons[i]; i++) {
        GtkWidget *b = gtk_button_new_with_label(buttons[i]);
        g_object_set_data(G_OBJECT(b), "idx", GINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(on_choice_btn), &cc);
        if (i == destructive) gtk_widget_add_css_class(b, "destructive-action");
        gtk_box_append(GTK_BOX(bb), b);
        last = b;
    }
    if (last && destructive < 0) gtk_widget_add_css_class(last, "suggested-action");
    gtk_box_append(GTK_BOX(vbox), bb);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    GtkEventController *key = gtk_event_controller_key_new();
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_choice_key), &cc);
    gtk_widget_add_controller(dlg, key);
    g_signal_connect(dlg, "close-request", G_CALLBACK(on_choice_close), &cc);

    gtk_window_present(GTK_WINDOW(dlg));
    if (last) gtk_widget_grab_focus(last);
    g_main_loop_run(cc.loop);
    g_main_loop_unref(cc.loop);
    gtk_window_destroy(GTK_WINDOW(dlg));
    return cc.response;
}

/* ── F1 help: plain list of keys ──────────────────────────────────── */

static const char *const help_keys[][2] = {
    { "Tab",                 "Switch panel" },
    { "Enter / Backspace",   "Open (archives as folders) / parent directory" },
    { "Space, Insert",       "Mark file" },
    { "+  -  *",             "Mark group / unmark group / invert" },
    { "Ctrl+A, Ctrl+Shift+A","Mark all / unmark all" },
    { "F2 / Shift+F2",       "Search / search history" },
    { "F3",                  "View file" },
    { "F4 / Shift+F4",       "Edit / new file" },
    { "F5 / Shift+F5",       "Copy / copy here" },
    { "F6 / Shift+F6, F9",   "Move / rename" },
    { "Ctrl+M",              "Multi-rename (mask, counter, find/replace)" },
    { "F7",                  "New directory" },
    { "F8, Del / Shift+Del", "Delete to trash / permanently" },
    { "Alt+Enter",           "Properties" },
    { "Ctrl+C / X / V",      "Clipboard copy / cut / paste" },
    { "Ctrl+Shift+C",        "Copy path" },
    { "Alt+P / Alt+E",       "Create / extract archive" },
    { "Ctrl+S",              "Quick search (jump to file)" },
    { "Ctrl+H",              "Hidden files" },
    { "Ctrl+R",              "Refresh" },
    { "Ctrl+=",              "Same directory in both panels" },
    { "Ctrl+T / Ctrl+W",     "New tab / close tab" },
    { "Ctrl+Tab / +Shift",   "Next / previous tab" },
    { "Ctrl+\u2191",         "Directory under cursor in a new tab" },
    { "Alt+←",          "Back to search results" },
    { "Ctrl+PgDn / Ctrl+Enter", "Go to found file / enter archive" },
    { "F10",                 "Quit" },
};

void help_show(FM *fm)
{
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Keyboard shortcuts");
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(fm->window));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 24);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_margin_top(grid, 16);
    gtk_widget_set_margin_bottom(grid, 16);
    gtk_widget_set_margin_start(grid, 20);
    gtk_widget_set_margin_end(grid, 20);

    for (gsize i = 0; i < G_N_ELEMENTS(help_keys); i++) {
        GtkWidget *k = gtk_label_new(help_keys[i][0]);
        GtkWidget *v = gtk_label_new(help_keys[i][1]);
        gtk_widget_add_css_class(k, "monospace");
        gtk_widget_add_css_class(k, "heading");
        gtk_label_set_xalign(GTK_LABEL(k), 0);
        gtk_label_set_xalign(GTK_LABEL(v), 0);
        gtk_grid_attach(GTK_GRID(grid), k, 0, (int)i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), v, 1, (int)i, 1, 1);
    }
    gtk_window_set_child(GTK_WINDOW(win), grid);

    /* Esc / F1 closes */
    GtkEventController *sc = gtk_shortcut_controller_new();
    GtkShortcutAction *close = gtk_named_action_new("window.close");
    gtk_shortcut_controller_add_shortcut(GTK_SHORTCUT_CONTROLLER(sc),
        gtk_shortcut_new(gtk_alternative_trigger_new(
                             gtk_keyval_trigger_new(GDK_KEY_Escape, 0),
                             gtk_keyval_trigger_new(GDK_KEY_F1, 0)),
                         g_object_ref(close)));
    g_object_unref(close);
    gtk_widget_add_controller(win, sc);

    gtk_window_present(GTK_WINDOW(win));
}
