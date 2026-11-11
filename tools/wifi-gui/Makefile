CC?=gcc
PKGS=gtk+-3.0 libnm
CFLAGS+=-O2 -Wall -Wextra -std=c11 $(shell pkg-config --cflags $(PKGS))
LDLIBS+=$(shell pkg-config --libs $(PKGS))
all: wifi-gui
wifi-gui: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
clean:
	$(RM) wifi-gui
.PHONY: all clean
