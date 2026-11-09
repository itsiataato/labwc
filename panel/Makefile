# monopanel build — C + wayland-client + cairo/pango + libnm, no new deps.
#   make        build monopanel + monopanel-wifi
#   make check  headless smoke (no display needed): wifi --list/dry-run,
#               gui --help/--dry-run/--preview --dry-run, notification-gui-test
#               headless preview hover-expand model, monopanel-test
#               headless model (counts/badge-anchor/blank-click)
#   make clean  (NEVER while a panel proc runs: rm is a trash wrapper and
#               the running exe would show as (deleted))
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra
PKGS = wayland-client cairo pangocairo glib-2.0 gobject-2.0 gio-2.0 libnm
CFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS = $(shell pkg-config --libs $(PKGS))

PROTO = wlr-layer-shell-unstable-v1.xml
XDG = /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
GEN = wlr-layer-shell-client.h wlr-layer-shell-protocol.c \
      xdg-shell-client.h xdg-shell-protocol.c

all: monopanel monopanel-wifi notification-gui

wlr-layer-shell-client.h wlr-layer-shell-protocol.c xdg-shell-client.h xdg-shell-protocol.c: $(PROTO) $(XDG)
	wayland-scanner client-header $(PROTO) wlr-layer-shell-client.h
	wayland-scanner private-code $(PROTO) wlr-layer-shell-protocol.c
	wayland-scanner client-header $(XDG) xdg-shell-client.h
	wayland-scanner private-code $(XDG) xdg-shell-protocol.c

monopanel: monopanel.c wlr-layer-shell-protocol.c xdg-shell-protocol.c wlr-layer-shell-client.h
	$(CC) $(CFLAGS) -o $@ monopanel.c wlr-layer-shell-protocol.c xdg-shell-protocol.c $(LDLIBS)

monopanel-wifi: wifi-menu.c
	$(CC) $(CFLAGS) -o $@ wifi-menu.c $(LDLIBS)

notification-gui: notification-gui.c wlr-layer-shell-protocol.c xdg-shell-protocol.c wlr-layer-shell-client.h
	$(CC) $(CFLAGS) -o $@ notification-gui.c wlr-layer-shell-protocol.c xdg-shell-protocol.c $(LDLIBS)

# In-tree MONOPANEL_TEST headless harness (same sources, no display: temp
# queue dir + cairo image surfaces). WHY the -Wno- pair, test target only:
# the TEST main bypasses the live Wayland main loop, so live-path-only
# callbacks (clock_tick, wl_dispatch_cb, nm_ready, ...) are unused by
# construction; the shipping `monopanel` target above keeps full -Wall
# -Wextra with zero warnings. Manual equivalent:
#   cc -O2 -Wall -Wextra -DMONOPANEL_TEST -o /tmp/mp-test monopanel.c \
#     wlr-layer-shell-protocol.c xdg-shell-protocol.c \
#     $(pkg-config --cflags --libs wayland-client cairo pangocairo glib-2.0 gobject-2.0 gio-2.0 libnm) \
#   && /tmp/mp-test
monopanel-test: monopanel.c wlr-layer-shell-protocol.c xdg-shell-protocol.c wlr-layer-shell-client.h
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable -DMONOPANEL_TEST -o $@ monopanel.c wlr-layer-shell-protocol.c xdg-shell-protocol.c $(LDLIBS)

notification-gui-test: notification-gui.c wlr-layer-shell-protocol.c xdg-shell-protocol.c wlr-layer-shell-client.h
	$(CC) $(CFLAGS) -Wno-unused-function -Wno-unused-variable -DNOTIFGUI_TEST -o $@ notification-gui.c wlr-layer-shell-protocol.c xdg-shell-protocol.c $(LDLIBS)

check: monopanel-wifi notification-gui notification-gui-test monopanel-test
	./monopanel-wifi --list
	./monopanel-wifi --dry-run
	./monopanel-wifi --dry-run __no_such_ssid__
	./notification-gui --help >/dev/null
	./notification-gui --dry-run >/dev/null
	./notification-gui --preview --dry-run >/dev/null
	./notification-gui-test
	./monopanel-test

clean:
	rm -f monopanel monopanel-wifi notification-gui notification-gui-test monopanel-test $(GEN)
