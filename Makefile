CC      := gcc
WARN    := -Wall -Wextra -Wno-deprecated-declarations
CFLAGS  := -O2 $(WARN) -std=c11 -D_GNU_SOURCE $(CFLAGS_EXTRA)

SRCDIR  := src
INCDIR  := include
BUILDDIR:= build

CFLAGS  += -I$(INCDIR)

# ── find libadwaita (includes GTK4) ──────────────────────────────────
_ADW_CFLAGS := $(shell pkg-config --cflags libadwaita-1 2>/dev/null)
_ADW_LIBS   := $(shell pkg-config --libs   libadwaita-1 2>/dev/null)

ifneq ($(strip $(_ADW_LIBS)),)
  CFLAGS += $(_ADW_CFLAGS) -DHAVE_LIBADWAITA
  LIBS    = $(_ADW_LIBS)
  $(info libadwaita-1 found – runtime theme switching enabled)
else
  _GTK_CFLAGS := $(shell pkg-config --cflags gtk4 2>/dev/null)
  _GTK_LIBS   := $(shell pkg-config --libs   gtk4 2>/dev/null)
  ifeq ($(strip $(_GTK_LIBS)),)
    $(error Neither libadwaita-1 nor gtk4 found. Install: sudo dnf install libadwaita-devel)
  endif
  CFLAGS += $(_GTK_CFLAGS)
  LIBS    = $(_GTK_LIBS)
  $(info libadwaita not found, using plain GTK4 – theme switching limited)
endif

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

# ── find libarchive (optional – browse archives like directories) ────
#  Install: sudo apt install libarchive-dev  /  sudo dnf install libarchive-devel
#  ARCHIVE_CFLAGS / ARCHIVE_LIBS override detection (unpacked headers etc.)
ARCHIVE_CFLAGS ?= $(shell pkg-config --cflags libarchive 2>/dev/null)
ARCHIVE_LIBS   ?= $(shell pkg-config --libs   libarchive 2>/dev/null)

ifneq ($(strip $(ARCHIVE_LIBS)),)
  CFLAGS += $(ARCHIVE_CFLAGS) -DHAVE_LIBARCHIVE
  LIBS   += $(ARCHIVE_LIBS)
  $(info libarchive found – archives open as directories)
else
  $(info libarchive-dev not found – archives open only via Alt+E. Install: sudo apt install libarchive-dev)
endif

# GLib-only core (unit-tested without GTK)
CORE_SRCS := $(SRCDIR)/util.c $(SRCDIR)/fsops.c $(SRCDIR)/arcfs.c
SRCS   := $(SRCDIR)/main.c $(SRCDIR)/fileitem.c $(SRCDIR)/fileops.c \
          $(SRCDIR)/archive.c $(SRCDIR)/arcpanel.c $(SRCDIR)/job.c $(SRCDIR)/dialogs.c \
          $(SRCDIR)/selection.c $(SRCDIR)/clipboard.c $(SRCDIR)/monitor.c \
          $(SRCDIR)/icons.c $(SRCDIR)/multirename.c \
          $(SRCDIR)/search.c $(SRCDIR)/ssh.c $(SRCDIR)/settings.c \
          $(SRCDIR)/viewer.c $(SRCDIR)/editor.c $(SRCDIR)/highlight.c \
          $(SRCDIR)/devices.c \
          $(CORE_SRCS)
HDRS   := $(wildcard $(INCDIR)/*.h)
TARGET := fm

# `make debug` / `make test SAN=1` → separate tree with ASan + UBSan
ifeq ($(SAN),1)
  BUILDDIR := build-debug
  CFLAGS   := $(filter-out -O2,$(CFLAGS)) -O1 -g -fno-omit-frame-pointer \
              -fsanitize=address,undefined
  LDSAN    := -fsanitize=address,undefined
endif

OBJS   := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SRCS))
BIN    := $(BUILDDIR)/$(TARGET)

.PHONY: all clean install user-install uninstall debug test test-asan test-sftp test-gui

all: $(BIN)

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LIBS) $(LDSAN)
	@echo "Done: $(BIN)"

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(HDRS) | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

debug:
	$(MAKE) SAN=1 all

# ── unit tests (GLib/GIO only – no display needed) ──────────────────
TEST_LIBS := $(shell pkg-config --libs gio-2.0) $(ARCHIVE_LIBS) $(if $(findstring HAVE_LIBSSH2,$(CFLAGS)),$(_SSH2_LIBS) $(if $(_SSH2_LIBS),,-lssh2))
TEST_BIN  := $(BUILDDIR)/test_core

$(TEST_BIN): tests/test_core.c $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CORE_SRCS)) $(HDRS)
	$(CC) $(CFLAGS) tests/test_core.c $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CORE_SRCS)) \
	      -o $@ $(TEST_LIBS) $(LDSAN)

test: $(TEST_BIN)
	$(TEST_BIN)

test-asan:
	$(MAKE) SAN=1 test

# SFTP integration tests – start a private sshd (skipped without one)
SFTP_TEST_BIN := $(BUILDDIR)/test_sftp

$(SFTP_TEST_BIN): tests/test_sftp.c $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CORE_SRCS)) $(HDRS)
	$(CC) $(CFLAGS) tests/test_sftp.c $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(CORE_SRCS)) \
	      -o $@ $(TEST_LIBS) $(LDSAN)

test-sftp:
ifneq ($(findstring HAVE_LIBSSH2,$(CFLAGS)),)
	$(MAKE) $(SFTP_TEST_BIN)
	tests/sftp_test.sh $(SFTP_TEST_BIN)
else
	@echo "sftp tests skipped: built without libssh2"
endif

# GUI integration tests (tabs, SFTP tabs, devices) – headless on broadway,
# against the throwaway sshd when available.  `make test-gui SAN=1` = ASan.
GUI_TEST_BIN := $(BUILDDIR)/test_gui
GUI_TEST_OBJS := $(filter-out $(BUILDDIR)/main.o $(BUILDDIR)/ssh.o $(BUILDDIR)/devices.o,$(OBJS))

$(GUI_TEST_BIN): tests/test_gui.c $(SRCDIR)/main.c $(SRCDIR)/ssh.c $(SRCDIR)/devices.c $(GUI_TEST_OBJS) $(HDRS)
	$(CC) $(CFLAGS) tests/test_gui.c $(GUI_TEST_OBJS) -o $@ $(LIBS) $(LDSAN)

test-gui: $(GUI_TEST_BIN)
	tests/gui_test.sh $(GUI_TEST_BIN)

clean:
	rm -rf build build-debug

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
