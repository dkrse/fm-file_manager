# FM — Project Guide for Claude

## Build

```bash
make          # build (auto-detects libssh2, gtksourceview5)
make clean    # clean
```

## Architecture

Single-window GTK4 dual-panel file manager (Total Commander clone) in C. One global `FM` struct holds all state.

Key files: `include/fm.h` (types), `src/main.c` (window/panels/keys), `src/fileops.c` (file operations + recursive SFTP helpers), `src/ssh.c` (SFTP + connect dialog), `src/settings.c` (config), `src/viewer.c` (F3), `src/editor.c` (F4), `src/highlight.c` (syntax), `src/fileitem.c` (GObject model), `src/search.c` (F2).

## Key patterns

- **Dialog pattern:** `DlgCtx` + `GMainLoop` (no `gtk_dialog_run`)
- **Cursor restore after dialogs:** always `p->inhibit_sel = TRUE; gtk_widget_grab_focus(p->column_view);` after `gtk_window_destroy`
- **SFTP guards:** `#ifdef HAVE_LIBSSH2` / `#endif`
- **GtkSourceView guards:** `#ifdef HAVE_GTKSOURCEVIEW` / `#endif`
- **SSH dropdown:** `ssh_dlg_dropdown_fill` must be called AFTER all form entries are created; explicit `ssh_dlg_fill_form` needed after `set_selected` (signal may not fire if index unchanged)

## Language

All UI strings and documentation in English.

## Docs

- `docs/architecture.md` — detailed architecture
- `docs/changelog.md` — changelog (latest: 2.5.0)
