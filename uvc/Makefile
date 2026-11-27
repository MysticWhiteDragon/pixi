# pixi-uvc: shows the Pixi screen to a PC as a USB webcam
PREFIX   ?= /usr/local
CC       ?= cc
CFLAGS   ?= -O2 -g
override CFLAGS += -std=c11 -Wall -Wextra -Wno-unused-parameter -I. $(shell pkg-config --cflags wayland-client)
override LDLIBS += $(shell pkg-config --libs wayland-client) -ljpeg

WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
PROTO = ../protocol/wlr-screencopy-unstable-v1.xml
GEN   = wlr-screencopy-unstable-v1-client-protocol.h wlr-screencopy-unstable-v1-protocol.c

pixi-uvc: pixi-uvc.c $(GEN)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ pixi-uvc.c wlr-screencopy-unstable-v1-protocol.c $(LDLIBS)

wlr-screencopy-unstable-v1-client-protocol.h: $(PROTO)
	$(WAYLAND_SCANNER) client-header $< $@

wlr-screencopy-unstable-v1-protocol.c: $(PROTO)
	$(WAYLAND_SCANNER) private-code $< $@

install: pixi-uvc
	install -Dm755 pixi-uvc $(DESTDIR)$(PREFIX)/bin/pixi-uvc
	install -Dm755 pixi-videoout $(DESTDIR)$(PREFIX)/lib/pixi/pixi-videoout
	install -Dm755 pixi-audio-route $(DESTDIR)$(PREFIX)/lib/pixi/pixi-audio-route

clean:
	rm -f pixi-uvc $(GEN)

.PHONY: install clean
