#ifndef FM_DIALOGS_H
#define FM_DIALOGS_H

/* Small modal dialogs shared by fileops.c, archive.c and selection.c.
 * All run a nested main loop (DlgCtx pattern) and return the answer. */

#include "fm.h"

/* Total Commander style "Copy 3 files to:" dialog: the entry holds the
 * target directory, is focused and fully selected – Enter = OK, Esc =
 * Cancel.  `prompt` is Pango markup.  Returns the typed path without a
 * trailing slash, or NULL when cancelled. */
gchar *ask_destination(FM *fm, const char *title, const char *prompt,
                       const char *default_dst);

/* "Copy 'name' to:" / "Copy 3 files/directories to:" (markup, caller frees) */
gchar *dlg_count_prompt(const char *verb, GList *paths, const char *suffix);

/* Yes / No question, Yes focused (Enter), No on Esc; Y/N keys work too */
gboolean ask_yes_no(FM *fm, const char *title, const char *markup);

/* Single-line text entry; `prompt` is Pango markup (escape user data!).
 * select_stem: preselect name without extension (rename). NULL = cancelled */
gchar *ask_text(FM *fm, const char *title, const char *prompt,
                const char *initial, const char *ok_label, gboolean select_stem);

/* Buttons are listed left→right, the last one is the default action.
 * Returns the index of the clicked button, or -1 when closed.
 * destructive: index styled as destructive-action (-1 = none). */
int ask_choice(FM *fm, const char *title, const char *markup, GList *files,
               const char *const *buttons, int destructive);

/* Scrollable list of names (dim labels) for confirmation dialogs */
GtkWidget *dlg_file_list(GList *files);

/* F1 – simple keyboard shortcut list (Esc closes) */
void help_show(FM *fm);

#endif /* FM_DIALOGS_H */
