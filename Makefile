CC      := gcc
WARN    := -Wall -Wextra -Wno-deprecated-declarations
CFLAGS  := -O2 $(WARN) -std=c11 -D_GNU_SOURCE

SRCDIR  := src
INCDIR  := include
BUILDDIR:= build

CFLAGS  += -I$(INCDIR)

# ── find GTK4 ────────────────────────────────────────────────────────
_GTK_CFLAGS := $(shell pkg-config --cflags gtk4 2>/dev/null)
_GTK_LIBS   := $(shell pkg-config --libs   gtk4 2>/dev/null)

ifeq ($(strip $(_GTK_LIBS)),)
  $(error GTK4 not found. Install: sudo dnf install gtk4-devel)
endif

CFLAGS += $(_GTK_CFLAGS)
LIBS    = $(_GTK_LIBS)

# ── find libssh2 (optional – enables full SFTP panel) ────────────────
#  Install: sudo dnf install libssh2-devel
_SSH2_CFLAGS := $(shell pkg-config --cflags libssh2 2>/dev/null)
_SSH2_LIBS   := $(shell pkg-config --libs   libssh2 2>/dev/null)

ifneq ($(strip $(_SSH2_LIBS)),)
  CFLAGS += $(_SSH2_CFLAGS) -DHAVE_LIBSSH2
  LIBS   += $(_SSH2_LIBS)
  $(info libssh2 found – SFTP panel enabled)
else
  # Try bare -lssh2 (no pkg-config, but libssh2.so present)
  _SSH2_TEST := $(shell echo 'int main(){}' | gcc -lssh2 -x c - -o /dev/null 2>&1)
  ifeq ($(strip $(_SSH2_TEST)),)
    CFLAGS += -DHAVE_LIBSSH2
    LIBS   += -lssh2
    $(info libssh2 found (bare) – SFTP panel enabled)
  else
    $(info libssh2-devel not found – SFTP panel disabled. Install: sudo dnf install libssh2-devel)
  endif
endif

# ── find gtksourceview-5 (optional – enables full syntax highlighting) ──
#  Install: sudo dnf install gtksourceview5-devel
_SV_CFLAGS := $(shell pkg-config --cflags gtksourceview-5 2>/dev/null)
_SV_LIBS   := $(shell pkg-config --libs   gtksourceview-5 2>/dev/null)

ifneq ($(strip $(_SV_LIBS)),)
  CFLAGS += $(_SV_CFLAGS) -DHAVE_GTKSOURCEVIEW
  LIBS   += $(_SV_LIBS)
  $(info gtksourceview-5 found – full syntax highlighting enabled)
else
  $(info gtksourceview-5 not found – basic highlighting only. Install: sudo dnf install gtksourceview5-devel)
endif

SRCS   := $(SRCDIR)/main.c $(SRCDIR)/fileitem.c $(SRCDIR)/fileops.c \
          $(SRCDIR)/search.c $(SRCDIR)/ssh.c $(SRCDIR)/settings.c \
          $(SRCDIR)/viewer.c $(SRCDIR)/editor.c $(SRCDIR)/highlight.c
OBJS   := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
TARGET := fm
BIN    := $(BUILDDIR)/$(TARGET)

.PHONY: all clean install user-install uninstall

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LIBS)
	@echo "Done: $(BIN)"

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/fm.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR)

PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	install -Dm644 data/sk.km.fm.desktop $(DESTDIR)$(DATADIR)/applications/sk.km.fm.desktop
	install -Dm644 data/icons/hicolor/scalable/apps/sk.km.fm.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/sk.km.fm.svg
	@for size in 16 24 32 48 64 128 256; do \
		install -Dm644 data/icons/hicolor/$${size}x$${size}/apps/sk.km.fm.png \
			$(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/sk.km.fm.png; \
	done
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null
	@echo "Installed: $(DESTDIR)$(PREFIX)/bin/$(TARGET)"

user-install: $(BIN)
	install -Dm755 $(BIN) $(HOME)/.local/bin/$(TARGET)
	install -Dm644 data/sk.km.fm.desktop $(HOME)/.local/share/applications/sk.km.fm.desktop
	install -Dm644 data/icons/hicolor/scalable/apps/sk.km.fm.svg $(HOME)/.local/share/icons/hicolor/scalable/apps/sk.km.fm.svg
	@for size in 16 24 32 48 64 128 256; do \
		install -Dm644 data/icons/hicolor/$${size}x$${size}/apps/sk.km.fm.png \
			$(HOME)/.local/share/icons/hicolor/$${size}x$${size}/apps/sk.km.fm.png; \
	done
	-gtk-update-icon-cache -f -t $(HOME)/.local/share/icons/hicolor 2>/dev/null
	@echo "Installed for user: $(HOME)/.local/bin/$(TARGET)"

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(TARGET)
	rm -f $(DESTDIR)$(DATADIR)/applications/sk.km.fm.desktop
	rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/sk.km.fm.svg
	@for size in 16 24 32 48 64 128 256; do \
		rm -f $(DESTDIR)$(DATADIR)/icons/hicolor/$${size}x$${size}/apps/sk.km.fm.png; \
	done
	-gtk-update-icon-cache -f -t $(DESTDIR)$(DATADIR)/icons/hicolor 2>/dev/null
