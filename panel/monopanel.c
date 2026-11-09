/* monopanel — top-anchored 28px kiosk bar: wifi status + sliders + clock.
 *
 * Layout left-to-right: wifi signal bars + SSID text, Bri slider (1-100%),
 * Mic slider (0-100% + red-X mute glyph) immediately left of Vol, Vol
 * slider (0-100% + MUTE flag), single notification icon, clock right.
 * Sliders are cairo-drawn
 * (track + fill + knob); drag sets the value, wheel over a slider steps
 * +-5%. Wifi zone click still opens the fuzzel menu (wifi-menu.c); icon
 * click spawns the preview popup (notification-gui --preview).
 *
 * Stack choice (documented per task spec): raw wayland-client +
 * wlr-layer-shell-unstable-v1 + cairo/pango + libnm, with fuzzel --dmenu
 * as the wifi menu / PSK prompt (see wifi-menu.c). gtk4-layer-shell exists
 * in CachyOS extra/ (1.3.0) but was not installed and this env has no
 * passwordless sudo, so it could not be built or verified here. The raw
 * client needs zero new packages and keeps RSS far under the 50MB idle
 * budget (no GTK stack). fuzzel is already a day-1 app and owns keyboard
 * focus + password masking, which would otherwise be hundreds of lines of
 * xkbcommon text-entry code.
 *
 * Separate process from the compositor by design: it can never block it.
 * Wifi state comes from libnm cached properties only (no nmcli, no D-Bus
 * round-trip per poll). Secrets: this file never sees a PSK at all.
 *
 * Config ~/.config/monowl/panel.conf (key=value, all optional):
 *   height=28  interface=wlan0  font=monospace 10
 *   clock_format=%a %d %b %H:%M  menu_lines=12
 *
 * Build: make -C panel
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE /* memfd_create */
#include <cairo/cairo.h>
#include <fcntl.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <NetworkManager.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-client.h"

static struct {
	int height;
	char *interface;
	char *font;
	char *clock_format;
	int menu_lines;
} cfg = { 28, NULL, NULL, NULL, 12 };

static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_surface *surf;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surf;
static int surf_w = 800; /* replaced by first configure */
static int configured = 0;
static double pointer_x = -1;
static int pointer_on_bar = 0;
static double wifi_zone_w = 300; /* updated at draw; clicks left of it open menu */

/* Slider state. WHY fixed sysfs path: this kiosk has one intel_backlight
 * device; brightnessctl is still used for writes so a rename only affects
 * the readback label, never control. */
#define SLIDER_W 90
#define SLIDER_GAP 16
static int bri_pct = -1; /* 1..100, -1 = unknown (sysfs unreadable) */
static int vol_pct = -1; /* 0..100, -1 = unknown (wpctl failed/offline) */
static int vol_muted = 0;
static int mic_pct = -1; /* 0..100, -1 = unknown (no source / wpctl failed) */
static int mic_muted = 0;
static gint64 bri_cache_until = 0; /* 2s readback cache (monotonic µs) */
static gint64 vol_cache_until = 0;
static gint64 mic_cache_until = 0;
static gboolean vol_inflight = FALSE; /* one wpctl query at a time */
static gboolean mic_inflight = FALSE; /* one wpctl source query at a time */
/* Widget track rects for hit-testing; tx < 0 means "not drawn this frame". */
static double bri_tx = -1, bri_tw = 0, vol_tx = -1, vol_tw = 0;
static double mic_tx = -1, mic_tw = 0;
/* Mic glyph icon rect (fixed zone left of the Mic track); gx < 0 = hidden. */
#define MIC_GLYPH_W 16
static double mic_gx = -1, mic_gw = 0;
static int drag_target = 0; /* 0 none, 1 brightness, 2 volume, 3 mic */

/* Single notification icon (clock-adjacent, always drawn): blank dim pill
 * when the queue is empty, amber count pill showing TOTAL waiting
 * (plain .txt + question .q) otherwise. NO message text ever renders on
 * the bar (TASK-010 removed the inline plain slot and the TASK-007 hover
 * dropdown with it — no dead code remains).
 * WHY dir-queue, no daemon protocol: the bar already ticks at 1Hz, so a
 * readdir of a near-empty dir per tick is ~µs and needs zero new deps,
 * zero sockets, zero focus stealing. Writers are any proc that can
 * $XDG_DATA_HOME slash monowl slash notifications (see panel/notification
 * CLI). Icon click with 0 waiting = no-op; with >0 it spawns
 * `notification-gui --preview` (tiny layer-shell popup: up to 3 oldest
 * plain rows + 1 question row that routes to the answer deck).
 * WHY split counts internally: .txt = plain (preview dismisses the
 * clicked file), .q = question (preview routes to notification-gui, never
 * dismisses as plain). A single displayed total must never let a plain
 * click silently drop a question an agent is blocked on; the split lives
 * in the preview, which owns the queue files. The badge anchor is
 * permanent: blank at zero, count when waiting. */
static int notif_count = 0; /* waiting plain .txt (feeds preview rows) */
static int q_count = 0; /* waiting question .q (feeds preview question row) */
static double badge_tx = -1, badge_tw = 0;
static char *notif_dir = NULL;

static void render_frame(void); /* defined below; tick repaints */

static void notif_dir_init(void)
{
	g_free(notif_dir);
	notif_dir = g_build_filename(g_get_user_data_dir(), "monowl",
				     "notifications", NULL);
	g_mkdir_with_parents(notif_dir, 0700);
}

/* Rescan queue; counts only (no file reads, no text kept).
 * WHY split counts: .txt = plain (preview rows, click dismisses the
 * clicked file), .q = question (preview question row routes to
 * notification-gui, never dismissed as plain). The bar displays only
 * their sum; the split stays internal for preview routing.
 * TASK-008: this scan is strictly READ-ONLY (readdir only, no unlink).
 * Queue files live in a dir, so fullscreen transitions cannot drop
 * them; this file has NO unlink path at all — all dismisses happen in
 * the preview popup, confined to the queue dir (see notification-gui
 * --preview). */
static void notif_refresh(void)
{
	notif_count = 0;
	q_count = 0;
	if (!notif_dir)
		return;
	GDir *d = g_dir_open(notif_dir, 0, NULL);
	if (!d)
		return;
	const char *e;
	while ((e = g_dir_read_name(d))) {
		size_t n = strlen(e);
		/* WHY split guards: && binds tighter than ||, so a single
		 * n > 4 guard would leave e+n-2 underflowed (OOB read) for
		 * short names checked against ".q". No sort: the bar shows
		 * only the total; FIFO ordering lives in the preview. */
		if (n > 4 && !strcmp(e + n - 4, ".txt"))
			notif_count++;
		else if (n > 2 && !strcmp(e + n - 2, ".q"))
			q_count++;
	}
	g_dir_close(d);
}

/* Icon click with waiting items: open the tiny layer-shell preview popup
 * (`notification-gui --preview`: up to 3 oldest plain rows + 1 question
 * row that routes to the answer deck). Fast-path guard (argv, never
 * shell): a click while the preview is open skips the fork. WHY
 * full-cmdline anchored match: the same pgrep -f -x idiom as the old
 * answer-deck guard (TASK-006), extended with the --preview flag so the
 * preview and the deck never suppress each other. WHY the guard stays
 * despite the race: guard + spawn are not atomic, so two rapid clicks can
 * both pass it — but the real serialization is the preview's own atomic
 * mkdir lock (queue dir/.preview-lock, see notification-gui
 * preview_lock_acquire): the losing fork exits quietly instead of
 * stacking a second popup, and a stale dir is reclaimed inside the child
 * (pid + cmdline check). */
static void spawn_preview(void)
{
	char self[4096];
	char *slash;
	char *helper;
	char *argv[] = { NULL, "--preview", NULL };
	GError *err = NULL;
	ssize_t n;
	char *pargv[] = { "pgrep", "-f", "-x",
			  ".*/notification-gui --preview", NULL };
	gint st = 0;
	GError *perr = NULL;

	if (g_spawn_sync(NULL, pargv, NULL,
			 G_SPAWN_SEARCH_PATH |
			 G_SPAWN_STDOUT_TO_DEV_NULL |
			 G_SPAWN_STDERR_TO_DEV_NULL,
			 NULL, NULL, NULL, NULL, &st, &perr)) {
		if (WIFEXITED(st) && WEXITSTATUS(st) == 0)
			return;
	} else {
		g_clear_error(&perr); /* no pgrep: spawn anyway */
	}
	/* Same dir as this binary; never via shell, no secrets in argv.
	 * WHY loud failure: same lesson as the wifi menu — a dead click must
	 * leave evidence in /tmp/monowl.log, not silence. */
	n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0) {
		fprintf(stderr, "monopanel: notif click: readlink /proc/self/exe failed\n");
		return;
	}
	self[n] = '\0';
	slash = strrchr(self, '/');
	if (!slash) {
		fprintf(stderr, "monopanel: notif click: no slash in exe path\n");
		return;
	}
	*slash = '\0';
	helper = g_strconcat(self, "/notification-gui", NULL);
	argv[0] = helper;
	fprintf(stderr, "monopanel: notif click total=%d spawning %s --preview\n",
		notif_count + q_count, helper);
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			   NULL, &err)) {
		char *nargv[] = { "notify-send", "notification-gui",
				  "preview failed (see /tmp/monowl.log)",
				  NULL };
		fprintf(stderr, "monopanel: notif click: spawn %s failed: %s\n",
			helper, err ? err->message : "unknown");
		g_spawn_async(NULL, nargv, NULL, G_SPAWN_SEARCH_PATH, NULL,
			      NULL, NULL, NULL);
		g_clear_error(&err);
	}
	g_free(helper);
}

static int in_badge(double x)
{
	return badge_tx >= 0 && x >= badge_tx && x <= badge_tx + badge_tw;
}

static NMClient *nm = NULL;
static NMDevice *wifidev = NULL;
static char status_text[128] = "wifi …";
static int status_strength = -1; /* -1 = no link */

static struct wl_buffer *buf = NULL;
static void *shm_data = NULL;
static size_t shm_size = 0;

/* ---------- config ---------- */

static void cfg_init(void)
{
	cfg.interface = g_strdup("wlan0");
	cfg.font = g_strdup("monospace 10");
	cfg.clock_format = g_strdup("%a %d %b %H:%M");
	char *p = g_build_filename(g_get_home_dir(), ".config", "monowl",
				   "panel.conf", NULL);
	char *data = NULL;
	if (g_file_get_contents(p, &data, NULL, NULL)) {
		char **lines = g_strsplit(data, "\n", -1);
		for (int i = 0; lines[i]; i++) {
			char *eq = strchr(lines[i], '=');
			if (!eq || lines[i][0] == '#')
				continue;
			*eq = '\0';
			char *k = g_strstrip(lines[i]), *v = g_strstrip(eq + 1);
			if (!strcmp(k, "height"))
				cfg.height = CLAMP(atoi(v), 16, 64);
			else if (!strcmp(k, "interface")) {
				g_free(cfg.interface);
				cfg.interface = g_strdup(v);
			} else if (!strcmp(k, "font")) {
				g_free(cfg.font);
				cfg.font = g_strdup(v);
			} else if (!strcmp(k, "clock_format")) {
				g_free(cfg.clock_format);
				cfg.clock_format = g_strdup(v);
			} else if (!strcmp(k, "menu_lines"))
				cfg.menu_lines = CLAMP(atoi(v), 4, 40);
		}
		g_strfreev(lines);
		g_free(data);
	}
	g_free(p);
}

/* ---------- wifi state via libnm (cached props, never blocks) ---------- */

static void wifi_refresh(void)
{
	if (!nm)
		return;
	wifidev = NULL;
	const GPtrArray *devs = nm_client_get_devices(nm);
	for (guint i = 0; devs && i < devs->len; i++) {
		NMDevice *d = devs->pdata[i];
		if (NM_IS_DEVICE_WIFI(d) &&
		    !strcmp(nm_device_get_iface(d), cfg.interface)) {
			wifidev = d;
			break;
		}
	}
	if (!wifidev || nm_device_get_state(wifidev) != NM_DEVICE_STATE_ACTIVATED) {
		snprintf(status_text, sizeof status_text, "%s",
			 wifidev ? "disconnected" : "no wifi");
		status_strength = -1;
		return;
	}
	NMAccessPoint *ap = nm_device_wifi_get_active_access_point(
		NM_DEVICE_WIFI(wifidev));
	if (!ap) {
		snprintf(status_text, sizeof status_text, "connecting…");
		status_strength = -1;
		return;
	}
	GBytes *ssid = nm_access_point_get_ssid(ap);
	gsize n = 0;
	const char *s = ssid ? g_bytes_get_data(ssid, &n) : NULL;
	char pretty[64];
	if (s && n && n < sizeof pretty - 1 && g_utf8_validate(s, n, NULL)) {
		memcpy(pretty, s, n);
		pretty[n] = '\0';
	} else {
		snprintf(pretty, sizeof pretty, "(hidden)");
	}
	status_strength = CLAMP((int)nm_access_point_get_strength(ap), 0, 100);
	snprintf(status_text, sizeof status_text, "%s  %d%%",
		 pretty, status_strength);
}

static gboolean wifi_tick(gpointer unused)
{
	(void)unused;
	wifi_refresh();
	return G_SOURCE_CONTINUE;
}

/* ---------- rendering ---------- */

static int draw_wifi_text(cairo_t *cr, int x, int h)
{
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(cfg.font);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, status_text, -1);
	int tw, th;
	pango_layout_get_pixel_size(lo, &tw, &th);
	cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
	cairo_move_to(cr, x, (h - th) / 2);
	pango_cairo_show_layout(cr, lo);
	g_object_unref(lo);
	return tw;
}

static void draw_signal_bars(cairo_t *cr, int x, int h)
{
	/* 4 cairo rects: zero font dependency, readable at 28px. */
	int bw = 4, gap = 2, base = h - 7;
	for (int i = 0; i < 4; i++) {
		int bh = 4 + i * 4;
		double on = status_strength >= (i + 1) * 25;
		if (status_strength < 0)
			on = 0;
		cairo_set_source_rgb(cr, on ? 0.35 : 0.35, on ? 0.85 : 0.35,
				     on ? 0.45 : 0.35);
		cairo_rectangle(cr, x + i * (bw + gap), base - bh, bw, bh);
		cairo_fill(cr);
	}
}

/* Measure a label in the bar font without drawing it (for fit checks). */
static int measure_text(cairo_t *cr, const char *s, int *th_out)
{
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(cfg.font);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, s, -1);
	int tw, th;
	pango_layout_get_pixel_size(lo, &tw, &th);
	if (th_out)
		*th_out = th;
	g_object_unref(lo);
	return tw;
}

static int draw_label_text(cairo_t *cr, int x, int h, const char *s)
{
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(cfg.font);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, s, -1);
	int tw, th;
	pango_layout_get_pixel_size(lo, &tw, &th);
	cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
	cairo_move_to(cr, x, (h - th) / 2);
	pango_cairo_show_layout(cr, lo);
	g_object_unref(lo);
	return tw;
}

/* Draw "label  track+fill+knob" at x inside a FIXED-WIDTH label cell, so the
 * track rect never moves when the value text changes width ("Bri 9%" vs
 * "Bri 100%"). WHY: with a variable-width label, tx = x + textwidth + 6
 * shifts under a held cursor on 9%->10% transitions; pointer_motion then
 * recomputes a new pct from the shifted track, re-renders, shifts again —
 * an expand/shrink feedback loop while the finger never moves. The cell
 * width is the worst-case label ("Bri 100%", "Vol 100% MUTE"); the live
 * label is left-aligned inside it, so tx = x + cell + 6 is constant. */
static int draw_slider(cairo_t *cr, int x, int h, const char *label,
		       int label_cell_w, int pct, double fr, double fg,
		       double fb, double *tx_out, double *tw_out)
{
	draw_label_text(cr, x, h, label);
	int tx = x + label_cell_w + 6;
	int ty = h / 2 - 2;
	cairo_set_source_rgb(cr, 0.32, 0.32, 0.34);
	cairo_rectangle(cr, tx, ty, SLIDER_W, 4);
	cairo_fill(cr);
	if (pct >= 0) {
		int fw = (int)(SLIDER_W * (CLAMP(pct, 0, 100) / 100.0));
		if (fw > 0) {
			cairo_set_source_rgb(cr, fr, fg, fb);
			cairo_rectangle(cr, tx, ty, fw, 4);
			cairo_fill(cr);
		}
		cairo_set_source_rgb(cr, 0.95, 0.95, 0.95);
		cairo_arc(cr, tx + fw, h / 2.0, 5, 0, 2 * 3.14159);
		cairo_fill(cr);
	}
	*tx_out = tx;
	*tw_out = SLIDER_W;
	return label_cell_w + 6 + SLIDER_W;
}
/* Amber pill behind the notification text so it reads as one tappable
 * slot, distinct from wifi Sliders clock without shouting. */
static void pill_rect(cairo_t *cr, double x, double y, double w, double h,
		      double r)
{
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -3.14159 / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, 3.14159 / 2);
	cairo_arc(cr, x + r, y + h - r, r, 3.14159 / 2, 3.14159);
	cairo_arc(cr, x + r, y + r, r, 3.14159, 3.14159 * 1.5);
	cairo_close_path(cr);
}

/* Mic glyph icon: tiny cairo mic (capsule head + holder arc + stem +
 * base) in a fixed MIC_GLYPH_W zone left of the Mic slider track. Red X
 * (two diagonal strokes) overlaid when muted; plain glyph when live.
 * WHY cairo, not text: no font dependency for the icon (same reason as
 * the signal bars), legible at 28px. Sets mic_gx/mic_gw for the
 * mute-toggle hit-test; the track hit-test lives on mic_tx/mic_tw, so
 * the two zones never overlap. Returns width consumed. */
static int draw_mic_glyph(cairo_t *cr, int x, int h, int muted)
{
	double cx = x + MIC_GLYPH_W / 2.0;
	double cy = h / 2.0 - 2.0;
	double g = muted ? 0.55 : 0.85;
	cairo_set_source_rgb(cr, g, g, muted ? 0.60 : 0.85);
	/* capsule head: rounded 7x10 body via the shared pill path */
	pill_rect(cr, cx - 3.5, cy - 9, 7, 10, 3.5);
	cairo_fill(cr);
	/* holder arc cradling the head + stem + base */
	cairo_set_line_width(cr, 1.5);
	cairo_arc(cr, cx, cy + 1, 5.5, 0.35, 3.14159 - 0.35);
	cairo_stroke(cr);
	cairo_move_to(cr, cx, cy + 6.5);
	cairo_line_to(cr, cx, cy + 9.5);
	cairo_stroke(cr);
	cairo_move_to(cr, cx - 4, cy + 9.5);
	cairo_line_to(cr, cx + 4, cy + 9.5);
	cairo_stroke(cr);
	if (muted) {
		/* WHY red X over the whole glyph cell: readable at a
		 * glance, mirrors the Vol MUTE-flag convention. */
		cairo_set_source_rgb(cr, 0.92, 0.20, 0.20);
		cairo_set_line_width(cr, 2);
		cairo_move_to(cr, x + 2, cy - 9);
		cairo_line_to(cr, x + MIC_GLYPH_W - 2, cy + 10);
		cairo_move_to(cr, x + MIC_GLYPH_W - 2, cy - 9);
		cairo_line_to(cr, x + 2, cy + 10);
		cairo_stroke(cr);
	}
	mic_gx = x;
	mic_gw = MIC_GLYPH_W;
	return MIC_GLYPH_W;
}

/* Centered label inside a button pill of width w at x. */
static void draw_btn_label(cairo_t *cr, int x, int w, int h, const char *s)
{
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(cfg.font);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, s, -1);
	int tw, th;
	pango_layout_get_pixel_size(lo, &tw, &th);
	cairo_move_to(cr, x + (w - tw) / 2, (h - th) / 2);
	pango_cairo_show_layout(cr, lo);
	g_object_unref(lo);
}

/* Fixed icon reserve, measured per frame in the live font: the widest
 * sane label (envelope + " 99") + pill padding, 40px floor. Right-anchored
 * at clock_x, so slider layout never shifts when the count changes or
 * clears to zero. */
static int badge_need_w(cairo_t *cr)
{
	int need = measure_text(cr, "\xe2\x9c\x89 99", NULL) + 16 + 8;
	return need < 40 ? 40 : need;
}

/* Single notification icon: ALWAYS a clock-adjacent pill at the fixed
 * anchor — blank dim placeholder at zero, amber count pill showing TOTAL
 * waiting (plain .txt + question .q) otherwise. The split counts stay
 * separate internally for preview routing; the bar shows one number.
 * WHY unconditional reserve: badge_need_w is fixed, so slider layout and
 * the clock never shift when the count changes or clears to zero. Blank
 * click is a no-op (see pointer_button); a live pill opens the preview.
 * Yields (draws nothing) only when <40px is free, so wifi + clock stay
 * usable on tiny bars. Returns width consumed off the right. */
static int draw_badge(cairo_t *cr, int sx, int h, int clock_x)
{
	int total = notif_count + q_count;
	int avail = clock_x - 8 - sx;
	int bw, bx, th;
	char label[32];

	badge_tx = -1;
	badge_tw = 0;
	/* WHY 40px floor (not 70): waiting notifications outrank slider
	 * comfort; below 40px the pill would clip its own label, so only
	 * then do we yield to wifi+clock. */
	if (avail < 40)
		return 0;
	/* Pill height from a representative label; same font ⇒ same height
	 * as any live count. */
	measure_text(cr, "\xe2\x9c\x89 0", &th);
	bw = badge_need_w(cr) - 8;
	if (bw > avail)
		bw = avail;
	bx = clock_x - 8 - bw;
	if (total) {
		int tw;
		snprintf(label, sizeof label, "\xe2\x9c\x89 %d", total); /* ✉ N */
		tw = measure_text(cr, label, &th);
		/* Pathological N >= 100: grow left rather than clip. */
		if (tw + 16 > bw) {
			bw = tw + 16 <= avail ? tw + 16 : avail;
			bx = clock_x - 8 - bw;
		}
	} else {
		label[0] = '\0'; /* blank placeholder: no label at all */
	}
	{
		double by = (h - (th + 8)) / 2.0;
		if (by < 2)
			by = 2;
		pill_rect(cr, bx, by, bw, th + 8, 8);
		if (total) {
			cairo_set_source_rgb(cr, 0.28, 0.23, 0.10);
			cairo_fill_preserve(cr);
			cairo_set_source_rgb(cr, 0.85, 0.68, 0.25);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
			cairo_set_source_rgb(cr, 1.0, 0.88, 0.55);
			draw_btn_label(cr, bx, bw, h, label);
		} else {
			/* WHY dim, not hidden: a stable anchor the eye learns
			 * once; arrivals replace the blank in place. */
			cairo_set_source_rgb(cr, 0.14, 0.14, 0.15);
			cairo_fill_preserve(cr);
			cairo_set_source_rgb(cr, 0.32, 0.32, 0.34);
			cairo_set_line_width(cr, 1);
			cairo_stroke(cr);
		}
	}
	badge_tx = bx;
	badge_tw = bw;
	return (clock_x - 8 - bx) + 8;
}

/* Real buffer management: keep one memfd + pool per size. */
static int shm_fd = -1;
static struct wl_shm_pool *shm_pool = NULL;
static int pool_w = 0, pool_h = 0;

static void render_frame(void)
{
	int w = surf_w, h = cfg.height; /* fixed bar height, no dropdown */
	if (w < 100)
		w = 100;
	if (w != pool_w || h != pool_h) {
		if (shm_pool)
			wl_shm_pool_destroy(shm_pool);
		if (shm_fd >= 0)
			close(shm_fd);
		if (shm_data)
			munmap(shm_data, shm_size);
		size_t need = (size_t)w * h * 4;
		shm_fd = memfd_create("monopanel", MFD_CLOEXEC);
		if (shm_fd < 0 || ftruncate(shm_fd, need) < 0)
			return;
		shm_data = mmap(NULL, need, PROT_READ | PROT_WRITE,
				 MAP_SHARED, shm_fd, 0);
		if (shm_data == MAP_FAILED) {
			close(shm_fd);
			shm_fd = -1;
			return;
		}
		shm_size = need;
		shm_pool = wl_shm_create_pool(shm, shm_fd, need);
		pool_w = w;
		pool_h = h;
	}
	if (buf)
		wl_buffer_destroy(buf);
	buf = wl_shm_pool_create_buffer(shm_pool, 0, w, h, w * 4,
					WL_SHM_FORMAT_ARGB8888);

	cairo_surface_t *cs = cairo_image_surface_create_for_data(
		shm_data, CAIRO_FORMAT_ARGB32, w, h, w * 4);
	cairo_t *cr = cairo_create(cs);
	/* bar background: near-black, opaque (no wallpaper behind us anyway) */
	cairo_set_source_rgb(cr, 0.08, 0.08, 0.09);
	cairo_paint(cr);

	int x = 8;
	draw_signal_bars(cr, x, h);
	x += 4 * (4 + 2) + 8;
	x += draw_wifi_text(cr, x, h);
	wifi_zone_w = x + 12; /* clicks left of here open the wifi menu */

	char clock[64];
	time_t now = time(NULL);
	strftime(clock, sizeof clock, cfg.clock_format, localtime(&now));
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(cfg.font);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, clock, -1);
	int tw, th;
	pango_layout_get_pixel_size(lo, &tw, &th);
	int clock_x = w - 8 - tw; /* sliders (below) yield to this */
	cairo_set_source_rgb(cr, 0.85, 0.85, 0.85);
	cairo_move_to(cr, clock_x, (h - th) / 2);
	pango_cairo_show_layout(cr, lo);
	g_object_unref(lo);

	/* Sliders sit between wifi and clock; each yields if the bar is too
	 * narrow, so wifi + clock stay usable on small widths. */
	char bri_label[32], mic_label[32], vol_label[32];
	if (bri_pct >= 0)
		snprintf(bri_label, sizeof bri_label, "Bri %d%%", bri_pct);
	else
		snprintf(bri_label, sizeof bri_label, "Bri --");
	if (mic_pct >= 0)
		snprintf(mic_label, sizeof mic_label,
			 mic_muted ? "Mic %d%% MUTE" : "Mic %d%%", mic_pct);
	else
		snprintf(mic_label, sizeof mic_label, "Mic --");
	if (vol_pct >= 0)
		snprintf(vol_label, sizeof vol_label,
			 vol_muted ? "Vol %d%% MUTE" : "Vol %d%%", vol_pct);
	else
		snprintf(vol_label, sizeof vol_label, "Vol --");
	int sx = (int)wifi_zone_w + 10;
	bri_tx = vol_tx = mic_tx = -1; /* hidden until drawn (hit-test guard) */
	mic_gx = -1;
	/* WHY fixed cells: worst-case label widths, measured once per frame
	 * in the live font, so 9%->10%->100% never moves a track rect. */
	int bri_cell = measure_text(cr, "Bri 100%", NULL);
	int vol_cell = measure_text(cr, "Vol 100% MUTE", NULL);
	int mic_cell = measure_text(cr, "Mic 100% MUTE", NULL);
	{
		int w2 = measure_text(cr, "Bri --", NULL);
		if (w2 > bri_cell)
			bri_cell = w2;
		w2 = measure_text(cr, "Vol --", NULL);
		if (w2 > vol_cell)
			vol_cell = w2;
		w2 = measure_text(cr, "Mic --", NULL);
		if (w2 > mic_cell)
			mic_cell = w2;
	}
	/* The icon reserves its width BEFORE sliders: sliders yield to the
	 * clock-adjacent anchor whether it shows the count or the blank
	 * placeholder, never the reverse. Below a 40px floor the icon still
	 * yields to wifi+clock; see draw_badge. The reserve is fixed
	 * (badge_need_w), so the bar never shifts when the count changes. */
	int badge_need = badge_need_w(cr);
	int slider_limit = clock_x - 8;
	if (sx + badge_need <= clock_x - 8)
		slider_limit -= badge_need;
	if (sx + bri_cell + 6 + SLIDER_W <= slider_limit) {
		sx += draw_slider(cr, sx, h, bri_label, bri_cell, bri_pct,
				  1.0, 0.78, 0.25, &bri_tx, &bri_tw);
		sx += SLIDER_GAP;
		/* Mic sits immediately left of Vol: glyph icon + its own
		 * fixed-cell slider (same track/knob/drag/wheel discipline
		 * as Bri/Vol, green fill instead of blue so the two audio
		 * tracks read apart). Mic and Vol are SEQUENTIAL siblings
		 * inside Bri (not nested): each checks the live sx and only
		 * advances it when drawn, so a narrow bar degrades
		 * Bri+Mic+Vol -> Bri+Mic -> Bri+Vol -> Bri instead of
		 * dropping Vol just because Mic did not fit. Badge reserve
		 * + wifi + clock always win on narrow bars. */
		if (sx + MIC_GLYPH_W + 4 + mic_cell + 6 + SLIDER_W <=
		    slider_limit) {
			sx += draw_mic_glyph(cr, sx, h, mic_muted);
			sx += 4;
			sx += draw_slider(cr, sx, h, mic_label, mic_cell,
					  mic_pct,
					  mic_muted ? 0.55 : 0.35,
					  mic_muted ? 0.55 : 0.80,
					  mic_muted ? 0.60 : 0.45, &mic_tx,
					  &mic_tw);
			sx += SLIDER_GAP;
		}
		if (sx + vol_cell + 6 + SLIDER_W <= slider_limit) {
			sx += draw_slider(cr, sx, h, vol_label,
					  vol_cell, vol_pct,
					  vol_muted ? 0.55 : 0.35,
					  vol_muted ? 0.55 : 0.75,
					  vol_muted ? 0.60 : 1.0,
					  &vol_tx, &vol_tw);
			sx += SLIDER_GAP;
		}
	}

	/* Single notification icon, rightmost and clock-adjacent. The icon
	 * reserve was already subtracted from slider_limit above
	 * (badge_need_w, fixed whether blank or live), so the return value
	 * needs no further use; clock_x stays the anchor. */
	draw_badge(cr, sx, h, clock_x);

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, w, h);
	wl_surface_commit(surf);
	wl_display_flush(dpy);
}

/* ---------- brightness + volume: poll, parse, set ---------- */
/* WHY sysfs for brightness reads: no fork per poll (a 2s cadence would fork
 * brightnessctl 30x/min); brightnessctl is only exec'd on user drag/scroll.
 * WHY async capture for wpctl: g_spawn_sync would stall the bar while
 * PipeWire answers; the child-watch path never blocks the main loop.
 * Writes use g_spawn_async with argv (never shell; no secrets involved). */

static void bri_poll(void)
{
	/* WHY 2s cache: sysfs reads are cheap but render ticks 1Hz; the
	 * cache also holds our optimistic drag value briefly. */
	gint64 now = g_get_monotonic_time();
	if (now < bri_cache_until)
		return;
	bri_cache_until = now + 2 * G_TIME_SPAN_SECOND;
	char *cur_s = NULL, *max_s = NULL;
	gboolean bri_ok = FALSE;
	long cur = -1, mx = -1;
	if (g_file_get_contents(
		    "/sys/class/backlight/intel_backlight/brightness", &cur_s,
		    NULL, NULL) &&
	    g_file_get_contents(
		    "/sys/class/backlight/intel_backlight/max_brightness",
		    &max_s, NULL, NULL)) {
		cur = atol(cur_s);
		mx = atol(max_s);
		bri_ok = (mx > 0 && cur >= 0);
	}
	if (bri_ok)
		/* WHY read clamp is 0..100 (not 1..100): the bar must
		 * show 0% when the screen is off; only writes stay
		 * floored at 1% (see bri_set_pct). Rounding matches the
		 * compositor's bright_read_pct. */
		bri_pct = CLAMP((int)((cur * 100 + mx / 2) / mx), 0, 100);
	else
		/* WHY reset to unknown (TASK-014): sysfs going unreadable
		 * (unbound/rebound backlight) must read "Bri --" within one
		 * poll, never a stale level — same rule as vol/mic. */
		bri_pct = -1;
	g_free(cur_s);
	g_free(max_s);
}

static void bri_set_pct(int pct)
{
	const char *dev;
	/* WHY unknown-guard (TASK-014, mirrors mic_set_pct from TASK-013):
	 * bri_pct == -1 ("Bri --", sysfs unreadable) and a drag would
	 * otherwise plant a phantom level no poll can confirm. Bri CAN be
	 * unknown (see the -1 init above), so this guard is live, not dead.
	 * A reappearing backlight re-enables control via the next poll. */
	if (bri_pct < 0)
		return;
	pct = CLAMP(pct, 1, 100); /* WHY min 1: 0 would blank the backlight */
	if (pct == bri_pct)
		return; /* WHY: drag motion emits many repeats of the same
			* value; skip the fork when nothing changed. */
	bri_pct = pct; /* optimistic; next poll confirms */
	bri_cache_until = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
	char arg[8];
	snprintf(arg, sizeof arg, "%d%%", pct);
	/* WHY -d: pin the single intel_backlight device (BRIGHT_DEV override)
	 * so a second backlight can never hijack control. */
	dev = g_getenv("BRIGHT_DEV");
	if (!dev || !*dev)
		dev = "intel_backlight";
	char *argv[] = { "brightnessctl", "-d", (char *)dev, "s", arg,
			 NULL };
	/* argv/arg live through fork; the child execs before we return. */
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
		      NULL, NULL);
	if (configured)
		render_frame();
}

/* Run argv (no shell) and hand its stdout to cb from a child watch. */
typedef void (*out_cb)(char *out, gsize len, gboolean ok);
struct child_req {
	int fd;
	GString *buf;
	out_cb cb;
};

static void child_watch_cb(GPid pid, gint status, gpointer data)
{
	struct child_req *r = data;
	/* WHY thread exit status (TASK-014): a nonzero exit is poll failure
	 * even with plausible stdout — but exit status ALONE is not enough
	 * either, because wpctl prints EMPTY stdout with rc=0 for a gone
	 * device (measured live). The result cbs OR this flag with the
	 * empty/unparseable-output check, so either leg resets to unknown. */
	gboolean ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	char tmp[512];
	ssize_t n;
	/* WHY read-after-exit: output is tiny and already in the pipe, so
	 * this cannot block; no GIOChannel bookkeeping needed. */
	while ((n = read(r->fd, tmp, sizeof tmp)) > 0)
		g_string_append_len(r->buf, tmp, (gssize)n);
	close(r->fd);
	r->cb(r->buf->str, r->buf->len, ok);
	g_string_free(r->buf, TRUE);
	g_free(r);
	g_spawn_close_pid(pid);
}

static gboolean spawn_capture(char **argv, out_cb cb)
{
	GPid pid = 0;
	int outfd = -1;
	GError *err = NULL;
	struct child_req *r = g_new0(struct child_req, 1);
	r->buf = g_string_new(NULL);
	r->cb = cb;
	if (!g_spawn_async_with_pipes(NULL, argv, NULL,
				      G_SPAWN_SEARCH_PATH |
					      G_SPAWN_DO_NOT_REAP_CHILD |
					      G_SPAWN_STDERR_TO_DEV_NULL,
				      NULL, NULL, &pid, NULL, &outfd, NULL,
				      &err)) {
		g_clear_error(&err);
		g_string_free(r->buf, TRUE);
		g_free(r);
		return FALSE;
	}
	int fl = fcntl(outfd, F_GETFL, 0);
	if (fl >= 0)
		fcntl(outfd, F_SETFL, fl | O_NONBLOCK);
	r->fd = outfd;
	g_child_watch_add(pid, child_watch_cb, r);
	return TRUE;
}

static gboolean vol_parse(const char *out)
{
	/* wpctl prints "Volume: 0.42" plus " [MUTED]" when muted. */
	double v = -1;
	/* WHY bool (TASK-014): empty/unparseable stdout is poll failure, not
	 * "no news" — wpctl prints EMPTY stdout with rc=0 for a gone sink
	 * (measured live), so the caller resets to unknown on FALSE instead
	 * of keeping a stale level. Muted assigns only on success. */
	if (sscanf(out, "Volume: %lf", &v) != 1 || v < 0 || v > 1.5)
		return FALSE;
	vol_pct = CLAMP((int)(v * 100.0 + 0.5), 0, 100);
	vol_muted = (strstr(out, "MUTED") || strstr(out, "muted")) ? 1 : 0;
	return TRUE;
}

static void vol_result_cb(char *out, gsize len, gboolean ok)
{
	vol_inflight = FALSE;
	/* WHY nonzero-exit OR empty/garbled stdout resets to unknown
	 * (TASK-014): a hot-removed sink must read "Vol --" within one poll,
	 * never a stale level. Muted clears too — a gone sink has no mute
	 * state to show. Success assigns exactly as before. */
	if (!ok || len == 0 || !vol_parse(out)) {
		vol_pct = -1;
		vol_muted = 0;
	}
	if (configured)
		render_frame();
}

static void vol_poll(void)
{
	gint64 now = g_get_monotonic_time();
	if (vol_inflight || now < vol_cache_until)
		return;
	vol_cache_until = now + 2 * G_TIME_SPAN_SECOND;
	vol_inflight = TRUE;
	char *argv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@",
			 NULL };
	if (!spawn_capture(argv, vol_result_cb)) {
		/* WHY spawn failure resets too (TASK-014): a failed fork
		 * means no result_cb will ever arrive, so without this the
		 * bar would hold a stale level indefinitely. */
		vol_inflight = FALSE;
		vol_pct = -1;
		vol_muted = 0;
		if (configured)
			render_frame();
	}
}

static void vol_set_pct(int pct)
{
	/* WHY unknown-guard (TASK-014, mirrors mic_set_pct from TASK-013):
	 * with no sink vol_pct == -1 ("Vol --") and a drag would otherwise
	 * plant a phantom level that failed polls can never restore to -1.
	 * Polls retry every 2s, so a hot-plugged sink re-enables control on
	 * its own. Kept as `< 0` (not `<= 0`) so 0% stays adjustable. */
	if (vol_pct < 0)
		return;
	pct = CLAMP(pct, 0, 100);
	if (pct == vol_pct)
		return; /* WHY: same drag-storm fork guard as bri_set_pct. */
	vol_pct = pct; /* optimistic; wpctl confirms on next poll */
	vol_cache_until = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
#ifndef MONOPANEL_TEST
	/* WHY no live spawn under test (TASK-014 review): the headless
	 * harness asserts the optimistic globals change; forking real
	 * wpctl would mutate the live sink every `make check`. */
	char frac[16];
	snprintf(frac, sizeof frac, "%.2f", pct / 100.0);
	char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@",
			 frac, NULL };
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
		      NULL, NULL);
#endif
	if (configured)
		render_frame();
}

/* Mic input: same async-capture discipline as Vol (never blocks the
 * bar), but against @DEFAULT_AUDIO_SOURCE@. No source override needed:
 * with no mic, get-volume fails, mic_pct stays -1, and the bar shows
 * "Mic --" exactly like the "Vol --" pattern. Writes use g_spawn_async
 * with argv (never shell; no secrets involved). */
static gboolean mic_parse(const char *out)
{
	/* wpctl prints "Volume: 0.42" plus " [MUTED]" when muted. */
	double v = -1;
	/* WHY bool (TASK-014): same empty-stdout-with-rc=0 shape as vol —
	 * the caller resets to unknown on FALSE (see vol_parse). */
	if (sscanf(out, "Volume: %lf", &v) != 1 || v < 0 || v > 1.5)
		return FALSE;
	mic_pct = CLAMP((int)(v * 100.0 + 0.5), 0, 100);
	mic_muted = (strstr(out, "MUTED") || strstr(out, "muted")) ? 1 : 0;
	return TRUE;
}

static void mic_result_cb(char *out, gsize len, gboolean ok)
{
	mic_inflight = FALSE;
	/* WHY nonzero-exit OR empty/garbled stdout resets to unknown
	 * (TASK-014): a hot-removed source must read "Mic --" within one
	 * poll, never a stale level (tester replica proved mic_parse("")
	 * left 65 stale). Success assigns exactly as before. */
	if (!ok || len == 0 || !mic_parse(out)) {
		mic_pct = -1;
		mic_muted = 0;
	}
	if (configured)
		render_frame();
}

static void mic_poll(void)
{
	gint64 now = g_get_monotonic_time();
	if (mic_inflight || now < mic_cache_until)
		return;
	mic_cache_until = now + 2 * G_TIME_SPAN_SECOND;
	mic_inflight = TRUE;
	char *argv[] = { "wpctl", "get-volume", "@DEFAULT_AUDIO_SOURCE@",
			 NULL };
	if (!spawn_capture(argv, mic_result_cb)) {
		/* WHY spawn failure resets too (TASK-014): same no-callback-
		 * ever-arrives reasoning as vol_poll. */
		mic_inflight = FALSE;
		mic_pct = -1;
		mic_muted = 0;
		if (configured)
			render_frame();
	}
}

static void mic_set_pct(int pct)
{
	/* WHY unknown-guard: with no source mic_pct == -1 ("Mic --") and a
	 * drag would otherwise plant a phantom level that failed polls can
	 * never restore to -1. Polls retry every 2s, so a hot-plugged
	 * source re-enables control on its own (mirrors the wheel guard). */
	if (mic_pct < 0)
		return;
	pct = CLAMP(pct, 0, 100);
	if (pct == mic_pct)
		return; /* WHY: same drag-storm fork guard as bri/vol_set_pct. */
	mic_pct = pct; /* optimistic; wpctl confirms on next poll */
	mic_cache_until = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
#ifndef MONOPANEL_TEST
	/* WHY no live spawn under test (TASK-014 review): same harness
	 * reasoning as vol_set_pct — assert globals, never touch the
	 * live source. */
	char frac[16];
	snprintf(frac, sizeof frac, "%.2f", pct / 100.0);
	char *argv[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SOURCE@",
			 frac, NULL };
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
		      NULL, NULL);
#endif
	if (configured)
		render_frame();
}

static void mic_toggle_mute(void)
{
	/* WHY unknown-guard: same phantom-level reasoning as mic_set_pct —
	 * toggling "Mic --" would fork wpctl against no source and flip a
	 * mute flag no poll can confirm. Polls retry every 2s, so a
	 * hot-plugged source re-enables the toggle on its own. */
	if (mic_pct < 0)
		return;
	/* WHY optimistic flip: same 2s-hold pattern as the set paths; the
	 * next poll confirms against wpctl. */
	mic_muted = !mic_muted;
	mic_cache_until = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
#ifndef MONOPANEL_TEST
	/* WHY no live spawn under test (TASK-014 review): the harness
	 * asserts the optimistic flip; forking set-mute would toggle the
	 * live source once per `make check`. */
	char *argv[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@",
			 "toggle", NULL };
	g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
		      NULL, NULL);
#endif
	if (configured)
		render_frame();
}

/* Absolute position -> percent; knob-radius slop so thin-track grabs work. */
static int slider_pct_at(double tx, double tw, double x)
{
	if (tw <= 0)
		return -1;
	double f = (x - tx) / tw;
	f = CLAMP(f, 0.0, 1.0);
	return (int)(f * 100.0 + 0.5);
}

static int in_bri(double x)
{
	return bri_tx >= 0 && x >= bri_tx - 6 && x <= bri_tx + bri_tw + 6;
}

static int in_vol(double x)
{
	return vol_tx >= 0 && x >= vol_tx - 6 && x <= vol_tx + vol_tw + 6;
}

static int in_mic(double x)
{
	return mic_tx >= 0 && x >= mic_tx - 6 && x <= mic_tx + mic_tw + 6;
}

/* Glyph cell only (mute toggle); ±3 slop, still far short of the track
 * zone, which starts a full label cell to the right. */
static int in_mic_glyph(double x)
{
	return mic_gx >= 0 && x >= mic_gx - 3 &&
	       x <= mic_gx + mic_gw + 3;
}

/* ---------- layer-shell / seat handlers ---------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *s,
			    uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	(void)s;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(s, serial);
	if ((int)w != 0)
		surf_w = w;
	/* Bar height is fixed at cfg.height (TASK-010 removed the expandable
	 * dropdown); h only echoes a compositor clamp. */
	configured = 1;
	render_frame();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *s)
{
	(void)data;
	(void)s;
	_exit(0); /* compositor killed our bar; nothing to save */
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void spawn_wifi_menu(void)
{
	/* Same dir as this binary; never via shell, no secrets in argv.
	 * WHY loud failure: the old code ignored g_spawn_async's return, so
	 * a dead click looked identical to a working one. Now every path
	 * logs to stderr (lands in /tmp/monowl.log via run-monowl.sh) and
	 * pops notify-send, so the next dead click leaves evidence. */
	char self[4096];
	ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0) {
		fprintf(stderr, "monopanel: wifi click: readlink /proc/self/exe failed\n");
		return;
	}
	self[n] = '\0';
	char *slash = strrchr(self, '/');
	if (!slash) {
		fprintf(stderr, "monopanel: wifi click: no slash in exe path\n");
		return;
	}
	*slash = '\0';
	/* WHY wifi-gui preference: the wifi zone now opens the full GTK manager
	 * when it is built; the fuzzel menu stays wired as fallback so the zone
	 * never dead-ends on a clean tree. */
	char *argv[5];
	int argi = 0;
	char *helper, *lines = NULL;
	char *gui = g_strconcat(self, "/../tools/wifi-gui/wifi-gui", NULL);
	if (access(gui, X_OK) == 0) {
		helper = gui;
	} else {
		g_free(gui);
		helper = g_strconcat(self, "/monopanel-wifi", NULL);
		lines = g_strdup_printf("%d", cfg.menu_lines);
	}
	argv[argi++] = helper;
	if (lines) {
		argv[argi++] = "-l";
		argv[argi++] = lines;
	}
	argv[argi] = NULL;
	GError *err = NULL;
	fprintf(stderr, "monopanel: wifi click x=%.0f zone=%.0f spawning %s\n",
		pointer_x, wifi_zone_w, helper);
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
			   NULL, &err)) {
		fprintf(stderr, "monopanel: wifi click: spawn %s failed: %s\n",
			helper, err ? err->message : "unknown");
		char *nargv[] = { "notify-send", "monopanel-wifi",
				  "menu spawn failed (see /tmp/monowl.log)", NULL };
		g_spawn_async(NULL, nargv, NULL, G_SPAWN_SEARCH_PATH, NULL,
			      NULL, NULL, NULL);
		g_clear_error(&err);
	}
	g_free(lines);
	g_free(helper);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	if (button == BTN_LEFT && pointer_on_bar && pointer_x >= 0) {
		if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
			/* Sliders first: press+drag sets the value. The wifi
			 * branch keeps its exact zone (left of wifi_zone_w);
			 * slider zones sit right of it so they never overlap.
			 * Icon (badge) is rightmost: it opens the preview popup,
			 * never dismisses anything directly. */
			if (in_bri(pointer_x)) {
				drag_target = 1;
				int p = slider_pct_at(bri_tx, bri_tw,
						      pointer_x);
				if (p >= 0)
					bri_set_pct(p);
			} else if (in_mic_glyph(pointer_x)) {
				/* Glyph click toggles the source mute; the
				 * track (below) adjusts level instead. */
				mic_toggle_mute();
			} else if (in_mic(pointer_x)) {
				drag_target = 3;
				int p = slider_pct_at(mic_tx, mic_tw,
						      pointer_x);
				if (p >= 0)
					mic_set_pct(p);
			} else if (in_vol(pointer_x)) {
				drag_target = 2;
				int p = slider_pct_at(vol_tx, vol_tw,
						      pointer_x);
				if (p >= 0)
					vol_set_pct(p);
			} else if (in_badge(pointer_x)) {
				/* Blank icon click = no-op; a live count pill
				 * opens the preview popup. */
				if (notif_count + q_count > 0)
					spawn_preview();
			} else if (pointer_x < wifi_zone_w) {
				spawn_wifi_menu();
			}
		} else {
			drag_target = 0; /* release ends any drag */
		}
	} else if (state != WL_POINTER_BUTTON_STATE_PRESSED) {
		drag_target = 0;
	}
}
static void pointer_enter(void *d, struct wl_pointer *p, uint32_t s,
			  struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)s;
	(void)y;
	pointer_on_bar = (sf == surf);
	pointer_x = wl_fixed_to_double(x);
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s,
			  struct wl_surface *sf)
{
	(void)d;
	(void)p;
	(void)s;
	(void)sf;
	pointer_on_bar = 0;
	pointer_x = -1;
	drag_target = 0; /* never keep a stale drag off-bar */
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t,
			   wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)t;
	(void)y;
	pointer_x = wl_fixed_to_double(x);
	if (drag_target == 1) {
		int p = slider_pct_at(bri_tx, bri_tw, pointer_x);
		if (p >= 0)
			bri_set_pct(p);
	} else if (drag_target == 3) {
		int p = slider_pct_at(mic_tx, mic_tw, pointer_x);
		if (p >= 0)
			mic_set_pct(p);
	} else if (drag_target == 2) {
		int p = slider_pct_at(vol_tx, vol_tw, pointer_x);
		if (p >= 0)
			vol_set_pct(p);
	}
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a,
			 wl_fixed_t v)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
	(void)v;
}
/* No-op tail of wl_pointer_listener: seat is bound at v5, so the
 * compositor may send frame/axis_source/axis_stop/axis_discrete. A NULL
 * handler aborts the client (listener for opcode 5 is NULL). */
static void pointer_frame(void *d, struct wl_pointer *p)
{
	(void)d;
	(void)p;
}
static void pointer_axis_source(void *d, struct wl_pointer *p, uint32_t s)
{
	(void)d;
	(void)p;
	(void)s;
}
static void pointer_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
			      uint32_t a)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
}
static void pointer_axis_discrete(void *d, struct wl_pointer *p, uint32_t a,
				  int32_t n)
{
	(void)d;
	(void)p;
	/* WHY discrete only: wheel mice emit one discrete step per detent;
	 * handling the continuous axis too would double-step under labwc,
	 * which sends both. Trackpads drag instead. Sign: up (away) is
	 * negative and raises the value. */
	if (!pointer_on_bar || pointer_x < 0 ||
	    a != WL_POINTER_AXIS_VERTICAL_SCROLL || n == 0)
		return;
	if (in_bri(pointer_x) && bri_pct >= 0)
		bri_set_pct(bri_pct - n * 5);
	else if ((in_mic(pointer_x) || in_mic_glyph(pointer_x)) &&
		 mic_pct >= 0)
		mic_set_pct(mic_pct - n * 5);
	else if (in_vol(pointer_x) && vol_pct >= 0)
		vol_set_pct(vol_pct - n * 5);
}
static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_enter,
	.leave = pointer_leave,
	.motion = pointer_motion,
	.button = pointer_button,
	.axis = pointer_axis,
	.frame = pointer_frame,
	.axis_source = pointer_axis_source,
	.axis_stop = pointer_axis_stop,
	.axis_discrete = pointer_axis_discrete,
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n)
{
	(void)d;
	(void)s;
	(void)n;
}
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

static void reg_global(void *data, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	(void)data;
	if (!strcmp(iface, wl_compositor_interface.name))
		comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
	else if (!strcmp(iface, wl_shm_interface.name))
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		layer_shell = wl_registry_bind(r, name,
					       &zwlr_layer_shell_v1_interface,
					       MIN(ver, 4));
	else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	}
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d;
	(void)r;
	(void)n;
}
static const struct wl_registry_listener reg_listener = { reg_global,
							  reg_remove };

static gboolean clock_tick(gpointer unused)
{
	(void)unused;
	bri_poll(); /* sysfs: cheap, self-throttled to 2s */
	vol_poll(); /* async wpctl: never blocks, self-throttled to 2s */
	mic_poll(); /* async wpctl source: never blocks, self-throttled */
	notif_refresh(); /* dir scan: ~µs, queue is near-empty */
	if (configured)
		render_frame();
	return G_SOURCE_CONTINUE;
}

static gboolean wl_dispatch_cb(GIOChannel *src, GIOCondition cond,
			       gpointer unused)
{
	(void)src;
	(void)cond;
	(void)unused;
	/* External-main-loop read side: glib already polled for us, so
	 * prepare + read + dispatch_pending here. dispatch_pending alone
	 * would never read the socket (configure would never arrive);
	 * wl_display_dispatch() would block in quiet periods and stall
	 * the 1s clock. */
	while (wl_display_prepare_read(dpy) != 0)
		wl_display_dispatch_pending(dpy);
	if (wl_display_read_events(dpy) < 0)
		_exit(1); /* compositor gone */
	if (wl_display_dispatch_pending(dpy) < 0)
		_exit(1);
	wl_display_flush(dpy); /* push ack_configure etc. */
	return G_SOURCE_CONTINUE;
}

static void nm_ready(GObject *src, GAsyncResult *res, gpointer unused)
{
	(void)unused;
	GError *err = NULL;
	nm = nm_client_new_finish(res, &err);
	if (!nm) {
		snprintf(status_text, sizeof status_text, "nm offline");
		g_clear_error(&err);
		return;
	}
	(void)src;
	wifi_refresh();
	render_frame();
}

#ifdef MONOPANEL_TEST
/* TASK-010 headless: the bar is a count-only view (no text, no unlink).
 * Proves notif_refresh is read-only, split counts feed one total, junk
 * names are ignored, badge reserve is fixed blank-vs-live and
 * right-anchored, blank click is a no-op. FIFO preview rows live in
 * notification-gui --preview (tested via --preview --dry-run); the panel
 * only owns counts + the icon. Needs glib + cairo + temp dir only. */
static int t_fail = 0;
#define TCHECK(c, msg) \
	do { \
		if (!(c)) { \
			fprintf(stderr, "FAIL %d: %s\n", __LINE__, msg); \
			t_fail = 1; \
		} \
	} while (0)

int main(int argc, char **argv)
{
	char *tmp, *a, *b, *c, *d, *q, *ans, *junk, *shortn;
	(void)argc;
	(void)argv;
	tmp = g_dir_make_tmp("monopanel-test-XXXXXX", NULL);
	TCHECK(tmp != NULL, "mktemp dir");
	if (!tmp)
		return 1;
	g_free(notif_dir);
	notif_dir = g_strdup(tmp);
	cfg_init();
	a = g_build_filename(tmp, "a.txt", NULL);
	b = g_build_filename(tmp, "b.txt", NULL);
	c = g_build_filename(tmp, "c.txt", NULL);
	d = g_build_filename(tmp, "d.txt", NULL);
	q = g_build_filename(tmp, "q1.q", NULL);
	ans = g_build_filename(tmp, "q1.answer", NULL);
	junk = g_build_filename(tmp, "note.tmp", NULL);
	shortn = g_build_filename(tmp, "ab", NULL);
	TCHECK(g_file_set_contents(a, "alpha", -1, NULL), "write a.txt");
	TCHECK(g_file_set_contents(b, "bravo", -1, NULL), "write b.txt");
	TCHECK(g_file_set_contents(c, "charlie", -1, NULL), "write c.txt");
	TCHECK(g_file_set_contents(d, "delta", -1, NULL), "write d.txt");
	TCHECK(g_file_set_contents(q, "Pick?\nYes\nNo\n", -1, NULL),
	       "write q1.q");
	TCHECK(g_file_set_contents(ans, "yes", -1, NULL), "write rendezvous");
	TCHECK(g_file_set_contents(junk, "junk", -1, NULL), "write junk");
	TCHECK(g_file_set_contents(shortn, "x", -1, NULL), "write short");
	notif_refresh();
	TCHECK(notif_count == 4, "four plain counted");
	TCHECK(q_count == 1, "one question counted");
	TCHECK(notif_count + q_count == 5, "single total is the sum");
	TCHECK(g_file_test(a, G_FILE_TEST_EXISTS) &&
	       g_file_test(b, G_FILE_TEST_EXISTS) &&
	       g_file_test(c, G_FILE_TEST_EXISTS) &&
	       g_file_test(d, G_FILE_TEST_EXISTS) &&
	       g_file_test(q, G_FILE_TEST_EXISTS),
	       "refresh is read-only: all queue files survive");
	/* Short/junk/rendezvous names never feed the counts (guards above). */
	/* TASK-013 mic: parse mirrors vol_parse (level + [MUTED] flag). */
	vol_pct = -1;
	vol_muted = 0;
	vol_parse("Volume: 0.42");
	TCHECK(vol_pct == 42 && vol_muted == 0, "vol_parse level 0.42");
	vol_parse("Volume: 0.00 [MUTED]");
	TCHECK(vol_pct == 0 && vol_muted == 1, "vol_parse muted flag");
	mic_pct = -1;
	mic_muted = 0;
	mic_parse("Volume: 0.65");
	TCHECK(mic_pct == 65 && mic_muted == 0, "mic_parse level 0.65");
	mic_parse("Volume: 0.65 [MUTED]");
	TCHECK(mic_pct == 65 && mic_muted == 1, "mic_parse muted flag");
	mic_parse("Volume: 0.25 [muted]");
	TCHECK(mic_muted == 1, "mic_parse lowercase muted");
	/* TASK-013 unknown-guard: a drag/click on "Mic --" (mic_pct == -1)
	 * must be a no-op — no phantom level, no wpctl fork. Failed polls
	 * can never restore -1, so the guard is the only thing stopping a
	 * stuck fake level; hot-plug polls re-enable control on their own. */
	mic_pct = -1;
	mic_muted = 0;
	mic_set_pct(50);
	TCHECK(mic_pct == -1, "mic_set_pct from unknown is a no-op");
	mic_toggle_mute();
	TCHECK(mic_pct == -1 && mic_muted == 0,
	       "mic_toggle_mute from unknown is a no-op");
	/* TASK-014 poll failure must restore unknown: empty stdout (the
	 * measured hot-remove shape — wpctl prints nothing with rc=0),
	 * garbled output, or a nonzero exit resets to -1, never stale;
	 * success still parses exactly as before. */
	int t14_enter = t_fail;
	vol_pct = 42;
	vol_muted = 0;
	vol_result_cb("", 0, TRUE);
	TCHECK(vol_pct == -1 && vol_muted == 0,
	       "vol_result_cb empty stdout resets to unknown");
	mic_pct = 65;
	mic_muted = 1;
	mic_result_cb("", 0, TRUE);
	TCHECK(mic_pct == -1 && mic_muted == 0,
	       "mic_result_cb empty stdout resets to unknown");
	vol_pct = 42;
	vol_result_cb("garbage", 7, TRUE);
	TCHECK(vol_pct == -1 && vol_muted == 0,
	       "vol_result_cb garbled resets to unknown");
	mic_pct = 65;
	mic_result_cb("Volume: abc", 11, TRUE);
	TCHECK(mic_pct == -1 && mic_muted == 0,
	       "mic_result_cb garbled resets to unknown");
	vol_pct = 42;
	vol_result_cb("Volume: 0.42", 12, FALSE);
	TCHECK(vol_pct == -1 && vol_muted == 0,
	       "vol_result_cb nonzero exit resets to unknown");
	mic_pct = 65;
	mic_result_cb("Volume: 0.65 [MUTED]", 20, FALSE);
	TCHECK(mic_pct == -1 && mic_muted == 0,
	       "mic_result_cb nonzero exit resets to unknown");
	vol_pct = -1;
	vol_muted = 0;
	vol_result_cb("Volume: 0.42", 12, TRUE);
	TCHECK(vol_pct == 42 && vol_muted == 0,
	       "vol_result_cb success still parses");
	mic_pct = -1;
	mic_muted = 0;
	mic_result_cb("Volume: 0.65 [MUTED]", 20, TRUE);
	TCHECK(mic_pct == 65 && mic_muted == 1,
	       "mic_result_cb success still parses muted");
	/* TASK-014 positive-direction locks (re-review): the `< 0` guards
	 * must not over-correct into `<= 0` — 0% and normal levels stay
	 * adjustable, and the mute toggle still flips from a known level.
	 * NOTE: the set/mute spawns are compiled out under MONOPANEL_TEST,
	 * so these assert the optimistic globals with zero live side
	 * effect. */
	vol_pct = 50;
	vol_muted = 0;
	vol_set_pct(60);
	TCHECK(vol_pct == 60, "vol_set_pct from known level still works");
	mic_pct = 50;
	mic_muted = 0;
	mic_set_pct(60);
	TCHECK(mic_pct == 60, "mic_set_pct from known level still works");
	mic_pct = 50;
	mic_muted = 0;
	mic_toggle_mute();
	TCHECK(mic_muted == 1,
	       "mic_toggle_mute from known level still flips");
	/* Vol unknown-guard negative control (mirrors the mic pair above). */
	vol_pct = -1;
	vol_muted = 0;
	vol_set_pct(50);
	TCHECK(vol_pct == -1, "vol_set_pct from unknown is a no-op");
	int t14_ok = (t_fail == t14_enter);
	/* Single icon: same fixed reserve blank or live, right-anchored. */
	{
		cairo_surface_t *cs;
		cairo_t *cr;
		int u0, u5;
		cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 800,
						cfg.height);
		cr = cairo_create(cs);
		notif_count = 0;
		q_count = 0;
		u0 = draw_badge(cr, 200, cfg.height, 780);
		TCHECK(u0 == badge_need_w(cr),
		       "blank consumes the full fixed reserve");
		TCHECK(badge_tx >= 0 &&
		       badge_tx + badge_tw == 780 - 8,
		       "blank pill right-anchored at clock_x");
		notif_count = 4;
		q_count = 1;
		u5 = draw_badge(cr, 200, cfg.height, 780);
		TCHECK(u5 == u0, "live total consumes the same reserve");
		TCHECK(badge_tx + badge_tw == 780 - 8,
		       "live total right-anchored at clock_x");
		/* Blank click: must return without spawning anything. */
		notif_count = 0;
		q_count = 0;
		u0 = draw_badge(cr, 200, cfg.height, 780);
		pointer_on_bar = 1;
		pointer_x = badge_tx + 1;
		pointer_button(NULL, NULL, 0, 0, BTN_LEFT,
			       WL_POINTER_BUTTON_STATE_PRESSED);
		TCHECK(1, "blank click no-op (returned)");
		/* Pathological total still draws inside the bar. */
		notif_count = 150;
		q_count = 5;
		u5 = draw_badge(cr, 200, cfg.height, 780);
		TCHECK(u5 > 0 && badge_tx >= 0,
		       "large total draws without clipping out");
		cairo_destroy(cr);
		cairo_surface_destroy(cs);
	}
	/* TASK-013 mic widget geometry: fixed glyph cell + fixed-cell
	 * slider, zones disjoint (glyph click toggles mute, track
	 * click/drag adjusts level, never each other). */
	{
		cairo_surface_t *cs;
		cairo_t *cr;
		cs = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 800,
						cfg.height);
		cr = cairo_create(cs);
		mic_gx = -1;
		mic_tx = -1;
		{
			int mx = 200;
			int gw = draw_mic_glyph(cr, mx, cfg.height, 0);
			int cell = measure_text(cr, "Mic 100% MUTE", NULL);
			/* WHY globals, not locals: draw_slider only fills
			 * the rects you hand it — render_frame passes
			 * &mic_tx/&mic_tw, and in_mic() reads those
			 * globals, so the test must wire them the same
			 * way or it probes stale -1 rects. */
			int used = draw_slider(cr, mx + gw + 4, cfg.height,
					       "Mic 50%", cell, 50, 0.35,
					       0.80, 0.45, &mic_tx, &mic_tw);
			(void)used;
			TCHECK(gw == MIC_GLYPH_W,
			       "glyph consumes its fixed zone");
			TCHECK(mic_gx == mx, "glyph hit rect anchored");
			TCHECK(mic_gx + mic_gw + 3 < mic_tx - 6,
			       "mic glyph and track zones disjoint");
			TCHECK(in_mic_glyph(mx + 1) && !in_mic(mx + 1),
			       "glyph click hits glyph only");
			TCHECK(in_mic(mic_tx + 1) && !in_mic_glyph(mic_tx + 1),
			       "track click hits track only");
			draw_mic_glyph(cr, mx, cfg.height, 1);
			TCHECK(mic_gx == mx,
			       "muted glyph keeps its hit rect");
		}
		cairo_destroy(cr);
		cairo_surface_destroy(cs);
	}
	unlink(a);
	unlink(b);
	unlink(c);
	unlink(d);
	unlink(q);
	unlink(ans);
	unlink(junk);
	unlink(shortn);
	rmdir(tmp);
	g_free(a);
	g_free(b);
	g_free(c);
	g_free(d);
	g_free(q);
	g_free(ans);
	g_free(junk);
	g_free(shortn);
	g_free(tmp);
	if (!t_fail)
		printf("notif count PASS\n");
	if (t14_ok)
		printf("poll failure PASS\n");
	return t_fail;
}
#else
int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	cfg_init();
	notif_dir_init();
	notif_refresh();
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "monopanel: no Wayland display\n");
		return 1;
	}
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !layer_shell) {
		fprintf(stderr, "monopanel: compositor lacks layer-shell\n");
		return 1;
	}
	surf = wl_compositor_create_surface(comp);
	layer_surf = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surf, NULL, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		"monopanel");
	zwlr_layer_surface_v1_set_anchor(
		layer_surf, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
				ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(layer_surf, 0, cfg.height);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surf, cfg.height);
	zwlr_layer_surface_v1_add_listener(layer_surf, &layer_listener, NULL);
	wl_surface_commit(surf);
	wl_display_flush(dpy); /* else our commit sits buffered: compositor
				* waits for it while we wait for configure */

	nm_client_new_async(NULL, nm_ready, NULL);

	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GIOChannel *ch = g_io_channel_unix_new(wl_display_get_fd(dpy));
	g_io_add_watch(ch, G_IO_IN, wl_dispatch_cb, NULL);
	g_io_channel_unref(ch);
	g_timeout_add_seconds(1, clock_tick, NULL);
	g_timeout_add_seconds(5, wifi_tick, NULL);
	g_main_loop_run(loop);
	return 0;
}
#endif /* MONOPANEL_TEST */
