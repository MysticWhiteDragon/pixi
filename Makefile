# Pixi - Pi Cross Interface: compositor + shell
PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share/pixi

all: compositor shell uvc

compositor:
	$(MAKE) -C compositor

shell:
	$(MAKE) -C shell DATADIR=$(DATADIR)

uvc:
	$(MAKE) -C uvc

install: all
	$(MAKE) -C compositor install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	$(MAKE) -C shell install PREFIX=$(PREFIX) DATADIR=$(DATADIR) DESTDIR=$(DESTDIR)
	$(MAKE) -C uvc install PREFIX=$(PREFIX) DESTDIR=$(DESTDIR)
	install -d $(DESTDIR)$(DATADIR)/glyphs $(DESTDIR)$(DATADIR)/sounds $(DESTDIR)$(DATADIR)/fonts
	install -m644 assets/glyphs/*.png $(DESTDIR)$(DATADIR)/glyphs/
	install -m644 assets/sounds/*.ogg $(DESTDIR)$(DATADIR)/sounds/
	install -m644 assets/fonts/* $(DESTDIR)$(DATADIR)/fonts/
	install -m644 pixi.conf $(DESTDIR)$(DATADIR)/pixi.conf.example
	install -Dm755 session/pixi-session $(DESTDIR)$(PREFIX)/bin/pixi-session
	install -Dm755 session/pixi-askpass $(DESTDIR)$(PREFIX)/lib/pixi/pixi-askpass
	install -Dm644 session/pixi.desktop $(DESTDIR)$(PREFIX)/share/wayland-sessions/pixi.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/pixi/pixi-askpass $(DESTDIR)$(PREFIX)/bin/pixi-uvc $(DESTDIR)$(PREFIX)/lib/pixi/pixi-videoout $(DESTDIR)$(PREFIX)/lib/pixi/pixi-audio-route
	rm -f $(DESTDIR)$(PREFIX)/bin/pixi $(DESTDIR)$(PREFIX)/bin/pixi-shell $(DESTDIR)$(PREFIX)/bin/pixi-session
	rm -f $(DESTDIR)$(PREFIX)/share/wayland-sessions/pixi.desktop
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	$(MAKE) -C compositor clean
	$(MAKE) -C shell clean
	$(MAKE) -C uvc clean

.PHONY: all compositor shell uvc install uninstall clean
