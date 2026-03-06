# Changelog

## [2.3.0] — 2026-03-01

### Editor for SFTP, saved SSH connections, cursor fixes

**Text editor over SFTP:**
- Editor (F4) now works on the remote (SFTP) panel — reads via `ssh_read_file`, saves via `ssh_write_file`
- New `ssh_write_file` function in `ssh.c` (real + stub)
- `EditorCtx` extended with `remote_path` and `ssh_conn`; window title, `●` modifier and syntax highlighting use the remote path

**Saved SSH connections:**
- Replaced simple `user@host` format with full named connections (`SshBookmark`: name, user, host, port)
- Stored in `settings.ini` under key `[ssh] connections`, format `name|user|host|port;...`
- Backward compatibility: old `user@host` format is read and converted
- SSH connect dialog redesigned: GtkDropDown for connection selection, form (Name/Host/User/Port/Password), buttons New / Save / Delete / Close / Connect
- Password is **not saved** (security)
- New API: `ssh_bookmark_free`, `settings_load_ssh_bookmarks`, `settings_save_ssh_bookmarks`

**Cursor fixes:**
- At startup: only the active panel (left) shows cursor — panel[1] is deselected right after `set_active_panel`
- Mouse click on inactive panel: removed cursor jump to old position before click target — `on_focus_enter` now distinguishes mouse (inhibit_sel=FALSE → skip restore) from Tab-switch (inhibit_sel=TRUE → restore cursor_pos)
- After closing viewer/editor: cursor is restored to the original file — `grab_focus` with `inhibit_sel=TRUE` triggers restore in `on_focus_enter`

---

## [2.2.0] — 2026-03-01

### Text editor (F4), Syntax highlighting, Viewer font

**Text editor:**
- New built-in text editor on F4 (replaces SSH dialog, which moved to a standalone button)
- Classic window without toolbar — Ctrl+S saves, title shows `●` for unsaved changes
- Optional line numbers (Cairo gutter) with configurable font size
- Status bar shows line and column number

**Syntax highlighting:**
- GtkSourceView 5 integration as optional dependency (`sudo dnf install gtksourceview5-devel`)
- Automatic language detection from file extension — hundreds of languages via system `.lang` files
- Color scheme selection (classic, oblivion, solarized-dark, kate, ...) in Settings
- Without GtkSourceView: fallback — custom regex highlighter for C/C++, Python, Shell, JS
- Highlighting active in editor (live, 300ms debounce) and viewer

**Viewer:**
- Configurable font and size (new "Viewer" tab in Settings)
- CSS class `fm-viewer` for isolated styling

**Settings — tabbed layout:**
- Dialog redesigned as `GtkNotebook` with tabs: Panels / Cursor / Viewer / Editor / System
- Dialog width expanded to 520 px
- Editor: text font, editor GUI font, line number font size, highlighting toggle, color scheme
- Viewer: font and size

**Fixes:**
- Cursor after arrow key navigation following `panel_go_up` was skipping multiple rows — fixed by disabling `gtk_sort_list_model_set_incremental` (was causing race condition between async sort and cursor restore)
- `apply_editor_css` was not called after saving settings — editor font was not applied
- CSS specificity: `window.fm-editor-win *` was overriding `textview.fm-editor` font-size — fixed with targeted `label.fm-editor-status`

---

## [2.1.0] — 2026-02-28

### Cursor, performance, search

**Panel cursor:**
- Configurable cursor style: filled color (with inverse text color) or outline only
- Cursor color selection via `GtkColorDialogButton`
- Suppressed GTK focus rings (`row:focus-visible { outline: none }`)
- `inhibit_sel` flag — prevents `on_selection_changed` from overwriting `cursor_pos` during programmatic selection
- Cursor position remembered when switching panels (Tab, mouse click)

**Performance:**
- `panel_load`: replaced 4000× `g_list_store_append` with a single `g_list_store_splice` — dramatic speedup for large directories
- `fstatat(dirfd, ...)` instead of `stat(fullpath, ...)` — fewer string allocations

**Search:**
- Removed 4096 result limit
- Maximum recursion depth increased from 20 to 64

---

## [2.0.0] — 2026-02-28

### Complete rewrite to GTK4

**Major changes:**
- Migration from GTK3 to GTK4 (minimum 4.12+)
- `GtkTreeView` + `GtkListStore` replaced with `GtkColumnView` + `GListStore<FileItem>`
- `GtkCellRenderer` replaced with `GtkSignalListItemFactory` (setup/bind callbacks)
- `GtkDialog` + `gtk_dialog_run()` replaced with `GtkWindow` + `DlgCtx` (GMainLoop pattern)
- `GtkStatusbar` replaced with `GtkLabel`
- Icons: `GdkPixbuf` cache replaced with icon name strings

**New features:**
- Terminal button in panel — opens terminal in the current directory (local and SSH)
- Terminal application selection in settings
- Progress bar with byte-level tracking for copy and move
- SSH download: file sizes via SFTP stat for accurate progress

**Fixes:**
- Use-after-free in `panel_load_remote` after copying
- Progress bar was not displaying — added event loop pump after `gtk_window_present`

---

## [1.0.0] — 2026-02-27

### First version (GTK3)

- Dual-panel file manager (Total Commander style)
- Copy, move, delete, mkdir, rename (F5–F9)
- Recursive search (F2) with glob pattern
- File viewer (F3), max 1 MB, UTF-8
- SSH/SFTP connection via libssh2 — browsing, upload, download
- Settings: panel font, GUI font, column widths, hidden files
- SSH bookmarks (saved in settings.ini)
- Sorting: .. always first, directories before files
- Keyboard shortcuts: Tab, F-keys, Space/Insert, Ctrl+H, Ctrl+R, Ctrl+=
