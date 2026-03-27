# Changelog

## [2.7.0] — 2026-03-27

### Security fixes, progress dialog redesign, cancel support, async SSH, app icon

**Security fixes:**
- **Command injection prevention:** Terminal launch rewritten to use `g_spawn_async()` with argv array instead of `g_spawn_command_line_async()` (shell string parsing); SSH remote paths quoted via `g_shell_quote()`
- **Temp file race condition:** Remote file preview now creates unique temp directory via `g_dir_make_tmp("fm-view-XXXXXX")` instead of writing directly to `/tmp/filename` (prevents symlink attacks)
- **Path traversal guard:** New `name_is_safe()` function rejects filenames containing `/`, `..`, or empty — applied to copy, move, delete, rename, mkdir

**Progress dialog redesign:**
- New layout: phase label + current file + progress bar + detail line (size + speed) + Cancel button
- Two modes: indeterminate (auto-pulsing 80ms timer) and determinate (fraction + percentage + MB/s)
- Animated "Calculating size…" phase — GTK events pumped every 64–128 files during recursive size calculation, keeping the pulse animation alive
- Transfer speed display after 0.5 seconds elapsed

**Cancel support:**
- Cancel button in all progress dialogs (copy, move, delete)
- Cancellation checked at multiple levels: read/write loops (every 256KB), recursive traversal, main file loop, size calculation phase
- Partial files cleaned up on cancel (incomplete destinations deleted)
- Status bar shows "Copy cancelled (N copied, M errors)" etc.

**Async SSH connect:**
- SSH connection (DNS + TCP + handshake + auth) runs in background thread via `GTask`
- UI stays responsive during connection — no more frozen window

**Application icon and desktop integration:**
- New SVG icon (dual-panel file manager) + PNG in all sizes (16–256px)
- Desktop entry file (`data/sk.km.fm.desktop`)
- Makefile: new `user-install`, `uninstall` targets; `install` now includes icons + .desktop

**Build changes:**
- Binary output moved to `build/fm` (was `./fm` in project root)
- `make clean` removes entire `build/` directory

---

## [2.6.0] — 2026-03-12

### File marking, remote-to-remote copy/move, directory appearance settings, sort fix

**File marking (Insert/Space):**
- Proper Total Commander-style file marking — independent of GTK selection model
- Insert / Space toggles mark on current file (colored + bold) and moves cursor down
- `+` marks/unmarks all files
- Marked files are used for Copy (F5), Move (F6), Delete (F8); if no marks, cursor item is used
- Marks survive panel reload but are cleared on directory change or after operations
- Marks stored in `Panel.marks` (`GHashTable`) — per-panel, persistent across reloads
- Marked items rendered with configurable color (default red `#D32F2F`)
- Mark color configurable in Settings → Panels → "Marked files (Insert/Space)"
- Status bar shows mark count ("N marked | M items")

**Remote→Remote copy/move (SFTP→SFTP):**
- Copy and move between two different SSH connections now works
- Implementation: download to temp directory, upload to destination, clean up temp
- Progress bar tracks both download and upload phases (total bytes × 2)
- Move additionally deletes source after successful transfer

**Directory appearance in Settings:**
- New "Directories" section in Settings → Panels tab
- Configurable directory color (default `#1565C0` blue)
- Configurable bold/normal font for directories
- Applied to both local and SFTP panels

**Sort fix — directories always on top:**
- Directories now stay on top regardless of sort direction (ascending or descending)
- Implementation: `GtkMultiSorter` with standalone `dirs_first_func` as primary sorter, column view sorter as secondary — direction toggle only affects the secondary sorter

---

## [2.5.0] — 2026-03-11

### Recursive SFTP directory operations, SSH dialog and cursor fixes

**Recursive SFTP directory operations:**
- Copy directories with all subdirectories and files over SFTP (upload and download)
- Move directories over SFTP (copy + recursive delete)
- Delete directories recursively over SFTP with progress updates
- Mkdir (F7) now works on SFTP panels via `libssh2_sftp_mkdir`
- Rename (F9) now works on SFTP panels via `libssh2_sftp_rename`
- New helpers: `sftp_calc_size_r`, `sftp_upload_dir_r`, `sftp_download_dir_r`, `sftp_delete_r`

**SSH connect dialog fixes:**
- Dropdown: form fields now populate immediately when dialog opens (moved `ssh_dlg_dropdown_fill` after entry widgets are created)
- Dropdown: explicit `ssh_dlg_fill_form` call after `set_selected` — fixes case where `notify::selected` signal doesn't fire when index is already 0
- Signal connected before dropdown fill to ensure initial selection fires callback

**Cursor fixes:**
- Cursor no longer becomes unresponsive after SSH connect — `inhibit_sel + grab_focus` added to `ssh_connect_dialog`
- Cursor no longer becomes unresponsive after F7 mkdir — `inhibit_sel + grab_focus` added to `fo_mkdir`
- Same fix applied to `fo_rename`, `fo_delete`, `fo_copy`, `fo_move` for consistency

---

## [2.4.0] — 2026-03-02

### Editable path, find/replace in editor and viewer, menu bar

**Editable path in panels:**
- Path entry at the top of each panel shows the current path
- Manual path change + Enter loads the directory
- Support for `~` expansion (home directory)
- Support for relative paths (resolved against current directory)
- Escape restores the original path
- Works for SFTP panels too (sftp://user@host/path or just /path)

**Find and replace in editor:**
- Menu bar: File (Save, Close), Edit (Undo, Redo, Cut, Copy, Paste), Find (Find, Replace, Find next/prev.)
- Ctrl+F opens search panel, Ctrl+H also opens replace row
- All occurrences highlighted in yellow
- Match position indicators on vertical scrollbar (red marks)
- "X of Y" match counter
- F3 / Shift+F3 navigation between matches

**Search in viewer:**
- Ctrl+F opens search panel (no replace - read-only)
- Same highlighting and scrollbar indicators as in editor
- F3 / Shift+F3 navigation

**Fixes:**
- Path entry: fixed use-after-free during navigation (gtk_editable_get_text returns internal buffer)

---

## [2.3.0] — 2026-03-01

### Editor for SFTP, saved SSH connections, cursor fixes

**Text editor via SFTP:**
- Editor (F4) works on remote (SFTP) panel too — reading via `ssh_read_file`, saving via `ssh_write_file`
- New function `ssh_write_file` in `ssh.c` (real + stub)
- `EditorCtx` extended with `remote_path` and `ssh_conn`; window title, `●` modifier and syntax highlighting use the remote path

**Saved SSH connections:**
- Replaced simple `user@host` format with full named connections (`SshBookmark`: name, user, host, port)
- Stored in `settings.ini` under key `[ssh] connections`, format `name|user|host|port;...`
- Backward compatibility: old `user@host` format is loaded and converted
- SSH connect dialog redesigned: GtkDropDown for connection selection, form (Name/Host/User/Port/Password), buttons New / Save / Remove / Close / Connect
- Password is **not saved** (security)
- New API: `ssh_bookmark_free`, `settings_load_ssh_bookmarks`, `settings_save_ssh_bookmarks`

**Cursor fixes:**
- At startup: only the active panel (left) shows cursor — panel[1] is deselected right after `set_active_panel`
- Mouse click on inactive panel: removed jump to old position before clicked location — `on_focus_enter` now distinguishes mouse (inhibit_sel=FALSE → skip restore) from Tab switch (inhibit_sel=TRUE → restore cursor_pos)
- After closing viewer/editor: cursor is restored to the original file — `grab_focus` with `inhibit_sel=TRUE` triggers restore in `on_focus_enter`

---

## [2.2.0] — 2026-03-01

### Text editor (F4), Syntax highlighting, Viewer font

**Text editor:**
- New built-in text editor on F4 (replaces SSH dialog, which moved to a separate button)
- Classic window without toolbar — Ctrl+S saves, title shows `●` for unsaved changes
- Optional line numbers (Cairo gutter) with configurable font size
- Status bar shows line and column number

**Syntax highlighting:**
- GtkSourceView 5 integration as optional dependency (`sudo dnf install gtksourceview5-devel`)
- Automatic language detection from file extension — hundreds of languages via system `.lang` files
- Color scheme selectable in Settings (classic, oblivion, solarized-dark, kate, ...)
- Without GtkSourceView: fallback — custom regex highlighter for C/C++, Python, Shell, JS
- Highlighting active in editor (live, 300 ms debounce) and viewer

**Viewer:**
- Configurable font and size (new "Viewer" tab in Settings)
- CSS class `fm-viewer` for isolated styling

**Settings — tabbed layout:**
- Dialog redesigned as `GtkNotebook` with tabs: Panels / Cursor / Viewer / Editor / System
- Dialog width expanded to 520 px
- Editor: text font, editor GUI font, line number font size, highlighting toggle, color scheme
- Viewer: font and size

**Fixes:**
- Cursor when navigating with arrow keys after `panel_go_up` was jumping multiple rows — fixed by disabling `gtk_sort_list_model_set_incremental` (was causing race condition between async sort and cursor restore)
- `apply_editor_css` was not called after saving settings — editor font was not applied
- CSS specificity: `window.fm-editor-win *` was overriding `textview.fm-editor` font-size — fixed with targeted `label.fm-editor-status`

---

## [2.1.0] — 2026-02-28

### Cursor, performance, search

**Cursor in panels:**
- Configurable cursor style: filled color (with inverse text color) or outline only
- Cursor color selection via `GtkColorDialogButton`
- GTK focus ring suppression (`row:focus-visible { outline: none }`)
- `inhibit_sel` flag — prevents `on_selection_changed` from overwriting `cursor_pos` during programmatic selection
- Cursor position remembered when switching panels (Tab, mouse click)

**Performance:**
- `panel_load`: replaced 4000× `g_list_store_append` with single `g_list_store_splice` — dramatic speedup for large directories
- `fstatat(dirfd, ...)` instead of `stat(fullpath, ...)` — fewer string allocations

**Search:**
- Removed 4096 result limit
- Maximum recursion depth increased from 20 to 64

---

## [2.0.0] — 2026-02-28

### Complete rewrite to GTK4

**Main changes:**
- Migration from GTK3 to GTK4 (minimum 4.12+)
- `GtkTreeView` + `GtkListStore` replaced by `GtkColumnView` + `GListStore<FileItem>`
- `GtkCellRenderer` replaced by `GtkSignalListItemFactory` (setup/bind callbacks)
- `GtkDialog` + `gtk_dialog_run()` replaced by `GtkWindow` + `DlgCtx` (GMainLoop pattern)
- `GtkStatusbar` replaced by `GtkLabel`
- Icons: `GdkPixbuf` cache replaced by icon name strings

**New features:**
- Terminal button in panel — opens terminal in current directory (local and SSH)
- Terminal application selection in settings
- Progress bar with byte-level tracking for copy and move
- SSH download: file sizes via SFTP stat for accurate progress

**Fixes:**
- Use-after-free in `panel_load_remote` after copying
- Progress bar was not showing — added event loop pump after `gtk_window_present`

---

## [1.0.0] — 2026-02-27

### First version (GTK3)

- Dual-panel file manager (Total Commander style)
- Copy, move, delete, mkdir, rename (F5–F9)
- Recursive search (F2) with glob pattern
- File viewer (F3), max 1 MB, UTF-8
- SSH/SFTP connection via libssh2 — browsing, upload, download
- Settings: panel font, GUI font, column widths, hidden files
- SSH bookmarks (stored in settings.ini)
- Sorting: .. always first, directories before files
- Keyboard shortcuts: Tab, F-keys, Space/Insert, Ctrl+H, Ctrl+R, Ctrl+=
