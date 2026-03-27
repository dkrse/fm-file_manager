# FM — Total Commander for Linux

Dual-panel file manager inspired by Total Commander, written in C with GTK4.

## Features

- **Two panels** — navigation like in Total Commander
- **Editable path** — path entry at the top of each panel, Enter loads directory, supports ~ and relative paths
- **SFTP panel** — connect to a remote server via SSH/SFTP (libssh2), async connect (non-blocking UI)
- **File marking** — Insert/Space marks files (colored), `+` marks all; marks used for copy/move/delete
- **Copy, move, delete** — F5/F6/F8 with progress dialog (animated phases, byte-level tracking, speed display, cancel button); full recursive directory support over SFTP; remote-to-remote (SFTP→SFTP) supported
- **Archive support** — extract and create archives (tar.gz, tar.bz2, tar.xz, tar.zst, zip, 7z, rar)
- **File filter** — Ctrl+S, instant substring filter on active panel
- **File mask** — glob pattern display filter on active panel (files only, e.g. `*.c`)
- **File search** — F2, recursive search with content grep, size filter, MC-style grouped results in panel
- **File viewer** — F3, displays text (max 50 MB) with configurable font and search (Ctrl+F)
- **Text editor** — F4, built-in editor with menu bar, find/replace (Ctrl+F/H), syntax highlighting; works on SFTP panel too
- **Syntax highlighting** — GtkSourceView (hundreds of languages) or built-in highlighter (C/Python/Shell/JS)
- **Content-type icons** — files and folders shown with freedesktop standard icons based on extension (~80 types)
- **SSH terminal** — open terminal directly in the current directory (local and SSH)
- **Saved SSH connections** — named connections (name/host/user/port) in settings.ini, selectable via dropdown
- **Hamburger menu** — Search, Mask, Filter, Extract, Pack, SSH accessible from header bar
- **Settings** — tabbed dialog: Panels / Cursor / Viewer / Editor / System

## Keyboard shortcuts

### Panel
| Key | Action |
|-----|--------|
| Tab | Switch active panel |
| Enter | Open file/directory (or confirm path in path entry) |
| Escape | Cancel path editing / close filter |
| Backspace | Go up one level |
| Space / Insert | Mark file + move cursor |
| + | Mark/unmark all |
| F2 | Search files |
| F3 | View file |
| F4 | Text editor |
| F5 | Copy |
| F6 | Move |
| F7 | New directory |
| F8 | Delete |
| F9 | Rename |
| F10 | Quit |
| Ctrl+S | Filter files in active panel |
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
```

## Project structure

```
include/fm.h        — shared types and declarations
src/main.c          — window, panels, keys, CSS, filter, hamburger menu
src/fileitem.c      — FileItem GObject (model for file list)
src/fileops.c       — file operations (copy, move, delete, mkdir, rename, extract, pack)
src/search.c        — file search
src/ssh.c           — SSH/SFTP connection and browsing
src/settings.c      — settings (font, cursor, editor, viewer, terminal, icon size)
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

**Panels** — panel font, GUI font, column widths (name/size/date), hidden files, directory color and bold, mark color, icon size (8–64 px)

**Cursor** — style (filled color or outline only), cursor color

**Viewer** — font and size for file viewer (F3)

**Editor** — text font, editor GUI font, line number font size, line numbers, syntax highlighting, color scheme

**System** — terminal emulator (e.g. `ptyxis`, `gnome-terminal`, `konsole`)

## SSH connections

Stored in `~/.config/fm/settings.ini`, section `[ssh]`, key `connections`. Format: `name|user|host|port` separated by `;`.

The dialog (SSH icon or hamburger menu) allows:
- **Select** a saved connection from the dropdown → fills the form
- **New** — clears the form for entering a new connection
- **Save** — saves/updates the connection (password is not saved)
- **Remove** — deletes the selected connection
- **Connect** — connects with the current details and password (async, non-blocking UI)

## License

MIT
