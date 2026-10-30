# FM Architecture

## Overview

Single-window GTK4 application (with optional libadwaita for runtime theme switching) — a Total Commander clone. A single global `FM` struct (stack-allocated in `main.c` as `g_fm`) holds the entire application state and is passed everywhere as a pointer.

## Files

| File | Responsibility |
|------|---------------|
| `include/fm.h` | Shared types, all cross-file declarations, `DlgCtx` helper |
| `include/util.h`, `src/util.c` | GLib-only helpers: `fmt_size/fmt_date`, `fs_name_is_safe`, `fs_relpath_is_safe`, `fs_path_is_inside`, `fs_remote_join`, `fs_unique_name`, `arc_detect/arc_extension/arc_with_extension`, `fs_parse_uri_list`, `fs_match_patterns` |
| `include/fsops.h`, `src/fsops.c` | File operation engine (no GTK, worker-thread safe): `fs_copy/move/delete/trash/calc_size` and SFTP `fs_sftp_upload/download/delete/calc_size/exists`; conflicts, progress and cancel via `FsCtx` callbacks |
| `include/job.h`, `src/job.c` | `Job`: worker thread + modal progress dialog (100 ms UI timer), overwrite prompt marshalled to the main thread; `fm_call_on_main()` |
| `include/dialogs.h`, `src/dialogs.c` | `ask_destination`, `ask_text`, `ask_choice`, `dlg_file_list` |
| `src/main.c` | `FM` struct, application entry, `panel_setup/load/reload/go_up`, `panel_selection(_paths)`, `panel_select_name`, sort comparators, key handlers, CSS, incremental search (Ctrl+S), panel + right-click menus, path entry navigation |
| `src/icons.c` | Content-type icon resolving (`icon_for_entry`) |
| `src/monitor.c` | `file_item_from_stat`, directory monitoring (`panel_monitor_start/stop`, `on_dir_changed` incl. renames/moves, `sftp_poll_tick` with try-lock) |
| `src/selection.c` | Marking: `sel_toggle_cursor`, `sel_mark_all`, `sel_invert`, `sel_group_dialog`, `sel_refresh` |
| `src/clipboard.c` | `clip_copy` / `clip_paste` / `clip_copy_paths` (x-special/gnome-copied-files, text/uri-list) |
| `src/fileitem.c` | `FileItem` GObject subclass (model for column list) |
| `src/fileops.c` | Copy/move (`fo_transfer`), duplicate, delete/trash, mkdir, new file, rename, properties — dialogs on the main thread, work in `Job`s |
| `src/archive.c` | Extract / pack: child process watched by a `Job` worker, stderr captured |
| `src/search.c` | Advanced search: glob + content grep + size filter, string-interned results, MC-style grouped panel display; same-filesystem traversal (`-xdev`), symlink loop protection, visited-inode tracking, cancel support |
| `src/ssh.c` | Full libssh2 SFTP implementation (guarded `#ifdef HAVE_LIBSSH2`); `ssh_read_file`, `ssh_write_file`; async connect via `GTask`; connect dialog with GtkDropDown, `SshDlgData` helpers |
| `src/settings.c` | Persistent settings, font/column/cursor/editor/viewer config, theme (dark/light), `apply_*_css`, `apply_theme`; theme-aware color helpers (`fm_visible_color`, `fm_link_color`, `fm_exec_color`, `fm_scheme_for_theme`); SSH bookmark API (`SshBookmark`, `settings_load/save_ssh_bookmarks`, `ssh_bookmark_free`) |
| `src/viewer.c` | File viewer (F3), local + SSH, max 50 MB; search (Ctrl+F) with highlighting and scrollbar indicators; image preview via `GtkPicture` for image files (configurable) |
| `src/editor.c` | Text editor (F4) with menu bar, find/replace (Ctrl+F/H), Ctrl+S saves, line numbers, syntax highlight; local and SFTP (`EditorCtx.ssh_conn + remote_path`) |
| `src/highlight.c` | Syntax highlighting: GtkSourceView wrapper or custom regex highlighter |
| `data/sk.km.fm.desktop` | Desktop entry for application launchers |
| `data/icons/` | Application icons: SVG (scalable) + PNG (16–256px) |

## Build

```
make              → build/fm  (binary + .o files in build/)
make clean        → rm -rf build/
make install      → /usr/local/bin/fm + icons + .desktop
make user-install → ~/.local/bin/fm + ~/.local/share/icons + ~/.local/share/applications
make uninstall    → removes system install
make test         → build/test_core (GLib tests of util.c + fsops.c)
make debug        → build-debug/fm with -fsanitize=address,undefined
make test-asan    → tests under sanitizers
```

## Data flow

```
FM (g_fm)
├── panels[0..1]  Panel
│   ├── GListStore<FileItem>
│   ├── GtkSortListModel    (wraps store)
│   ├── GtkFilterListModel  (wraps sort model — Ctrl+S filter)
│   ├── GtkMultiSelection   (wraps filter model)
│   └── GtkColumnView
└── CSS providers (4, different priority levels)
    ├── css_provider          APPLICATION+0  (* global GUI font, cursor CSS)
    ├── font_css_provider     APPLICATION+1  (columnview panel font)
    ├── editor_css_provider   APPLICATION+2  (textview.fm-editor + label.fm-editor-status)
    └── viewer_css_provider   APPLICATION+2  (textview.fm-viewer)
```

## Panel model

Each `Panel` has:
- `path_entry` — editable path; Enter navigates, Escape restores; supports `~` and relative paths
- `GListStore<FileItem>` — raw data
- `GtkSortListModel` — sorts store (synchronous, `incremental=FALSE`)
- `GtkFilterListModel` — wraps sort model; filters by mask pattern
- `GtkMultiSelection` — selection over filter model
- `GtkColumnView` — 3 columns (Name, Size, Date) via `GtkSignalListItemFactory`
- `cursor_pos` — manual cursor position tracking
- `inhibit_sel` — flag preventing `on_selection_changed` from overwriting `cursor_pos` during programmatic selection
- `filter_bar`, `filter_entry` — Ctrl+S incremental search UI (MC-style jump to file, no filtering)
- `mask_pattern` — glob mask filter (files only, case sensitive)
- `search_mode`, `search_prev_cwd`, `search_btn` — search results mode state
- `default_sorter` — saved sorter for restoring after search mode
- `dir_monitor` — `GFileMonitor` for local auto-refresh (inotify); NULL when not watching
- `poll_id` — `g_timeout` source ID for SFTP auto-refresh polling; 0 when off

**Sort:** Always `..` first, then directories, then files (enforced via `GtkMultiSorter`: a standalone `dirs_first_func` sorter as primary, column view sorter as secondary — direction toggle never affects directory-first ordering). Within each group by active column.

**Marking:** Files/directories are marked via Insert/Space (toggle) or `+` (all). Marks are stored in `Panel.marks` (`GHashTable<gchar*,NULL>`) — independent of GTK selection model. Marks survive panel reload but are cleared on directory change or after file operations (copy/move/delete). `panel_selection()` returns marked items if any exist, otherwise falls back to cursor row (TC style). Marked items are rendered with configurable `mark_color` (default red `#D32F2F`), bold.

### Performance notes

- `name_sort_func` compares `file_item_sort_key()` (lazy collation key) with `strcmp`; `..` is recognised by `size_raw == -2`.
- Marks: `file_item_set_marked()` emits `mark-changed`; every column factory connects it in `watch_bind` / disconnects in `watch_unbind` and re-runs its bind function. Never call `items_changed` for a mark change — it re-sorts the panel.
- `panel_load` reads the directory with `dir_read()` (worker thread + `GCond` wait of `DIR_WAIT_MS`, then a modal Cancel window). FileItems are still built on the main thread. The monitor is stopped while waiting.
- SFTP poll (`sftp_poll_tick`) starts `sftp_poll_thread` (trylock, readdir) and merges in `sftp_poll_apply` on the main thread; `panel_monitor_stop` bumps `poll_gen` and joins the thread before the connection can be closed. Every main-thread SFTP call must hold `conn->lock`.
- Directory monitor events are queued in `Panel.mon_pending` and applied by `monitor_flush` (`MON_BATCH_MS`, reload above `MON_RELOAD_LIMIT`).

## Key handling hierarchy

1. `on_window_key_press` (CAPTURE phase) — Tab (switch panel), F-keys global (incl. Shift+F4/F5/F8), Del/Shift+Del, Alt+Enter (properties), Ctrl+C/X/V, Ctrl+Shift+C, Ctrl+A/Ctrl+Shift+A, Ctrl+S (filter), Alt+E (extract), Alt+P (pack) (skipped if `GtkEntry` has focus)
2. `on_tree_key_press` (CAPTURE phase, per-panel) — Up/Down/PgUp/PgDn/Home/End, Shift+Up/Down (mark while moving), Space/Insert (mark toggle), Backspace (go up), Enter (activate), `+`/`-` (select/unselect group), `*` (invert)
3. `on_path_key_press` (per-panel path_entry) — Enter (navigate to path), Escape (restore original path)
4. `on_filter_key` (CAPTURE phase, filter entry) — Escape (close search bar), Enter (close + activate item), arrows/PgUp/PgDn/Home/End (close, cursor stays)

## Dialog pattern

All modal dialogs use `DlgCtx` + `GMainLoop` instead of `gtk_dialog_run`:
```c
DlgCtx ctx; dlg_ctx_init(&ctx);
// build GtkWindow, connect OK/Cancel to dlg_ctx_ok/dlg_ctx_cancel
if (dlg_ctx_run(&ctx) == 1) { /* OK pressed */ }
gtk_window_destroy(GTK_WINDOW(dlg));
```

## Background jobs (threading model)

File operations never block the main loop:

```
main thread                               worker thread (job.c)
───────────                               ─────────────────────
dialogs (ask_destination, ask_choice)
job_new() → modal progress window
job_run() ── g_thread_new ──────────────▶ JobFunc (xfer/delete/archive)
   nested GMainLoop                         fs_copy / fs_sftp_upload …
   job_tick() every 100 ms  ◀── mutex ───   FsCtx.progress → job->done, file
   Cancel → fs_cancel()     ── atomic ──▶   fs_cancelled() checks
   conflict dialog          ◀─ fm_call_on_main ─ FsCtx.conflict (worker waits)
   g_main_loop_quit         ◀── idle ────   return
reload panels, status line
```

- The progress window is modal, so the panels cannot start a second operation.
- **SFTP sessions are not thread-safe:** `SshConn.lock` (`GRecMutex`) is held by the
  worker for the whole job; the main thread only try-locks (`sftp_poll_tick`,
  `ssh_read_file`, `ssh_write_file`, rename/mkdir/properties) and backs off.
- Engine result codes are ordered by severity: `FS_OK < FS_SKIPPED < FS_CANCELLED < FS_ERR`;
  directories report the worst result of their children (`fs_worst`). Moves delete the
  source only on `FS_OK`.
- Without a `conflict` callback the engine **skips** existing targets — it never
  overwrites silently.

Progress dialog:

```
┌─ Copying ──────────────────────────────────┐
│ Copying…                                   │  phase
│ document.pdf                               │  current file
│ ████████████░░░░░░░░░░  54 %               │  bar (pulses while sizing)
│ 12.3 MB / 22.8 MB — 4.5 MB/s — 00:03 left  │  detail
│                                  [Cancel]  │
└────────────────────────────────────────────┘
```

## Incremental search (Ctrl+S) and file mask

**Ctrl+S — MC-style incremental search:**
- Each panel has a hidden search bar (below file list). Ctrl+S shows it and grabs focus
- Typing jumps cursor to the first matching file (case-insensitive substring match) — the file list is NOT filtered
- Enter closes search bar and activates the item under cursor (`activate_item()`)
- Arrow keys, PageUp/Down, Home/End close search bar (cursor stays on current item)
- Escape closes search bar without action
- Key controller uses `GTK_PHASE_CAPTURE` to intercept Enter/arrows before GtkEntry consumes them

**File mask (panel menu → File mask):**
- `GtkCustomFilter` with `filter_match_func` — evaluates mask pattern
- `..` always passes filter
- `GtkFilterListModel` sits between `GtkSortListModel` and `GtkMultiSelection` in the model chain
- `Panel.mask_pattern` stores glob pattern; NULL when mask is off. Applies to files only (dirs always visible), case sensitive (`fnmatch` without `FNM_CASEFOLD`)

## File search (F2)

Advanced search with results displayed in active panel (MC style):

**Input dialog:** file name (glob), content (text grep), search directory, size min/max with unit dropdown (B/KB/MB/GB). Parameters saved between calls.

**Search engine (`search.c`):**
- Two-phase design for memory efficiency:
  1. Collect lightweight `SearchResult` structs (dir pointer + name + stat data, ~40 bytes each)
  2. Sort by directory, build grouped `FileItem` GObject list with directory headers
- `StringPool` — hash-table-based string interning for directory paths. Millions of files sharing same directory use one allocation
- Content search: `file_contains()` reads files line-by-line, binary detection via null-byte probe on first 512 bytes
- Files only in results (directories appear as group headers)
- `*.*` treated as `*` (DOS/Commander compatibility)
- **Same-filesystem traversal:** `root_dev` (device of search root) checked before recursing — skips `/proc`, `/sys`, `/dev`, NFS mounts etc. (like `find -xdev`)
- **Symlink safety:** directory symlinks not followed (`S_ISLNK` check before recursion); `fstatat(AT_SYMLINK_NOFOLLOW)` for all entries
- **Loop detection:** `GHashTable<(dev,ino)>` tracks visited directories — prevents revisiting via bind mounts or hard-linked dirs
- **Worker thread:** `search_dir` runs in a GThread (`search_thread`); the main thread waits in a nested loop, a 100 ms timer shows "Scanning: N files, M found", and Cancel sets the atomic `cancelled` flag. A second F2 during a scan is refused.

**Panel integration:**
- Sorter disabled (`gtk_sort_list_model_set_sorter(NULL)`) to preserve grouped order
- Store replaced (not cleared) via `gtk_sort_list_model_set_model()` — avoids O(n) signal propagation
- `search_mode` flag changes behavior: Enter navigates to result's directory, Backspace returns, Date column shows dir_path
- `panel_cursor_fullpath()` builds correct path for F3/F4 in search mode
- Exit search button (X icon) in path bar
- On exit: store replaced first (small directory), then sorter restored — prevents sorting millions of items

## Search results lifecycle

Each finished search becomes a `SearchHist` (search.c: result `GListStore`, info text,
needle, origin dir, remembered cursor), kept in a history queue (max 10, newest first).
Panels only borrow pointers: `Panel.cur_search` while results are shown,
`Panel.last_search` after leaving them. `panel_load()` calls `search_detach()` (and
restores the sorter after swapping the store), so any navigation away keeps the results
reachable via `search_restore()` (Alt+←, results button) or `search_history_dialog()`
(Shift+F2). Enter on a result opens it (`gtk_show_uri`); `search_goto_file()`
(Ctrl+PgDn / Ctrl+Enter / Enter on a directory header) loads the directory. The viewer
asks `search_needle()` and `FileItem.line` to open on the matching line.

## Archive support

`fo_extract()` and `fo_pack()` in `fileops.c`:

**Detection:** `arc_detect(name)` returns `ArcType` enum based on file extension (double extensions like `.tar.gz` checked first).

**Execution:** `arc_worker()` in `archive.c` runs inside a `Job`: spawns the tool with `g_spawn_async_with_pipes` (working directory set per call — no process-wide `chdir`), drains stderr (last line shown on failure), polls `waitpid(WNOHANG)` every 50 ms. Cancel sends `SIGTERM` to the child.

**Extract:** Detects format → builds appropriate argv (`tar xf`, `unzip`, `7z x`, `unrar x`) → runs with destination `-C`/`-d`/`-o` flag.

**Pack:** Dialog with format dropdown (6 options), archive name entry, destination entry. Builds argv based on selected format: `tar czf/cjf/cJf/--zstd`, `zip -r`, `7z a`.

## Auto-refresh (directory monitoring)

`panel_monitor_start()` / `panel_monitor_stop()` in `main.c`. Enabled via Settings → Panels → "Auto-refresh panels".

**Local panels — `GFileMonitor` (inotify):**
- `g_file_monitor_directory()` with rate limit 500ms
- `on_dir_changed` callback handles CREATED / DELETED / CHANGES_DONE_HINT / ATTRIBUTE_CHANGED
- Incremental: CREATED → `g_list_store_append`; DELETED → `g_list_store_remove`; CHANGED → remove + re-insert (updates size/date)
- Zero CPU cost while idle (kernel inotify)

**SFTP panels — timer polling:**
- `g_timeout_add_seconds(5, sftp_poll_tick, panel)` — polls every 5 seconds
- `sftp_poll_tick`: opens remote directory with `libssh2_sftp_opendir`, builds `GHashTable` of remote filenames
- Pass 1: remove items no longer on remote (iterate store backwards)
- Pass 2: add new items from remote (check if already in store)
- One network round-trip per tick; CPU cost: O(n) hash lookups

**Lifecycle:** Monitor auto-starts in `panel_load()` and `panel_load_remote()`. Stops in `panel_monitor_stop()` (called at start of each load, on SSH disconnect, on setting toggle).

## Per-panel hamburger menus

Each panel has its own `GtkMenuButton` (☰) in the path bar with a `GSimpleActionGroup` using prefix `p0`/`p1`. Menu items: SSH/SFTP, Create archive, Extract archive, File mask, Search files, Filter files. Each callback sets the panel as active before executing the action. Actions are inserted on the menu button widget itself via `gtk_widget_insert_action_group()`.

## Content-type icons

`icon_for_entry(name, is_dir, is_link, is_exec)` in `main.c` (declared in `fm.h`):
- **Directories:** well-known names (Desktop, Documents, Downloads, Music, Pictures, Videos, Templates, Public, .git) → themed folder icons; others → `folder`
- **Files:** ~80 extensions mapped to freedesktop icon names (image-x-generic, audio-x-generic, video-x-generic, package-x-generic, application-pdf, x-office-document/spreadsheet/presentation, text-html, text-x-csrc, text-x-python, application-javascript, text-x-script, etc.)
- **Special:** symlinks → `emblem-symbolic-link`, executables → `application-x-executable`
- Icon pixel size configurable via `FM.icon_size` (Settings → Panels → Icons, 8–64 px)

## CSS architecture

Four CSS providers with different priorities ensure correct specificity:

| Provider | Priority | Target |
|----------|----------|--------|
| `css_provider` | APPLICATION | `*` global GUI font, cursor (`row:selected`), focus ring suppression |
| `font_css_provider` | APPLICATION+1 | `columnview` panel font |
| `editor_css_provider` | APPLICATION+2 | `textview.fm-editor` text font, `label.fm-editor-status` GUI font |
| `viewer_css_provider` | APPLICATION+2 | `textview.fm-viewer` font |

## Syntax highlighting

With `HAVE_GTKSOURCEVIEW` (gtksourceview5-devel):
- Editor and viewer use `GtkSourceView` / `GtkSourceBuffer`
- Language auto-detected from file extension via `GtkSourceLanguageManager`
- Color scheme configurable in Settings
- Line numbers natively via `gtk_source_view_set_show_line_numbers`

Without GtkSourceView (fallback):
- `highlight.c` — custom GRegex-based highlighter for C/C++, Python, Shell, JS
- Editor: live re-highlight with 300 ms debounce (`g_timeout_add`)
- Viewer: one-time on load
- Custom Cairo gutter for line numbers

## Cursor — focus behavior

- **Start:** after `set_active_panel(fm, 0)` panel[1] is deselected (`inhibit_sel=TRUE` + `unselect_all`)
- **Tab switch:** before `grab_focus`, `panels[next].inhibit_sel = TRUE` is set → `on_focus_enter` detects it, restores `cursor_pos`, resets flag
- **Mouse click:** `inhibit_sel` is FALSE on `on_focus_enter` → restore is skipped → `on_selection_changed` sets cursor directly to clicked position (no jump)
- **After closing viewer/editor:** `inhibit_sel = TRUE` + `gtk_widget_grab_focus(panel->column_view)` → `on_focus_enter` restores cursor
- **After dialogs (SSH connect, mkdir, rename, delete, copy, move):** same `inhibit_sel + grab_focus` pattern restores cursor
- **Window regains focus (Alt-Tab, taskbar):** `GtkEventControllerFocus` on window fires `on_window_focus_in` → `restore_panel_cursor()`
- **After search/mask:** explicit `gtk_widget_grab_focus(p->column_view)` at end of operation

## SSH — async connect

SSH connection runs in a background thread via `GTask`:
1. User fills in host/user/port/password in dialog, clicks Connect
2. `ssh_connect_thread` runs `ssh_conn_new()` off the main thread: DNS (all addresses, IPv4/IPv6), TCP, handshake, **host key check**, auth
   - host key: `~/.ssh/known_hosts` loaded line by line (`libssh2_knownhost_readline`, unparsable lines skipped); `MATCH` → continue, `MISMATCH` → abort with MITM warning, unknown → SHA256 fingerprint prompt on the main thread (`fm_call_on_main`), accepted keys appended (`[host]:port` for non-22 ports)
   - auth order: ssh-agent → key files (`id_ed25519`, `id_ecdsa`, `id_rsa` or the configured key; password reused as passphrase) → password → keyboard-interactive
3. `ssh_connect_done` callback on main thread sets `panel->ssh_conn` and loads remote directory
4. UI stays responsive during the entire connection process

## SSH connections — data model

```
SshBookmark { name, user, host, port, key_path }   // fm.h
settings.ini [ssh]
  connections = name|user|host|port|key_path;name2|user2|host2|port2|key_path2
```

`settings_load_ssh_bookmarks` reads both new format and old `user@host` (backward compat).
`settings_save_ssh_bookmarks` always writes new format.

`SshDlgData` (ssh.c, internal) — SSH connect dialog state:
- `dropdown` + `str_list` — GtkDropDown over GtkStringList
- `block_sel` — flag suppressing `notify::selected` during `ssh_dlg_dropdown_fill`
- `bookmarks` — GList<SshBookmark*>, owned

## Security

- **SSH host keys:** verified against `~/.ssh/known_hosts` (see "SSH — async connect")
- **No silent overwrite / no write-through:** destination files are created with `O_EXCL` after an explicit overwrite decision, so an existing symlink at the destination is replaced, never followed
- **Path traversal guard:** `fs_name_is_safe()` in util.c rejects exact `..`, `.`, `/` in filenames, and empty strings — applied to all file operations (copy, move, delete, rename, mkdir)
- **Command injection prevention:** Terminal launch uses `g_spawn_async()` with argv array (no shell parsing); SSH remote paths, user, and host quoted via `g_shell_quote()`
- **Archive flag injection prevention:** `--` separator before archive path in all extract/pack commands (tar, unzip, 7z, unrar) — prevents malicious filenames from being interpreted as flags
- **Symlink safety:** `copy_file()` opens source with `O_NOFOLLOW` — refuses to follow symlinks during copy
- **Credential hygiene:** SSH password cleared from memory via `explicit_bzero()` before `g_free()` after authentication
- **Temp file safety:** Remote file preview creates unique temp directory via `g_dir_make_tmp()` to prevent symlink attacks; R2R temp cleanup uses recursive `delete_r()`

## Settings

Stored in `~/.config/fm/settings.ini`, section `[display]`:

| Key | Description |
|-----|-------------|
| `font_family`, `font_size` | Panel font |
| `gui_font_family`, `gui_font_size` | GUI font |
| `col_name_width`, `col_size_width`, `col_date_width` | Column widths |
| `show_hidden` | Hidden files |
| `show_hover` | Row hover highlight (default true) |
| `auto_refresh` | Auto-refresh panels via inotify/polling (default false) |
| `icon_size` | File icon pixel size (8–64, default 16) |
| `terminal_app` | Terminal emulator |
| `cursor_color`, `cursor_outline` | Cursor style |
| `dir_color`, `dir_bold` | Directory appearance (color, bold) |
| `mark_color` | Marked files color |
| `viewer_font_family`, `viewer_font_size` | Viewer font |
| `editor_font_family`, `editor_font_size` | Editor text font |
| `editor_gui_font_family`, `editor_gui_font_size` | Editor GUI font |
| `editor_line_numbers`, `editor_linenum_font_size` | Line numbers |
| `syntax_highlight` | Syntax highlighting enabled |
| `viewer_syntax_highlight` | Viewer syntax highlighting |
| `viewer_line_numbers` | Viewer line numbers |
| `viewer_image_preview` | Show image files as pictures in viewer (default true) |
| `editor_style_scheme` | Color scheme (GtkSourceView) |
| `theme` | Theme: 0=dark, 1=light (default 1) |

## Dependencies

| Library | Required | Function |
|---------|----------|----------|
| GTK4 >= 4.12 | yes | entire UI |
| GLib 2.x | yes | data structures, GRegex |
| libadwaita-1 | optional | runtime dark/light theme switching (`#ifdef HAVE_LIBADWAITA`); uses `AdwApplication` + `AdwStyleManager` |
| libssh2 | optional | SFTP panel (`#ifdef HAVE_LIBSSH2`) |
| gtksourceview-5 | optional | full syntax highlighting (`#ifdef HAVE_GTKSOURCEVIEW`) |

## Important notes

- `on_browse_clicked` is defined in `fileops.c`, declared in `fm.h` as non-static — used by `search.c`
- `panel_load()` dispatches to `panel_load_remote()` at the start if `p->ssh_conn != NULL`
- `lstat()` everywhere (not `stat()`) — correct symlink detection
- Cross-device move: `rename()` → `EXDEV` → fallback copy+delete
- `g_list_store_splice()` for batch insert (performance with 4000+ files)
- Store replacement via `gtk_sort_list_model_set_model()` instead of `g_list_store_remove_all()` — critical for large lists (search results with millions of items)
- `FileItem.dir_path` — used in search mode to store the directory containing the file; NULL for normal directory listings and directory header rows
- `inhibit_sel` must be set BEFORE each `gtk_column_view_scroll_to(..., GTK_LIST_SCROLL_SELECT, ...)`
- `ssh_write_file` writes in a loop (`libssh2_sftp_write` may return partial write)
- `EditorCtx.filepath` is NULL for SFTP editing; `EditorCtx.remote_path` is NULL for local — always use `title = remote_path ?: filepath`
- `do_path_activate`: text from `gtk_editable_get_text` must be copied (`g_strdup`) before calling `panel_load`, because it modifies the entry and invalidates the internal buffer
- SFTP directory operations: `sftp_upload_dir_r` / `sftp_download_dir_r` recurse locally/remotely; `sftp_delete_r` walks remote dirs depth-first
- Remote→Remote (SFTP→SFTP) copy/move: download to temp dir (`g_dir_make_tmp`), upload to destination, clean up temp; progress tracks both phases (total_bytes × 2)
- Mark re-render: Insert/Space uses `g_list_store_find` + `g_object_ref` + remove + insert to force view re-bind; `+` (all) uses `panel_reload` (marks survive in hash table)
- `ssh_dlg_dropdown_fill` must be called AFTER all form entry widgets are created (otherwise `ssh_dlg_fill_form` writes to NULL widgets)
- Cancel during copy/move cleans up partial files (`unlink` on incomplete destination)
- Cancel during delete stops immediately — already-deleted files remain deleted
- Archive commands run async via `g_spawn_async` + `waitpid(WNOHANG)` poll loop — cancel sends `SIGTERM`
- Filter model uses `filter_model` (not `sort_model`) for all item access: `panel_cursor_name`, `panel_selection`, cursor navigation, search result lookup
- `FM.vis_mark_color` — cached theme-adjusted mark color, updated at settings load/save; used directly in bind callbacks (zero allocation per row)
- `fmt_date()` uses `localtime_r()` (thread-safe POSIX variant, not `localtime()`)
- `icon_for_entry()` returns static string constants — safe for `FileItem.icon_name` (not freed in finalize)
- `FileItem.fg_color` is now `g_strdup`'d and freed in finalize — required because `fm_visible_color()` returns allocated strings for theme-adjusted colors
- `fm_visible_color(fm, hex)` inverts colors that are not visible on the current theme background (dark-on-dark or light-on-light); caller must `g_free()` the result
- `fm_scheme_for_theme(fm)` auto-selects GtkSourceView dark scheme: tries `<base>-dark`, then `Adwaita-dark`, `oblivion`, `cobalt`; returns static string (do not free)
- `apply_theme()` uses `AdwStyleManager` when available (reliable runtime switching); falls back to `gtk-application-prefer-dark-theme` (startup only, dark→light unreliable without libadwaita)
- Ctrl+S search bar key controller uses `GTK_PHASE_CAPTURE` — required because GtkEntry otherwise consumes Enter/arrows before the handler sees them
- `sync_cursor_from_selection()` — called before all keyboard actions in `on_tree_key_press` to sync `cursor_pos` from actual GTK selection; fixes mouse click + keyboard desync on SFTP panels where `on_selection_changed` may not reliably fire after `panel_load_remote`
- SSH authentication tries password first, then public key (`key_path` from bookmark or default `~/.ssh/id_rsa`)
