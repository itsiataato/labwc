# monopanel — top bar (wifi + clock), layer-shell client

Separate process from the compositor by design: it can never block it.

## Build

```
make -C panel        # builds ./monopanel + ./monopanel-wifi, zero new deps
make -C panel check  # headless smoke: --list via libnm, no display needed
```

Toolchain: `cc`, `wayland-scanner`, `pkg-config`, and system packages
`wayland-client cairo pangocairo glib-2.0 gobject-2.0 gio-2.0 libnm`
(all already on CachyOS; no `sudo` needed). Protocols: vendored
`wlr-layer-shell-unstable-v1.xml` + system `xdg-shell.xml`, generated at
build time (`wlr-layer-shell-client.h`, `*-protocol.c` are build outputs).

## Run

```
./panel/monopanel                  # needs WAYLAND_DISPLAY; top-anchored 28px
./panel/monopanel-wifi             # click handler: AP menu / PSK prompt / connect
./panel/monopanel-wifi --list      # no display needed: SSID|strength|sec|saved|active
./panel/monopanel-wifi --dry-run [SSID]  # routing proof, zero network change
```

Click the left (wifi) zone of the bar to spawn the menu. The menu and the
password prompt are `fuzzel --dmenu` (day-1 app, owns focus + masking);
`--password` for the PSK. Copy `panel.conf.example` to
`~/.config/monowl/panel.conf` for `height / interface / font /
clock_format / menu_lines` (all optional).

## Notification icon + preview popup + answer deck

The queue in `~/.local/share/monowl/notifications/` holds `*.txt` plain
messages and `*.q` questions. The bar shows ONE notification icon
clock-adjacent: blank/dim when the queue is empty, otherwise an amber
count of TOTAL waiting (plain + questions). No message text appears on
the bar. Clicking the icon (a no-op when empty) opens a tiny layer-shell
preview popup: up to 3 oldest plain rows plus 1 question row for the
oldest question. Clicking a plain row dismisses that message; clicking
the question row spawns `./notification-gui` — a layer-shell centered
dialog (overlay, so it survives Super+D), oldest question first, one
button per answer (Yes/No when the question carries no answers),
auto-next, auto-close when the queue drains. The preview closes on
selection, empty queue, pointer-leave, or Esc. Click answers
(`<base>.answer` written before the `.q` is unlinked); Esc dismisses
without answering. `notification-gui --help`, `--dry-run`, and
`--preview [--dry-run]` work without a display.

## Stack decision

Raw `wayland-client` + `cairo/pango` + `libnm`, not `gtk4-layer-shell`:
gtk4-layer-shell is in CachyOS extra but was not installed and this env
has no passwordless sudo, so it could not be built or verified here. The
raw client needs zero new packages and idles far under the 50MB budget
(~20MB RSS, ~6.8MB PSS measured). SHM via `memfd_create` (never `/tmp`);
damage-driven re-render, 1s clock tick, no frame callbacks; glib main loop
with `prepare_read/read_events/dispatch_pending` + explicit `flush`
(blocking `wl_display_dispatch` and flush-less patterns both failed: no
configure arrived).

## Verify (measured 2026-10-27)

- `make` clean, zero warnings; `./monopanel-wifi --list` shows live APs
  over libnm (no nmcli): `MG-NETWORK|97|WPA2|saved|active`, etc.
- Headless labwc (`wayland-1`): `set_anchor(13) set_size(0,28)
  set_exclusive_zone(28)`, compositor `configure(1280x28)` acked, 1s clock
  re-renders; bar RSS ~20MB, PSS ~6.8MB.
- Menu via PATH stub: `* SSID strength SEC [saved]` entries, correct
  fuzzel args incl. `--password` prompt.
- Connect-failure path (fixed code): stub picks `DIRECT-*-EPSON` by SSID
  match, feeds wrong PSK → `connect failed (wrong password?)`,
  MG-NETWORK untouched, residue profile deleted. AP order shifts between
  scans — never select menu entries by fixed index.
- Live disconnect test proved Disconnect works (autoconnect restored
  MG-NETWORK). Connect-success and pixel-appearance still need a
  manual/tester run on a spare network.
- 2026-10-27 session: fixed NULL `wl_pointer` frame/axis_source/axis_stop/
  axis_discrete handlers (client aborted on first pointer frame under
  labwc). Bar now runs clean 8s under labwc (exit 124 = alive at kill),
  screenshot shows `MG-NETWORK 83%` + bars left, clock right, 28px top
  bar; RSS ~20MB. `--dry-run [SSID]` added: prints the AP list plus the
  planned `do_connect`/`do_disconnect` routing WITHOUT mutating NM —
  `make check` runs `--list` + both dry-run shapes, MG-NETWORK connected
  throughout. `make clean && make` rebuilds from zero warnings.
