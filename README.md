# FM — Total Commander for Linux

A dual-panel file manager inspired by Total Commander, written in C with GTK4.

## Features

- **Two panels** — Total Commander-style navigation
- **SFTP panel** — connect to a remote server via SSH/SFTP (libssh2)
- **Copy, move, delete** — F5/F6/F8 with progress bar (byte-level tracking)
- **Search** — F2, recursive glob search with no result limit
- **File viewer** — F3, displays text (max 1 MB, UTF-8) with configurable font
- **Text editor** — F4, built-in editor with Ctrl+S, line numbers, syntax highlighting; works on SFTP panels too
- **Syntax highlighting** — GtkSourceView (hundreds of languages) or built-in highlighter (C/Python/Shell/JS)
- **SSH terminal** — open a terminal directly in the current directory (local and SSH)
- **Saved SSH connections** — named connections (name/host/user/port) in settings.ini, selectable via dropdown
- **Settings** — tabbed dialog: Panels / Cursor / Viewer / Editor / System

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| Tab | Switch active panel |
| Enter | Open file/directory |
| Backspace | Go up one level |
| Space / Insert | Toggle selection + advance cursor |
| + | Select/deselect all |
| F2 | Search |
| F3 | View file |
| F4 | Text editor |
| F5 | Copy |
| F6 | Move |
| F7 | New directory |
| F8 | Delete |
| F9 | Rename |
| F10 | Quit |
| Ctrl+H | Hidden files |
| Ctrl+R | Reload both panels |
| Ctrl+= | Synchronize panels |
| Ctrl+S | Save (in editor) |

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

### Building

```bash
make              # compile (auto-detects libssh2 and gtksourceview5)
./fm              # run
make install      # install to /usr/local/bin/fm
make clean        # clean build artifacts
```

## Project Structure

```
include/fm.h        — shared types and declarations
src/main.c          — window, panels, key handlers, CSS
src/fileitem.c      — FileItem GObject (model for file list)
src/fileops.c       — file operations (copy, move, delete, mkdir, rename)
src/search.c        — file search
src/ssh.c           — SSH/SFTP connection and browsing
src/settings.c      — settings (font, cursor, editor, viewer, terminal)
src/viewer.c        — file viewer (F3)
src/editor.c        — text editor (F4)
src/highlight.c     — syntax highlighting (GtkSourceView wrapper / custom regex)
docs/architecture.md — detailed architecture
docs/changelog.md   — change history
Makefile            — build system
```

## Settings

Stored in `~/.config/fm/settings.ini`. Configure via the Settings dialog (gear icon):

**Panels** — panel font, GUI font, column widths (name/size/date), hidden files

**Cursor** — style (filled color or outline only), cursor color

**Viewer** — font and size for the file viewer (F3)

**Editor** — text font, editor GUI font, line number font size, line numbers, syntax highlighting, color scheme

**System** — terminal emulator (e.g. `ptyxis`, `gnome-terminal`, `konsole`)

## SSH Connections

Stored in `~/.config/fm/settings.ini`, section `[ssh]`, key `connections`. Format: `name|user|host|port` separated by `;`.

The dialog (SSH icon) allows:
- **Select** a saved connection from the dropdown → fills the form
- **New** — clears the form for entering a new connection
- **Save** — saves/updates the connection (password is not saved)
- **Delete** — deletes the selected connection
- **Connect** — connects with current data and password

## Author

**krse**

## License

MIT License

Copyright (c) 2026 krse

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
