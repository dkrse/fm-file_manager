# FM — Total Commander for Linux

Dual-panel file manager inspired by Total Commander, written in C with GTK4.

## Features

- **Two panels** — navigation like in Total Commander
- **Editable path** — path entry at the top of each panel, Enter loads directory, supports ~ and relative paths
- **SFTP panel** — connect to a remote server via SSH/SFTP (libssh2), async connect (non-blocking UI); host keys verified against `~/.ssh/known_hosts`; ssh-agent, ed25519/ecdsa/rsa keys, encrypted keys, password and keyboard-interactive auth; IPv4 + IPv6
- **File marking** — Insert/Space or Shift+arrows mark files; `+`/`-` select/unselect group by pattern (`*.c;*.h`), `*` inverts, Ctrl+A all; status bar shows count and total size of marked files
- **Copy, move, delete** — Total Commander style: F5 → path of the other panel is pre-selected, Enter starts copying (type `dir/newname` to rename a single item); F8 → Yes/No confirmation; operations run in a background thread with a From/To progress window (current file + total bar, speed, time left, Esc = cancel); overwrite prompt (Overwrite / Overwrite all / Skip / Skip all / Cancel); permissions, timestamps and symlinks preserved; directories merged; copying a directory into itself is refused; full recursive SFTP support incl. remote-to-remote
- **Trash** — F8/Del moves local files to the trash (freedesktop), Shift+Del / Shift+F8 deletes permanently
- **Clipboard** — Ctrl+C / Ctrl+X / Ctrl+V interoperable with other file managers (Nautilus, Dolphin, …); paste into an SFTP panel uploads
- **Context menu** — right click on a file: copy, cut, paste, duplicate, rename, delete, new folder/file, properties, selection
- **Properties** — Alt+Enter: size (directories computed in background), owner, dates, symlink target, editable permissions (also over SFTP)
- **New file / duplicate** — Shift+F4 creates a file and opens the editor; Shift+F5 copies in place under a new name; F7 accepts nested paths (`a/b/c`)
- **Archive support** — extract and create archives (tar.gz, tar.bz2, tar.xz, tar.zst, zip, 7z, rar)
- **Incremental search** — Ctrl+S, MC-style jump-to-file search (cursor jumps to matching file, list not filtered)
- **File mask** — glob pattern display filter on active panel (files only, e.g. `*.c`)
- **File search** — F2, recursive search with content grep, size filter, MC-style grouped results in panel. Results are kept: Enter opens a file in its default app, Ctrl+PgDn / Ctrl+Enter jumps to the file's directory, Alt+← (or the 🔍 button) returns to the results, Shift+F2 lists the last 10 searches. Content search shows `:line  matching text` next to each file; F3 opens the viewer on that match with the text highlighted (F3 / Shift+F3 = next / previous)
- **File viewer** — F3, displays text (max 50 MB) with configurable font and search (Ctrl+F); image preview for PNG/JPG/GIF/SVG/WebP/BMP/TIFF (toggleable in Settings)
- **Text editor** — F4, built-in editor with menu bar, find/replace (Ctrl+F/H), syntax highlighting; works on SFTP panel too
- **Syntax highlighting** — GtkSourceView (hundreds of languages) or built-in highlighter (C/Python/Shell/JS)
- **Content-type icons** — files and folders shown with freedesktop standard icons based on extension (~80 types)
- **SSH terminal** — open terminal directly in the current directory (local and SSH)
- **Saved SSH connections** — named connections (name/host/user/port/key) in settings.ini, selectable via dropdown; custom private key path per connection
- **Auto-refresh panels** — automatic directory monitoring via inotify (local) or polling (SFTP); incremental updates (add/remove/update individual files, no full reload)
- **Per-panel hamburger menu** — SSH/SFTP, Create/Extract archive, File mask, Search, Filter — in each panel's path bar
- **Dark / Light theme** — runtime switching via Settings → System (libadwaita); colors auto-adapt (directories, symlinks, marks, editor scheme)
- **Settings** — tabbed dialog: Panels / Display / Cursor / Viewer / Editor / System

## Keyboard shortcuts

### Panel
| Key | Action |
|-----|--------|
| Tab | Switch active panel |
| Enter | Open file/directory (or confirm path in path entry) |
| Escape | Cancel path editing / close filter |
| Backspace | Go up one level |
| Space / Insert | Mark file + move cursor |
| Shift+Up / Shift+Down | Mark while moving |
| + / Num+ | Select group (pattern) |
| - / Num- | Unselect group |
| * / Num* | Invert selection |
| Ctrl+A / Ctrl+Shift+A | Mark all / unmark all |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut / paste (system clipboard) |
| Ctrl+Shift+C | Copy full path(s) as text |
| Alt+Enter | Properties / permissions |
| Right click | Context menu |
| F1 | Keyboard shortcut help |
| F2 | Search files |
| Shift+F2 | Search history (last 10 searches) |
| Ctrl+PgDn / Ctrl+Enter | In results: go to the file's directory |
| Alt+← | Back to the last search results |
| F3 | View file |
| F4 | Text editor |
| Shift+F4 | New file (opens editor) |
| F5 | Copy |
| Shift+F5 | Duplicate in the same directory |
| F6 | Move |
| Shift+F6 | Rename |
| F7 | New directory (nested `a/b/c` allowed) |
| F8 / Del | Delete (local: move to trash) |
| Shift+F8 / Shift+Del | Delete permanently |
| F10 | Quit |
| Ctrl+S | Incremental search (MC-style jump to file) |
| Ctrl+H | Hidden files |
| Ctrl+R | Refresh both panels |
| Ctrl+= | Synchronize panels |
| Alt+E | Extract archive |
| Alt+P | Create archive (pack) |

### Editor / Viewer
| Key | Action |
|-----|--------|
| Ctrl+S | Save (editor) |
| Ctrl+F | Find |
| Ctrl+H | Replace (editor only) |
| F3 | Find next |
| Shift+F3 | Find previous |
| Escape | Close search panel |

## Requirements

- GTK4 (>= 4.12)
- GLib 2.x
- libadwaita-1 (optional — for runtime dark/light theme switching)
- libssh2 (optional — for SFTP panel)
- gtksourceview-5 (optional — for full syntax highlighting)

### Archive tools (optional, for extract/pack)

- `tar` — tar.gz, tar.bz2, tar.xz, tar.zst, tar
- `zip` / `unzip` — zip archives
- `7z` — 7-Zip archives
- `unrar` — RAR archives (extract only)

## Installation

### Fedora

```bash
sudo dnf install gtk4-devel
sudo dnf install libadwaita-devel     # optional, for runtime dark/light theme
sudo dnf install libssh2-devel        # optional, for SFTP
sudo dnf install gtksourceview5-devel # optional, for syntax highlighting
sudo dnf install p7zip unrar          # optional, for 7z/rar archives
```

### Compilation

```bash
make                  # compile (output: build/fm)
build/fm              # run
sudo make install     # install to /usr/local/bin/fm + icons + .desktop
make user-install     # install for current user (~/.local/)
make uninstall        # remove system install
make clean            # clean build artifacts
make test             # unit tests of the file-operation core (no display needed)
make test-asan        # the same under AddressSanitizer + UBSan
make test-sftp        # SFTP tests against a private throwaway sshd (skipped without sshd)
make debug            # debug build with sanitizers (build-debug/fm)
```

CI (`.github/workflows/ci.yml`) builds with `-Werror`, runs the tests (also under sanitizers) and checks a minimal build without optional libraries.

## Project structure

```
include/fm.h        — shared types and declarations
include/util.h      — GLib-only helpers (formatting, path safety, archive types, clipboard parsing)
include/fsops.h     — file operation engine API (thread-safe, GTK-free)
src/main.c          — window, panels, keys, CSS, filter, panel + context menus
src/fileitem.c      — FileItem GObject (model for file list)
src/fileops.c       — copy/move/delete/mkdir/new file/rename/properties (dialogs + jobs)
src/fsops.c         — engine: local + SFTP copy/move/delete, conflicts, progress, cancel
src/util.c          — pure helpers used by the engine and the UI
src/job.c           — worker thread + modal progress dialog + overwrite prompt
src/dialogs.c       — shared dialogs (destination, text entry, multi-button choice)
src/archive.c       — extract / pack via tar, zip, 7z, unrar
src/selection.c     — marking: group select, invert, select all
src/clipboard.c     — system clipboard copy/cut/paste
src/monitor.c       — auto-refresh (inotify / SFTP polling)
src/icons.c         — content-type icons
tests/test_core.c   — unit tests (GLib test framework)
src/search.c        — file search
src/ssh.c           — SSH/SFTP connection and browsing
src/settings.c      — settings (font, cursor, editor, viewer, terminal, icon size, theme)
src/viewer.c        — file viewer (F3)
src/editor.c        — text editor (F4)
src/highlight.c     — syntax highlighting (GtkSourceView wrapper / custom regex)
data/sk.km.fm.desktop — desktop entry
data/icons/         — application icons (SVG + PNG 16–256px)
docs/architecture.md — detailed architecture
docs/changelog.md   — changelog
build/              — compiled binary and object files
Makefile            — build system
```

## Settings

Stored in `~/.config/fm/settings.ini`. Configuration via Settings dialog (gear icon):

**Panels** — panel font, GUI font, column widths (name/size/date)

**Display** — hidden files, row hover highlight, auto-refresh panels, directory color and bold, mark color, icon size (8–64 px)

**Cursor** — style (filled color or outline only), cursor color

**Viewer** — font and size for file viewer (F3), syntax highlighting, line numbers, image preview toggle

**Editor** — text font, editor GUI font, line number font size, line numbers, syntax highlighting, color scheme

**System** — theme (dark / light), terminal emulator (e.g. `ptyxis`, `gnome-terminal`, `konsole`)

## SSH connections

Stored in `~/.config/fm/settings.ini`, section `[ssh]`, key `connections`. Format: `name|user|host|port|key_path` separated by `;`.

The dialog (panel hamburger menu → SSH/SFTP) allows:
- **Select** a saved connection from the dropdown → fills the form
- **New** — clears the form for entering a new connection
- **Save** — saves/updates the connection (password is not saved)
- **Remove** — deletes the selected connection
- **Key** — path to private key (optional; default tries ssh-agent, then `~/.ssh/id_ed25519`, `id_ecdsa`, `id_rsa`), with file browser. The password field is also used as key passphrase
- **Connect** — connects with the current details and password (async, non-blocking UI)

## Security

- SSH host keys are checked against `~/.ssh/known_hosts`; an unknown host shows its SHA256 fingerprint and is added only after confirmation; a **changed** key aborts the connection (possible MITM)
- Copy never writes through an existing destination file or symlink (`O_EXCL`), never truncates the source when source = destination, and removes partial files on error/cancel
- libssh2 sessions are guarded by a per-connection lock (file jobs run in a worker thread)

- Path traversal guard on all file operations (copy, move, delete, rename, mkdir)
- Archive extract/pack: `--` separator prevents flag injection from malicious filenames
- SSH terminal: user, host, and remote path quoted via `g_shell_quote()`
- SSH password cleared from memory (`explicit_bzero`) after authentication
- Copy operations refuse to follow symlinks (`O_NOFOLLOW`)
- Remote file preview uses unique temp directories (`g_dir_make_tmp`)

## Author

krse

## License

MIT — see [LICENSE](LICENSE)
