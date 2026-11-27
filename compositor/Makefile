# pixi compositor (wlroots 0.18)
PREFIX     ?= /usr/local
CC         ?= cc
PKGS        = wlroots-0.18 wayland-server xkbcommon
CFLAGS     ?= -O2 -g
# The version is the build date in UTC (YYYY.MM.DD); override with PIXI_VERSION=... if needed.
PIXI_VERSION ?= $(shell date -u +%Y.%m.%d)
override CFLAGS += -DPIXI_VERSION=\"$(PIXI_VERSION)\" -std=c11 -Wall -Wextra -Wno-unused-parameter -DWLR_USE_UNSTABLE -I. $(shell pkg-config --cflags $(PKGS))
override LDLIBS += $(shell pkg-config --libs $(PKGS))

WAYLAND_SCANNER   := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
WAYLAND_PROTOCOLS := $(shell pkg-config --variable=pkgdatadir wayland-protocols)
PROTO_DIR = ../protocol

GEN = xdg-shell-protocol.h pixi-shell-v1-protocol.h pixi-shell-v1-protocol.c

pixi: pixi.c $(GEN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ pixi.c pixi-shell-v1-protocol.c $(LDLIBS)

xdg-shell-protocol.h:
	$(WAYLAND_SCANNER) server-header $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml $@

pixi-shell-v1-protocol.h: $(PROTO_DIR)/pixi-shell-v1.xml
	$(WAYLAND_SCANNER) server-header $< $@

pixi-shell-v1-protocol.c: $(PROTO_DIR)/pixi-shell-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

install: pixi
	install -Dm755 pixi $(DESTDIR)$(PREFIX)/bin/pixi

clean:
	rm -f pixi $(GEN)

.PHONY: install clean
