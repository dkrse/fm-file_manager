# FM Architecture

## Overview

Single-window GTK4 application — a Total Commander clone. One global `FM` struct (stack-allocated in `main.c` as `g_fm`) holds the entire application state and is passed by pointer everywhere.

## Files

| File | Responsibility |
|------|---------------|
| `include/fm.h` | Shared types, all cross-file declarations, `DlgCtx` helper |
| `src/main.c` | `FM` struct, application entry, `panel_setup/load/reload/go_up`, sort comparator, key handlers, CSS, icon name resolving |
| `src/fileitem.c` | `FileItem` GObject subclass (model for column list) |
| `src/fileops.c` | copy/move/delete/mkdir/rename + progress dialog + `on_browse_clicked` |
| `src/search.c` | Recursive glob search dialog, `SearchItem` GObject |
| `src/ssh.c` | Full libssh2 SFTP implementation (guarded `#ifdef HAVE_LIBSSH2`); `ssh_read_file`, `ssh_write_file`; connect dialog with GtkDropDown, `SshDlgData` helpers |
| `src/settings.c` | Persistent settings, font/column/cursor/editor/viewer config, `apply_*_css`; SSH bookmark API (`SshBookmark`, `settings_load/save_ssh_bookmarks`, `ssh_bookmark_free`) |
| `src/viewer.c` | File viewer (F3), local + SSH, max 1 MB |
| `src/editor.c` | Text editor (F4), Ctrl+S saves, line numbers, syntax highlight; local and SFTP (`EditorCtx.ssh_conn + remote_path`) |
| `src/highlight.c` | Syntax highlighting: GtkSourceView wrapper or custom regex highlighter |

## Data Flow

```
FM (g_fm)
├── panels[0..1]  Panel
│   ├── GListStore<FileItem>
│   ├── GtkSortListModel  (wraps store)
│   ├── GtkMultiSelection (wraps sort model)
│   └── GtkColumnView
└── CSS providers (3 priority levels)
    ├── css_provider          APPLICATION+0  (* global GUI font, cursor CSS)
    ├── font_css_provider     APPLICATION+1  (columnview panel font)
    ├── editor_css_provider   APPLICATION+2  (textview.fm-editor + label.fm-editor-status)
    └── viewer_css_provider   APPLICATION+2  (textview.fm-viewer)
```

## Panel Model

Each `Panel` has:
- `GListStore<FileItem>` — raw data
- `GtkSortListModel` — sorts store (synchronous, `incremental=FALSE`)
- `GtkMultiSelection` — selection over sorted model
- `GtkColumnView` — 3 columns (Name, Size, Modified) via `GtkSignalListItemFactory`
- `cursor_pos` — manual cursor position tracking
- `inhibit_sel` — flag preventing `on_selection_changed` from overwriting `cursor_pos` during programmatic selection

**Sort:** Always `..` first, then directories, then files. Within each group, sorted by the active column.

**Selection:** `panel_selection()` returns GTK multi-selection if any rows are selected, otherwise falls back to the cursor row (TC style).

## Key Handling Hierarchy

1. `on_window_key_press` (CAPTURE phase) — Tab (switch panel), F-keys globally (skipped if `GtkEntry` has focus)
2. `on_tree_key_press` (CAPTURE phase, per-panel) — Up/Down/PgUp/PgDn/Home/End, Space/Insert (select toggle), Backspace (go up), Enter (activate), `+` (select all)

## Dialog Pattern

All modal dialogs use `DlgCtx` + `GMainLoop` instead of `gtk_dialog_run`:
```c
DlgCtx ctx; dlg_ctx_init(&ctx);
// build GtkWindow, connect OK/Cancel to dlg_ctx_ok/dlg_ctx_cancel
if (dlg_ctx_run(&ctx) == 1) { /* OK pressed */ }
gtk_window_destroy(GTK_WINDOW(dlg));
```

## CSS Architecture

Four CSS providers with different priorities ensure correct specificity:

| Provider | Priority | Target |
|----------|----------|--------|
| `css_provider` | APPLICATION | `*` global GUI font, cursor (`row:selected`), focus ring suppression |
| `font_css_provider` | APPLICATION+1 | `columnview` panel font |
| `editor_css_provider` | APPLICATION+2 | `textview.fm-editor` text font, `label.fm-editor-status` GUI font |
| `viewer_css_provider` | APPLICATION+2 | `textview.fm-viewer` font |

## Syntax Highlighting

With `HAVE_GTKSOURCEVIEW` (gtksourceview5-devel):
- Editor and viewer use `GtkSourceView` / `GtkSourceBuffer`
- Language auto-detected from extension via `GtkSourceLanguageManager`
- Color scheme configurable in Settings
- Line numbers natively via `gtk_source_view_set_show_line_numbers`

Without GtkSourceView (fallback):
- `highlight.c` — custom GRegex-based highlighter for C/C++, Python, Shell, JS
- Editor: live re-highlight with 300ms debounce (`g_timeout_add`)
- Viewer: one-time on load
- Custom Cairo gutter for line numbers

## Cursor — Focus Behavior

- **Startup:** after `set_active_panel(fm, 0)`, panel[1] is deselected (`inhibit_sel=TRUE` + `unselect_all`)
- **Tab-switch:** before `grab_focus`, `panels[next].inhibit_sel = TRUE` is set → `on_focus_enter` detects it, restores `cursor_pos`, resets flag
- **Mouse click:** `inhibit_sel` is FALSE at `on_focus_enter` → restore is skipped → `on_selection_changed` sets cursor directly to clicked position (no jump)
- **After closing viewer/editor:** `inhibit_sel = TRUE` + `gtk_widget_grab_focus(panel->column_view)` → `on_focus_enter` restores cursor

## SSH Connections — Data Model

```
SshBookmark { name, user, host, port }   // fm.h
settings.ini [ssh]
  connections = name|user|host|port;name2|user2|host2|port2
```

`settings_load_ssh_bookmarks` reads both new format and old `user@host` (backward compat).
`settings_save_ssh_bookmarks` always writes new format.

`SshDlgData` (ssh.c, internal) — SSH connect dialog state:
- `dropdown` + `str_list` — GtkDropDown over GtkStringList
- `block_sel` — flag suppressing `notify::selected` during `ssh_dlg_dropdown_fill`
- `bookmarks` — GList<SshBookmark*>, owned

## Settings

Stored in `~/.config/fm/settings.ini`, section `[display]`:

| Key | Description |
|-----|-------------|
| `font_family`, `font_size` | Panel font |
| `gui_font_family`, `gui_font_size` | GUI font |
| `col_name_width`, `col_size_width`, `col_date_width` | Column widths |
| `show_hidden` | Hidden files |
| `terminal_app` | Terminal emulator |
| `cursor_color`, `cursor_outline` | Cursor style |
| `viewer_font_family`, `viewer_font_size` | Viewer font |
| `editor_font_family`, `editor_font_size` | Editor text font |
| `editor_gui_font_family`, `editor_gui_font_size` | Editor GUI font |
| `editor_line_numbers`, `editor_linenum_font_size` | Line numbers |
| `syntax_highlight` | Syntax highlighting on/off |
| `editor_style_scheme` | Color scheme (GtkSourceView) |

## Dependencies

| Library | Required | Function |
|---------|----------|----------|
| GTK4 >= 4.12 | yes | entire UI |
| GLib 2.x | yes | data structures, GRegex |
| libssh2 | optional | SFTP panel (`#ifdef HAVE_LIBSSH2`) |
| gtksourceview-5 | optional | full syntax highlighting (`#ifdef HAVE_GTKSOURCEVIEW`) |

## Important Notes

- `on_browse_clicked` is defined in `fileops.c`, declared in `fm.h` as non-static — used by `search.c`
- `panel_load()` dispatches to `panel_load_remote()` at the beginning if `p->ssh_conn != NULL`
- `lstat()` everywhere (not `stat()`) — correct symlink detection
- Cross-device move: `rename()` → `EXDEV` → fallback copy+delete
- `g_list_store_splice()` for batch insert (performance with 4000+ files)
- `inhibit_sel` must be set BEFORE each `gtk_column_view_scroll_to(..., GTK_LIST_SCROLL_SELECT, ...)`
- `ssh_write_file` writes in a loop (`libssh2_sftp_write` may return partial write)
- `EditorCtx.filepath` is NULL for SFTP editing; `EditorCtx.remote_path` is NULL for local — always use `title = remote_path ?: filepath`
