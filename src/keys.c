/* F-key toolbar and the global key handler – split out of main.c */
#include "fm.h"
#include "dialogs.h"
#include "job.h"
#include <string.h>

/* ─────────────────────────────────────────────────────────────────── *
 *  Bottom toolbar – F-key buttons
 * ─────────────────────────────────────────────────────────────────── */

static void btn_copy(GtkButton *b, gpointer d)    { (void)b; fo_copy(d);   }
static void btn_move(GtkButton *b, gpointer d)    { (void)b; fo_move(d);   }
static void btn_mkdir(GtkButton *b, gpointer d)   { (void)b; fo_mkdir(d);  }
static void btn_delete(GtkButton *b, gpointer d)  { (void)b; fo_delete(d); }
static void btn_help(GtkButton *b, gpointer d)    { (void)b; help_show(d); }
static void btn_find(GtkButton *b, gpointer d)    { (void)b; search_run(d); }
static void btn_view(GtkButton *b, gpointer d)    { (void)b; viewer_show(d); }
static void btn_editor(GtkButton *b, gpointer d)  { (void)b; editor_show(d); }
/* close the window (not g_application_quit): close-request handlers run,
 * so the window size is saved and a running search is stopped first */
static void btn_quit(GtkButton *b, gpointer d)    { (void)b; gtk_window_close(GTK_WINDOW(((FM*)d)->window)); }

GtkWidget *create_toolbar(FM *fm)
{
    static const struct {
        const char *label;
        const char *tooltip;
        void (*cb)(GtkButton *, gpointer);
    } btns[] = {
        { "F1 Help",    "Keyboard shortcuts",        btn_help    },
        { "F2 Search",  "Search files",              btn_find    },
        { "F3 View",    "View file contents",        btn_view    },
        { "F4 Editor",  "Open file in editor",       btn_editor  },
        { "F5 Copy",    "Copy to other panel",       btn_copy    },
        { "F6 Move",    "Move to other panel",       btn_move    },
        { "F7 MkDir",   "Create new directory",      btn_mkdir   },
        { "F8 Delete",  "Delete selected files",     btn_delete  },
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

gboolean on_window_key_press(GtkEventControllerKey *ctrl,
                                     guint keyval, guint keycode,
                                     GdkModifierType state, FM *fm)
{
    (void)ctrl;

    /* Ctrl+Tab / Ctrl+Shift+Tab – next / previous tab (before plain Tab) */
    if ((state & GDK_CONTROL_MASK) &&
        (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab)) {
        gboolean back = keyval == GDK_KEY_ISO_Left_Tab || (state & GDK_SHIFT_MASK);
        panel_tab_cycle(active_panel(fm), back ? -1 : 1);
        return TRUE;
    }

    /* command line: typing in a panel, Ctrl+Enter, navigation */
    if (cmdline_handle_key(fm, keyval, state)) return TRUE;

    /* Tools: quick view, compare, checksums, network, background jobs */
    {
        gboolean c = (state & GDK_CONTROL_MASK) != 0;
        gboolean a = (state & GDK_ALT_MASK) != 0;
        gboolean s = (state & GDK_SHIFT_MASK) != 0;
        guint kv = gdk_keyval_to_lower(keyval);
        if (c && !a && kv == GDK_KEY_q) { qv_toggle(fm); return TRUE; }
        if (c && !a && kv == GDK_KEY_n) { net_connect_dialog(fm); return TRUE; }
        if (c && !a && kv == GDK_KEY_j) { job_show_background(fm); return TRUE; }
        if (a && !c && kv == GDK_KEY_c) { cmp_dirs(fm);  return TRUE; }
        if (a && !c && kv == GDK_KEY_d) { cmp_files(fm); return TRUE; }
        if (a && !c && kv == GDK_KEY_k) {
            if (s) cks_verify(fm); else cks_create(fm);
            return TRUE;
        }
    }

    /* Tab – switch active panel; with the quick view open the other
     * panel shows the preview, so the focus stays here */
    if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) && qv_active(fm)) {
        fm_status(fm, "Quick view is open – Ctrl+Q closes it");
        return TRUE;
    }
    if (keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) {
        /* the panel that really has the focus, not just fm->active */
        GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(fm->window));
        int cur = fm->active;
        for (int i = 0; i < 2; i++)
            if (f && (f == fm->panels[i].column_view ||
                      gtk_widget_is_ancestor(f, fm->panels[i].column_view)))
                cur = i;
        panel_focus(&fm->panels[cur ^ 1]);
        return TRUE;
    }

    /* Navigation keys with the keyboard focus nowhere in a panel (e.g. a
     * modal job window just closed and the focus did not come back):
     * GTK would move the focus to the next widget – typically the other
     * panel's path entry.  Put the cursor back into the active panel and
     * handle the key there instead. */
    switch (keyval) {
    case GDK_KEY_Up: case GDK_KEY_Down: case GDK_KEY_Page_Up: case GDK_KEY_Page_Down:
    case GDK_KEY_Home: case GDK_KEY_End: case GDK_KEY_Return: case GDK_KEY_KP_Enter:
    case GDK_KEY_space: case GDK_KEY_Insert: case GDK_KEY_BackSpace: {
        GtkWidget *f = gtk_window_get_focus(GTK_WINDOW(fm->window));
        gboolean in_text  = f && (GTK_IS_EDITABLE(f) || GTK_IS_TEXT_VIEW(f));
        gboolean in_panel = FALSE;
        for (int i = 0; i < 2; i++)
            if (f && (f == fm->panels[i].column_view ||
                      gtk_widget_is_ancestor(f, fm->panels[i].column_view)))
                in_panel = TRUE;
        if (!in_text && !in_panel) {
            Panel *a = active_panel(fm);
            panel_focus(a);
            on_tree_key_press(NULL, keyval, keycode, state, a);
            return TRUE;
        }
        break;
    }
    default:
        break;
    }

    /* F-keys – only when not typing in path entry */
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(fm->window));
    gboolean in_cmd = focus && fm->cmdline &&
        (focus == fm->cmdline || gtk_widget_is_ancestor(focus, fm->cmdline));
    if (in_cmd && !(keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)) return FALSE;
    if (!in_cmd && focus && (GTK_IS_EDITABLE(focus) || GTK_IS_TEXT_VIEW(focus))) return FALSE;

    gboolean shift = (state & GDK_SHIFT_MASK) != 0;
    gboolean ctl   = (state & GDK_CONTROL_MASK) != 0;
    gboolean alt   = (state & GDK_ALT_MASK) != 0;

    switch (keyval) {
    case GDK_KEY_F5:
        if (shift) fo_copy_here(fm); else fo_copy(fm);
        return TRUE;
    case GDK_KEY_F6:
        if (shift) fo_rename(fm); else fo_move(fm);
        return TRUE;
    case GDK_KEY_F7:  fo_mkdir(fm);           return TRUE;
    case GDK_KEY_F8:
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        if (shift) fo_delete_permanent(fm); else fo_delete(fm);
        return TRUE;
    case GDK_KEY_F9:  fo_rename(fm);          return TRUE;
    case GDK_KEY_F2:
        if (alt) { devices_popup(&fm->panels[1]); return TRUE; }
        if (shift) search_history_dialog(fm); else search_run(fm);
        return TRUE;

    /* Alt+Left – back to the last search results */
    case GDK_KEY_Left:
        if (alt) {
            if (!search_restore(active_panel(fm)))
                fm_status(fm, "No search results to return to (Shift+F2 = history)");
            return TRUE;
        }
        break;
    case GDK_KEY_F3:  viewer_show(fm);        return TRUE;
    case GDK_KEY_F4:
        if (shift) fo_newfile(fm); else editor_show(fm);
        return TRUE;
    case GDK_KEY_F1:
        if (alt) { devices_popup(&fm->panels[0]); return TRUE; }
        help_show(fm);
        return TRUE;
    case GDK_KEY_F10: gtk_window_close(GTK_WINDOW(fm->window)); return TRUE;

    /* Alt+Enter – properties */
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
        if (alt) { fo_properties(fm); return TRUE; }
        break;

    /* Clipboard / marking */
    case GDK_KEY_c:
    case GDK_KEY_C:
        if (ctl && shift) { clip_copy_paths(fm);  return TRUE; }
        if (ctl)          { clip_copy(fm, FALSE); return TRUE; }
        break;
    case GDK_KEY_x:
    case GDK_KEY_X:
        if (ctl) { clip_copy(fm, TRUE); return TRUE; }
        break;
    case GDK_KEY_v:
    case GDK_KEY_V:
        if (ctl) { clip_paste(fm); return TRUE; }
        break;
    case GDK_KEY_a:
    case GDK_KEY_A:
        if (ctl) { sel_mark_all(active_panel(fm), !shift); return TRUE; }
        break;

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

    /* Ctrl+M – multi-rename */
    case GDK_KEY_m:
    case GDK_KEY_M:
        if (ctl) { fo_multi_rename(fm); return TRUE; }
        break;

    /* Tabs: Ctrl+T new, Ctrl+W close, Ctrl+Up directory under cursor */
    case GDK_KEY_t:
    case GDK_KEY_T:
        if (ctl) { panel_tab_new(active_panel(fm), NULL); return TRUE; }
        break;
    case GDK_KEY_w:
    case GDK_KEY_W:
        if (ctl) { panel_tab_close(active_panel(fm)); return TRUE; }
        break;
    case GDK_KEY_Up:
        if (ctl) {
            Panel *a = active_panel(fm);
            if (a->ssh_conn) {
                gchar *rdir = NULL;
                guint n = g_list_model_get_n_items(G_LIST_MODEL(a->filter_model));
                FileItem *it = a->cursor_pos < n
                    ? g_list_model_get_item(G_LIST_MODEL(a->filter_model), a->cursor_pos)
                    : NULL;
                if (it && it->is_dir) {
                    const char *rp = a->ssh_conn->remote_path;
                    rdir = strcmp(it->name, "..") == 0 ? g_path_get_dirname(rp)
                         : g_build_filename(rp, it->name, NULL);
                }
                if (it) g_object_unref(it);
                panel_tab_new(a, rdir);
                g_free(rdir);
                return TRUE;
            }
            gchar *name = panel_cursor_name(a);
            gchar *full = panel_cursor_fullpath(a);
            if (name && strcmp(name, "..") == 0 && !a->search_mode) {
                g_free(full);
                full = g_path_get_dirname(a->cwd);
            }
            if (full && g_file_test(full, G_FILE_TEST_IS_DIR))
                panel_tab_new(a, full);
            else
                panel_tab_new(a, NULL);
            g_free(name);
            g_free(full);
            return TRUE;
        }
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
            Panel *a = active_panel(fm), *o = other_panel(fm);
            if (a->ssh_conn || o->ssh_conn) {
                /* remote → remote: load the same path in the other panel */
                if (a->ssh_conn && o->ssh_conn) {
                    gchar *want = g_strdup(a->ssh_conn->remote_path);
                    panel_load(o, want);
                    if (g_strcmp0(o->ssh_conn ? o->ssh_conn->remote_path : NULL,
                                  want) == 0)
                        fm_status(fm, "Panels synchronized");
                    g_free(want);
                } else {
                    fm_status(fm, "Cannot synchronize a local and an SFTP panel");
                }
                return TRUE;
            }
            /* search results: the directory of the found file under the
             * cursor, otherwise the directory the search started from */
            const char *dir = a->cwd;
            FileItem *it = NULL;
            if (a->search_mode) {
                dir = a->search_prev_cwd;
                guint n = g_list_model_get_n_items(G_LIST_MODEL(a->filter_model));
                if (a->cursor_pos < n)
                    it = g_list_model_get_item(G_LIST_MODEL(a->filter_model), a->cursor_pos);
                if (it && it->dir_path) dir = it->dir_path;
                else if (it && it->size_raw == -1) dir = it->name;  /* group header */
            }
            gchar *d = g_strdup(dir);
            if (it) g_object_unref(it);
            panel_load(o, d);
            g_free(d);
            fm_status(fm, "Panels synchronized");
            return TRUE;
        }
        break;

    default:
        break;
    }
    return FALSE;
}

