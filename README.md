# FM — Total Commander for Linux

Dual-panel file manager inspired by Total Commander, written in C with GTK4.

## Features

- **Two panels** — navigation like in Total Commander
- **Editable path** — path entry at the top of each panel, Enter loads directory, supports ~ and relative paths
- **SFTP panel** — connect to a remote server via SSH/SFTP (libssh2)
- **File marking** — Insert/Space marks files (colored), `+` marks all; marks used for copy/move/delete
- **Copy, move, delete** — F5/F6/F8 with progress bar (byte-level tracking); full recursive directory support over SFTP; remote-to-remote (SFTP→SFTP) supported
- **File search** — F2, recursive glob search with no result limit
- **File viewer** — F3, displays text (max 50 MB) with configurable font and search (Ctrl+F)
- **Text editor** — F4, built-in editor with menu bar, find/replace (Ctrl+F/H), syntax highlighting; works on SFTP panel too
- **Syntax highlighting** — GtkSourceView (hundreds of languages) or built-in highlighter (C/Python/Shell/JS)
- **SSH terminal** — open terminal directly in the current directory (local and SSH)
- **Saved SSH connections** — named connections (name/host/user/port) in settings.ini, selectable via dropdown
- **Settings** — tabbed dialog: Panels / Cursor / Viewer / Editor / System

## Keyboard shortcuts

### Panel
| Key | Action |
|-----|--------|
| Tab | Switch active panel |
| Enter | Open file/directory (or confirm path in path entry) |
| Escape | Cancel path editing (in path entry) |
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
| Ctrl+H | Hidden files |
| Ctrl+R | Refresh both panels |
| Ctrl+= | Synchronize panels |

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

## Installation

### Fedora

```bash
sudo dnf install gtk4-devel
sudo dnf install libssh2-devel        # optional, for SFTP
sudo dnf install gtksourceview5-devel # optional, for syntax highlighting
```

### Compilation

```bash
make              # compile (auto-detects libssh2 and gtksourceview5)
./fm              # run
make install      # install to /usr/local/bin/fm
make clean        # clean build artifacts
```

## Project structure

```
include/fm.h        — shared types and declarations
src/main.c          — window, panels, keys, CSS
src/fileitem.c      — FileItem GObject (model for file list)
src/fileops.c       — file operations (copy, move, delete, mkdir, rename)
src/search.c        — file search
src/ssh.c           — SSH/SFTP connection and browsing
src/settings.c      — settings (font, cursor, editor, viewer, terminal)
src/viewer.c        — file viewer (F3)
src/editor.c        — text editor (F4)
src/highlight.c     — syntax highlighting (GtkSourceView wrapper / custom regex)
docs/architecture.md — detailed architecture
docs/changelog.md   — changelog
Makefile            — build system
```

## Settings

Stored in `~/.config/fm/settings.ini`. Configuration via Settings dialog (gear icon):

**Panels** — panel font, GUI font, column widths (name/size/date), hidden files, directory color and bold, mark color

**Cursor** — style (filled color or outline only), cursor color

**Viewer** — font and size for file viewer (F3)

**Editor** — text font, editor GUI font, line number font size, line numbers, syntax highlighting, color scheme

**System** — terminal emulator (e.g. `ptyxis`, `gnome-terminal`, `konsole`)

## SSH connections

Stored in `~/.config/fm/settings.ini`, section `[ssh]`, key `connections`. Format: `name|user|host|port` separated by `;`.

The dialog (SSH icon) allows:
- **Select** a saved connection from the dropdown → fills the form
- **New** — clears the form for entering a new connection
- **Save** — saves/updates the connection (password is not saved)
- **Remove** — deletes the selected connection
- **Connect** — connects with the current details and password

## License

MIT
