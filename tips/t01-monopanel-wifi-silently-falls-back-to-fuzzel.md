# t01 — monopanel wifi zone silently falls back to the fuzzel menu when tools/wifi-gui is missing

Confidence: MEASURED (ran both paths).

## What we believed
Bundling monowl's `panel/` into this labwc fork was enough for the TopBar
plus its WiFi manager, because the plan said "WiFi manager from monowl
(monowl `panel/`)".

## What actually happens
`panel/monopanel.c` (`spawn_wifi_menu`, around line 1147) does not execute
`panel/monopanel-wifi` unconditionally. It first checks:

```c
char *gui = g_strconcat(self, "/../tools/wifi-gui/wifi-gui", NULL);
if (access(gui, X_OK) == 0) {
        helper = gui;                       /* full GTK3 + libnm manager */
} else {
        helper = g_strconcat(self, "/monopanel-wifi", NULL);  /* fuzzel menu */
}
```

`self` is the directory of `/proc/self/exe`, i.e. `panel/`. So the GTK
manager is looked up at `tools/wifi-gui/wifi-gui` **one level above**
`panel/`. A fork that copies only `panel/` (as this one initially did)
resolves that path to nothing, `access(X_OK)` fails, and the wifi zone
quietly opens the fuzzel dmenu instead. Both helpers talk to
NetworkManager through libnm, so the wifi status display is correct and
nothing errors — the click just opens the wrong UI. There is no log line
announcing the fallback; the stderr line only prints the helper it
finally spawned.

## Evidence
- `panel/monopanel.c:1147-1156` (preference + fallback).
- `find labwc-fork -maxdepth 3 -type d` after the initial copy: no
  `tools/` directory.
- `test -x panel/../tools/wifi-gui/wifi-gui` failed until the copy;
  after copying `tools/wifi-gui/{main.c,Makefile}`, building, and
  re-running the same check: `X_OK: yes`.

## What to do differently
Any monowl panel bundle must include **both** `panel/` and
`tools/wifi-gui/`. Build the GUI with `make -C tools/wifi-gui`
(gtk+-3.0 + libnm, ~1 s). The packaging launcher and `.gitignore` are
already updated (commit 7dfeceb). A missing `tools/wifi-gui/wifi-gui`
is not an error condition by design, so always verify the X_OK check
above after copying instead of trusting a successful `make -C panel`.
