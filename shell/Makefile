# pixi-shell
PREFIX   ?= /usr/local
DATADIR  ?= $(PREFIX)/share/pixi
CC       ?= cc
PKGS      = sdl2 SDL2_ttf SDL2_image SDL2_mixer glesv2 wayland-client libvlc libqrencode
CFLAGS   ?= -O2 -g
# The version is the build date in UTC (YYYY.MM.DD); override with PIXI_VERSION=... if needed.
PIXI_VERSION ?= $(shell date -u +%Y.%m.%d)
override CFLAGS += -DPIXI_VERSION=\"$(PIXI_VERSION)\" -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-format-truncation -I. \
                   -DPIXI_DATADIR=\"$(DATADIR)\" $(shell pkg-config --cflags $(PKGS))
override LDLIBS += $(shell pkg-config --libs $(PKGS)) -lm

WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
PROTO = ../protocol/pixi-shell-v1.xml
GEN   = pixi-shell-v1-client-protocol.h pixi-shell-v1-protocol.c

pixi-shell: pixi-shell.c sys.c sys.h pics.c pics.h ftp.c ftp.h web.c web.h browser.c browser.h video.c video.h terminal.c terminal.h shaders.h $(GEN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ pixi-shell.c sys.c pics.c ftp.c web.c browser.c video.c terminal.c pixi-shell-v1-protocol.c $(LDLIBS) -lpam -lutil

pixi-shell-v1-client-protocol.h: $(PROTO)
	$(WAYLAND_SCANNER) client-header $< $@

pixi-shell-v1-protocol.c: $(PROTO)
	$(WAYLAND_SCANNER) private-code $< $@

install: pixi-shell
	install -Dm755 pixi-shell $(DESTDIR)$(PREFIX)/bin/pixi-shell

clean:
	rm -f pixi-shell $(GEN)

.PHONY: install clean
