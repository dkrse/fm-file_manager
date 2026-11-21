/* Per-panel menus: hamburger menu, right-click context menu, "Open with",
 * File mask dialog – split out of main.c */
#include "fm.h"
#include "dialogs.h"
#include "job.h"
#include <string.h>
#include <fnmatch.h>

static void menu_mask(GSimpleAction *a, GVariant *v, gpointer d);
/* Per-panel hamburger menu callbacks */
/* Every panel menu entry makes its panel active, then runs an FM action */
#define PMENU(fn, call) \
    static void fn(GSimpleAction *a, GVariant *v, gpointer d) \
    { (void)a; (void)v; Panel *p = d; set_active_panel(p->fm, p->idx); call; }

PMENU(pmenu_ssh,      ssh_connect_dialog(p->fm))
PMENU(pmenu_pack,     fo_pack(p->fm))
PMENU(pmenu_extract,  fo_extract(p->fm))
PMENU(pmenu_mask,     menu_mask(a, v, p->fm))
PMENU(pmenu_search,   search_run(p->fm))
PMENU(pmenu_filter,   panel_show_filter(p))
PMENU(pmenu_sresults, search_restore(p))
PMENU(pmenu_shistory, search_history_dialog(p->fm))
PMENU(pmenu_copy,     clip_copy(p->fm, FALSE))
PMENU(pmenu_cut,      clip_copy(p->fm, TRUE))
PMENU(pmenu_paste,    clip_paste(p->fm))
PMENU(pmenu_copypath, clip_copy_paths(p->fm))
PMENU(pmenu_dup,      fo_copy_here(p->fm))
PMENU(pmenu_rename,   fo_rename(p->fm))
PMENU(pmenu_mrename,  fo_multi_rename(p->fm))
PMENU(pmenu_delete,   fo_delete(p->fm))
PMENU(pmenu_mkdir,    fo_mkdir(p->fm))
PMENU(pmenu_newfile,  fo_newfile(p->fm))
PMENU(pmenu_props,    fo_properties(p->fm))
PMENU(pmenu_selall,   sel_mark_all(p, TRUE))
PMENU(pmenu_selnone,  sel_mark_all(p, FALSE))
PMENU(pmenu_selgroup, sel_group_dialog(p->fm, TRUE))
PMENU(pmenu_unselgrp, sel_group_dialog(p->fm, FALSE))
PMENU(pmenu_invert,   sel_invert(p))
PMENU(pmenu_tabnew,   panel_tab_new(p, NULL))
PMENU(pmenu_tabclose, panel_tab_close(p))
PMENU(pmenu_qview,    qv_toggle(p->fm))
PMENU(pmenu_cmpdirs,  cmp_dirs(p->fm))
PMENU(pmenu_cmpfiles, cmp_files(p->fm))
PMENU(pmenu_cksnew,   cks_create(p->fm))
PMENU(pmenu_cksver,   cks_verify(p->fm))
PMENU(pmenu_net,      net_connect_dialog(p->fm))
PMENU(pmenu_jobs,     job_show_background(p->fm))
#undef PMENU

/* "Open with": target is a desktop app id, "" asks via the system chooser */
static void pmenu_openwith(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a;
    Panel *p = d;
    set_active_panel(p->fm, p->idx);
    gchar *path = panel_cursor_fullpath(p);
    if (!path) return;
    GFile *file = g_file_new_for_path(path);
    const char *id = g_variant_get_string(v, NULL);
    if (id[0]) {
        GAppInfo *app = NULL;
        GList *all = g_app_info_get_all();
        for (GList *l = all; l && !app; l = l->next)
            if (g_strcmp0(g_app_info_get_id(l->data), id) == 0)
                app = g_object_ref(l->data);
        g_list_free_full(all, g_object_unref);
        GError *err = NULL;
        GList *files = g_list_append(NULL, file);
        GdkAppLaunchContext *lc = gdk_display_get_app_launch_context(
            gtk_widget_get_display(p->fm->window));
        ext_view_remember(p);       /* before: the app may take focus at once */
        if (!app || !g_app_info_launch(G_APP_INFO(app), files,
                                       G_APP_LAUNCH_CONTEXT(lc), &err)) {
            ext_view_forget();
            fm_status(p->fm, "Open with: %s", err ? err->message : "application not found");
        }
        g_clear_error(&err);
        g_list_free(files);
        g_object_unref(lc);
        if (app) g_object_unref(app);
    } else {
        ext_view_remember(p);
        GtkFileLauncher *fl = gtk_file_launcher_new(file);
        gtk_file_launcher_set_always_ask(fl, TRUE);
        gtk_file_launcher_launch(fl, GTK_WINDOW(p->fm->window), NULL, NULL, NULL);
        g_object_unref(fl);
    }
    g_object_unref(file);
    g_free(path);
}

typedef struct { const char *label, *action; } MenuEntry;

/* Section of `menu` built from entries, actions prefixed with the group */
static void menu_section(GMenu *menu, const char *grp, const MenuEntry *e, gsize n)
{
    GMenu *sec = g_menu_new();
    for (gsize i = 0; i < n; i++) {
        gchar *act = g_strdup_printf("%s.%s", grp, e[i].action);
        g_menu_append(sec, e[i].label, act);
        g_free(act);
    }
    g_menu_append_section(menu, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
}

static GMenu *build_file_menu(const char *grp)
{
    static const MenuEntry edit[] = {
        { "Copy    Ctrl+C",            "copy"     },
        { "Cut    Ctrl+X",             "cut"      },
        { "Paste    Ctrl+V",           "paste"    },
        { "Copy path    Ctrl+Shift+C", "copypath" },
    };
    static const MenuEntry file[] = {
        { "Duplicate\u2026    Shift+F5",   "dup"      },
        { "Rename\u2026    Shift+F6",      "rename"   },
        { "Multi-rename\u2026    Ctrl+M",  "mrename"  },
        { "Delete\u2026    Del",           "delete"   },
        { "New directory\u2026    F7",     "mkdir"    },
        { "New file\u2026    Shift+F4",    "newfile"  },
        { "Properties\u2026    Alt+Enter", "props"    },
    };
    static const MenuEntry sel[] = {
        { "Select all    Ctrl+A",          "selall"   },
        { "Unselect all    Ctrl+Shift+A",  "selnone"  },
        { "Select group\u2026    +",       "selgroup" },
        { "Unselect group\u2026    -",     "unselgrp" },
        { "Invert selection    *",         "invert"   },
    };
    GMenu *m = g_menu_new();
    menu_section(m, grp, edit, G_N_ELEMENTS(edit));
    menu_section(m, grp, file, G_N_ELEMENTS(file));
    menu_section(m, grp, sel,  G_N_ELEMENTS(sel));
    return m;
}

/* "Open with" submenu for a local regular file, NULL otherwise */
static GMenu *build_openwith_menu(Panel *p, const char *grp)
{
    if (p->ssh_conn || p->arc_file) return NULL;
    gchar *path = panel_cursor_fullpath(p);
    if (!path || !g_file_test(path, G_FILE_TEST_IS_REGULAR)) { g_free(path); return NULL; }

    gchar *ctype = g_content_type_guess(path, NULL, 0, NULL);
    g_free(path);
    GMenu *m = g_menu_new();
    GAppInfo *def = g_app_info_get_default_for_type(ctype, FALSE);
    GList *apps = g_app_info_get_all_for_type(ctype);
    g_free(ctype);
    gchar *act = g_strdup_printf("%s.openwith", grp);
    /* default application first, then the rest */
    if (def) apps = g_list_prepend(apps, def);
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (GList *l = apps; l; l = l->next) {
        const char *id = g_app_info_get_id(l->data);
        if (!id || g_hash_table_contains(seen, id)) continue;
        g_hash_table_add(seen, (gpointer)id);
        GMenuItem *mi = g_menu_item_new(g_app_info_get_display_name(l->data), NULL);
        g_menu_item_set_action_and_target_value(mi, act, g_variant_new_string(id));
        GIcon *ic = g_app_info_get_icon(l->data);
        if (ic) g_menu_item_set_icon(mi, ic);
        g_menu_append_item(m, mi);
        g_object_unref(mi);
    }
    g_hash_table_destroy(seen);
    g_list_free_full(apps, g_object_unref);

    GMenu *other = g_menu_new();
    GMenuItem *mi = g_menu_item_new("Other application\u2026", NULL);
    g_menu_item_set_action_and_target_value(mi, act, g_variant_new_string(""));
    g_menu_append_item(other, mi);
    g_object_unref(mi);
    g_menu_append_section(m, NULL, G_MENU_MODEL(other));
    g_object_unref(other);
    g_free(act);
    return m;
}

/* Right click: move cursor to the clicked row, then show the file menu */
static void on_cv_right_click(GtkGestureClick *g, int n_press, double x, double y, Panel *p)
{
    (void)n_press;
    set_active_panel(p->fm, p->idx);

    GtkWidget *w = gtk_widget_pick(p->column_view, x, y, GTK_PICK_DEFAULT);
    FileItem *hit = NULL;
    for (; w && w != p->column_view; w = gtk_widget_get_parent(w))
        if ((hit = g_object_get_data(G_OBJECT(w), "fm-item"))) break;
    if (hit) {
        guint n = g_list_model_get_n_items(G_LIST_MODEL(p->filter_model));
        for (guint i = 0; i < n; i++) {
            FileItem *it = g_list_model_get_item(G_LIST_MODEL(p->filter_model), i);
            g_object_unref(it);
            if (it == hit) {
                p->cursor_pos = i;
                p->inhibit_sel = TRUE;
                gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), i,
                                          NULL, GTK_LIST_SCROLL_SELECT, NULL);
                p->inhibit_sel = FALSE;
                break;
            }
        }
    }

    GtkWidget *pop = g_object_get_data(G_OBJECT(p->column_view), "fm-ctx-menu");
    {
        /* rebuilt each time: the "Open with" list depends on the file type */
        gchar *grp = g_strdup_printf("p%d", p->idx);
        GMenu *cm = build_file_menu(grp);
        GMenu *ow = hit ? build_openwith_menu(p, grp) : NULL;
        if (ow) {
            GMenu *sec = g_menu_new();
            g_menu_append_submenu(sec, "Open with", G_MENU_MODEL(ow));
            g_menu_prepend_section(cm, NULL, G_MENU_MODEL(sec));
            g_object_unref(sec);
            g_object_unref(ow);
        }
        gtk_popover_menu_set_menu_model(GTK_POPOVER_MENU(pop), G_MENU_MODEL(cm));
        g_object_unref(cm);
        g_free(grp);
    }
    GdkRectangle r = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    gtk_popover_popup(GTK_POPOVER(pop));
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
}


/* ── Menu action callbacks ────────────────────────────────────────── */

static void menu_mask(GSimpleAction *a, GVariant *v, gpointer d)
{
    (void)a; (void)v;
    FM *fm = d;
    Panel *p = active_panel(fm);

    DlgCtx ctx;
    dlg_ctx_init(&ctx);

    GtkWidget *dlg = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "File mask");
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(fm->window));
    gtk_window_set_default_size(GTK_WINDOW(dlg), 350, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(vbox, 12);
    gtk_widget_set_margin_end(vbox, 12);
    gtk_widget_set_margin_top(vbox, 12);
    gtk_widget_set_margin_bottom(vbox, 12);

    GtkWidget *lbl = gtk_label_new("Show only (glob, e.g. *.c):");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    GtkWidget *entry = gtk_entry_new();
    /* Pre-fill with current mask or * */
    gtk_editable_set_text(GTK_EDITABLE(entry),
        (p->mask_pattern && p->mask_pattern[0]) ? p->mask_pattern : "*");

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *btn_cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *btn_ok     = gtk_button_new_with_label("OK");
    gtk_widget_add_css_class(btn_ok, "suggested-action");
    gtk_box_append(GTK_BOX(btn_box), btn_cancel);
    gtk_box_append(GTK_BOX(btn_box), btn_ok);

    gtk_box_append(GTK_BOX(vbox), lbl);
    gtk_box_append(GTK_BOX(vbox), entry);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(dlg), vbox);

    g_signal_connect(btn_ok,     "clicked",  G_CALLBACK(dlg_ctx_ok),       &ctx);
    g_signal_connect(btn_cancel, "clicked",  G_CALLBACK(dlg_ctx_cancel),   &ctx);
    g_signal_connect(entry,      "activate", G_CALLBACK(dlg_ctx_entry_ok), &ctx);
    g_signal_connect(dlg, "close-request",   G_CALLBACK(dlg_ctx_close),    &ctx);

    gtk_window_present(GTK_WINDOW(dlg));
    int resp = dlg_ctx_run(&ctx);

    const gchar *pat = gtk_editable_get_text(GTK_EDITABLE(entry));
    gchar *pattern = g_strdup(pat);
    gtk_window_destroy(GTK_WINDOW(dlg));

    if (resp != 1) { g_free(pattern); return; }

    /* Set mask: "*" or empty means no mask */
    g_free(p->mask_pattern);
    if (!pattern[0] || strcmp(pattern, "*") == 0 || strcmp(pattern, "*.*") == 0)
        p->mask_pattern = NULL;
    else
        p->mask_pattern = pattern;
    if (!p->mask_pattern) g_free(pattern);

    /* Trigger re-filter */
    panel_apply_mask(p);
    p->cursor_pos = 0;
    p->inhibit_sel = TRUE;
    if (g_list_model_get_n_items(G_LIST_MODEL(p->filter_model)) > 0) {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(p->column_view), 0,
                                  NULL, GTK_LIST_SCROLL_SELECT, NULL);
    }
    p->inhibit_sel = FALSE;
    panel_update_status(p);
    panel_focus(p);
}

/* Actions behind both panel menus, in the action group "p<idx>" */
GSimpleActionGroup *panel_menu_actions(Panel *p)
{
    GSimpleActionGroup *ag = g_simple_action_group_new();
    {
        struct { const char *name; GCallback cb; } acts[] = {
            { "ssh",      G_CALLBACK(pmenu_ssh)      },
            { "pack",     G_CALLBACK(pmenu_pack)     },
            { "extract",  G_CALLBACK(pmenu_extract)  },
            { "mask",     G_CALLBACK(pmenu_mask)     },
            { "search",   G_CALLBACK(pmenu_search)   },
            { "filter",   G_CALLBACK(pmenu_filter)   },
            { "sresults", G_CALLBACK(pmenu_sresults) },
            { "shistory", G_CALLBACK(pmenu_shistory) },
            { "copy",     G_CALLBACK(pmenu_copy)     },
            { "cut",      G_CALLBACK(pmenu_cut)      },
            { "paste",    G_CALLBACK(pmenu_paste)    },
            { "copypath", G_CALLBACK(pmenu_copypath) },
            { "dup",      G_CALLBACK(pmenu_dup)      },
            { "rename",   G_CALLBACK(pmenu_rename)   },
            { "mrename",  G_CALLBACK(pmenu_mrename)  },
            { "delete",   G_CALLBACK(pmenu_delete)   },
            { "mkdir",    G_CALLBACK(pmenu_mkdir)    },
            { "newfile",  G_CALLBACK(pmenu_newfile)  },
            { "props",    G_CALLBACK(pmenu_props)    },
            { "selall",   G_CALLBACK(pmenu_selall)   },
            { "selnone",  G_CALLBACK(pmenu_selnone)  },
            { "selgroup", G_CALLBACK(pmenu_selgroup) },
            { "unselgrp", G_CALLBACK(pmenu_unselgrp) },
            { "invert",   G_CALLBACK(pmenu_invert)   },
            { "tabnew",   G_CALLBACK(pmenu_tabnew)   },
            { "tabclose", G_CALLBACK(pmenu_tabclose) },
            { "qview",    G_CALLBACK(pmenu_qview)    },
            { "cmpdirs",  G_CALLBACK(pmenu_cmpdirs)  },
            { "cmpfiles", G_CALLBACK(pmenu_cmpfiles) },
            { "cksnew",   G_CALLBACK(pmenu_cksnew)   },
            { "cksver",   G_CALLBACK(pmenu_cksver)   },
            { "net",      G_CALLBACK(pmenu_net)      },
            { "jobs",     G_CALLBACK(pmenu_jobs)     },
        };
        for (gsize i = 0; i < G_N_ELEMENTS(acts); i++) {
            GSimpleAction *sa = g_simple_action_new(acts[i].name, NULL);
            g_signal_connect(sa, "activate", acts[i].cb, p);
            g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
            g_object_unref(sa);
        }
        {
            GSimpleAction *sa = g_simple_action_new("openwith", G_VARIANT_TYPE_STRING);
            g_signal_connect(sa, "activate", G_CALLBACK(pmenu_openwith), p);
            g_action_map_add_action(G_ACTION_MAP(ag), G_ACTION(sa));
            g_object_unref(sa);
        }
    }
    return ag;
}

/* Hamburger button in the path bar */
GtkWidget *panel_menu_button(const char *grp)
{
    static const MenuEntry conn[] = {
        { "SSH / SFTP\u2026", "ssh" },
        { "Connect to server (SMB, FTP, WebDAV)\u2026    Ctrl+N", "net" },
    };
    static const MenuEntry tools[] = {
        { "Quick view    Ctrl+Q",                       "qview"    },
        { "Compare / sync directories\u2026    Alt+C",   "cmpdirs"  },
        { "Compare files    Alt+D",                     "cmpfiles" },
        { "Create checksums\u2026    Alt+K",            "cksnew"   },
        { "Verify checksums    Alt+Shift+K",            "cksver"   },
        { "Background jobs    Ctrl+J",                  "jobs"     },
    };
    static const MenuEntry arc[] = {
        { "Create archive\u2026    Alt+P",  "pack"    },
        { "Extract archive\u2026    Alt+E", "extract" },
    };
    static const MenuEntry find[] = {
        { "File mask\u2026",             "mask"   },
        { "Search files\u2026    F2",     "search" },
        { "Filter files\u2026    Ctrl+S", "filter" },
        { "Back to search results    Alt+\u2190", "sresults" },
        { "Search history\u2026    Shift+F2",  "shistory" },
    };
    static const MenuEntry tabs[] = {
        { "New tab    Ctrl+T",   "tabnew"   },
        { "Close tab    Ctrl+W", "tabclose" },
    };
    GMenu *pmenu = g_menu_new();
    menu_section(pmenu, grp, tabs, G_N_ELEMENTS(tabs));
    menu_section(pmenu, grp, conn, G_N_ELEMENTS(conn));
    GMenu *files = build_file_menu(grp);
    g_menu_append_submenu(pmenu, "Files", G_MENU_MODEL(files));
    g_object_unref(files);
    menu_section(pmenu, grp, arc,  G_N_ELEMENTS(arc));
    menu_section(pmenu, grp, find, G_N_ELEMENTS(find));
    GMenu *tm = g_menu_new();
    menu_section(tm, grp, tools, G_N_ELEMENTS(tools));
    g_menu_append_submenu(pmenu, "Tools", G_MENU_MODEL(tm));
    g_object_unref(tm);

    GtkWidget *hbtn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(hbtn), "open-menu-symbolic");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(hbtn), G_MENU_MODEL(pmenu));
    gtk_widget_set_tooltip_text(hbtn, "Panel menu");
    gtk_widget_set_focusable(hbtn, FALSE);
    g_object_unref(pmenu);
    return hbtn;
}

/* Right-click popover on the file list */
void panel_context_menu_attach(Panel *p, const char *grp)
{
    GMenu *cm = build_file_menu(grp);
    GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(cm));
    g_object_unref(cm);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    gtk_widget_set_parent(pop, p->column_view);
    /* unparented together with the column view (a popover is not a
     * child the column view would release itself) */
    g_object_set_data_full(G_OBJECT(p->column_view), "fm-ctx-menu", pop,
                           (GDestroyNotify)gtk_widget_unparent);

    GtkGesture *rc = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(rc), GDK_BUTTON_SECONDARY);
    g_signal_connect(rc, "pressed", G_CALLBACK(on_cv_right_click), p);
    gtk_widget_add_controller(p->column_view, GTK_EVENT_CONTROLLER(rc));
}
