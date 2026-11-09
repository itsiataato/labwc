/* notification-gui — centered layer-shell answer deck for question
 * notifications (*.q in $XDG_DATA_HOME/monowl/notifications).
 *
 * File contract (owned by the `notification` CLI; this file only reads):
 *   *.q line1 = question (512B cap); lines2+ = answers, one per line
 *   (1-10 for the answers type; 0 lines = classic Yes/No).
 *   Answering = write <base>.answer (selected text verbatim; "yes"/"no"
 *   lowercase for Yes/No, matching what the CLI waiter compares) FIRST,
 *   then unlink the .q. WHY answer-first: deleting the question first
 *   looks like a manual dismiss (waiter exits 2); the answer must exist
 *   first. Esc = dismiss = unlink .q with no .answer (waiter exits 2).
 *
 * WHY layer-shell overlay, not xdg toplevel: Super+D (showdesktop) hides
 * xdg clients but never touches layer surfaces, so this dialog survives
 * it. Exclusive zone 0 (reserves no bar space), anchored nowhere (= both
 * axes centered by the compositor), keyboard EXCLUSIVE so Esc reaches us
 * even while a fullscreen client has focus.
 *
 * Behavior: snapshot-free — after every answer/dismiss it rescans, shows
 * the next oldest question, and exits 0 when the queue is empty (also
 * exits 0 immediately at launch when empty, before touching Wayland).
 * Double-instance race: before writing .answer we check the .q still
 * exists; a lost race skips the write instead of littering a stale
 * .answer nobody waits on.
 *
 * Stack: raw wayland-client + wlr-layer-shell + cairo/pango + glib, same
 * as monopanel (no new deps). No secrets; paths confined to the queue
 * dir; spawns nothing.
 *
 *   notification-gui --help      usage, no display needed (exit 0)
 *   notification-gui --dry-run   list queued questions, no display (exit 0)
 */
#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE /* memfd_create */
#include <cairo/cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <linux/input-event-codes.h>
#include <pango/pangocairo.h>
#include <signal.h>
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

#define GUI_FONT "sans 11"
#define DLG_W 520
#define PAD 16
#define BTN_H 44
#define BTN_GAP 8
#define Q_GAP 12
#define MAX_ANS 10 /* contract cap; extras are ignored, never rendered */

static char *queue_dir = NULL;
static struct wl_display *dpy;
static struct wl_registry *reg;
static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_keyboard *keyboard;
static struct wl_surface *surf;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct zwlr_layer_surface_v1 *layer_surf;
static int dlg_h = 0;
static int configured = 0;

/* Current question (oldest .q). btn_val holds full verbatim answer strings
 * (Yes/No stored lowercase "yes"/"no" for CLI compat); rendering
 * ellipsizes them into the buttons. */
static char cur_base[256] = ""; /* any basename (NAME_MAX 255) fits */
static char *cur_q = NULL;
static char *btn_val[MAX_ANS];
static int n_btns = 0;
static int btn_is_yesno = 0;
static int btn_y[MAX_ANS];
static int hover_btn = -1;
static double pointer_x = -1;
static double pointer_y = -1;
static int pointer_on_dlg = 0;

static struct wl_buffer *buf = NULL;
static void *shm_data = NULL;
static size_t shm_size = 0;
static int shm_fd = -1;
static struct wl_shm_pool *shm_pool = NULL;
static int pool_w = 0, pool_h = 0;

static void render_frame(void); /* defined below; configure calls it */

/* ---------- queue ---------- */

/* Sorted oldest-first basenames of *.q files. Caller frees (strv). */
static GPtrArray *queue_qs(void)
{
	GPtrArray *names = g_ptr_array_new();
	if (!queue_dir)
		return names;
	GDir *d = g_dir_open(queue_dir, 0, NULL);
	if (!d)
		return names;
	const char *e;
	while ((e = g_dir_read_name(d))) {
		size_t n = strlen(e);
		if (n > 2 && !strcmp(e + n - 2, ".q"))
			g_ptr_array_add(names, g_strdup(e));
	}
	g_dir_close(d);
	/* WHY sort_values: g_ptr_array_sort hands the compar slots
	 * (char **), which g_strcmp0 misreads; _values hands elements. */
	g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
	return names;
}

static void free_current(void)
{
	g_free(cur_q);
	cur_q = NULL;
	for (int i = 0; i < n_btns; i++)
		g_free(btn_val[i]);
	n_btns = 0;
	cur_base[0] = '\0';
}

/* Load the oldest .q into cur_*. Returns 0 when the queue is empty. */
static int load_oldest(void)
{
	free_current();
	GPtrArray *names = queue_qs();
	int ok = 0;
	if (names->len) {
		const char *oldest = names->pdata[0];
		if (g_strlcpy(cur_base, oldest, sizeof cur_base) >=
		    sizeof cur_base)
			fprintf(stderr,
				"notification-gui: queue name truncated: %s\n",
				oldest);
		char *dot = strrchr(cur_base, '.');
		if (dot)
			*dot = '\0';
		char *p = g_build_filename(queue_dir, oldest, NULL);
		char *data = NULL;
		if (g_file_get_contents(p, &data, NULL, NULL) && data) {
			/* Line 1 = question; rest = answers. Keep full
			 * answer bytes verbatim (writer caps length); only
			 * the rendered label is ellipsized later. */
			char **lines = g_strsplit(data, "\n", -1);
			char *q = g_strstrip(lines[0] ? lines[0] : (char *)"");
			if (!*q || !g_utf8_validate(q, -1, NULL))
				cur_q = g_strdup(*q ? "(unreadable)"
						    : "(empty question)");
			else
				cur_q = g_strdup(q);
			for (int i = 1; lines[i] && n_btns < MAX_ANS; i++) {
				char *a = g_strstrip(lines[i]);
				if (!*a)
					continue; /* skip blank lines */
				if (!g_utf8_validate(a, -1, NULL))
					a = "(unreadable)";
				btn_val[n_btns++] = g_strdup(a);
			}
			g_strfreev(lines);
			g_free(data);
			btn_is_yesno = (n_btns == 0);
			if (btn_is_yesno) {
				btn_val[0] = g_strdup("yes");
				btn_val[1] = g_strdup("no");
				n_btns = 2;
			}
			ok = 1;
		}
		g_free(p);
	}
	for (guint i = 0; i < names->len; i++)
		g_free(names->pdata[i]);
	g_ptr_array_free(names, FALSE);
	return ok;
}

/* Measure the question block height for DLG_W so each question gets its
 * own exact dialog height. Uses a scratch image surface (no Wayland). */
static int question_h(void)
{
	cairo_surface_t *cs =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
	cairo_t *cr = cairo_create(cs);
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd =
		pango_font_description_from_string(GUI_FONT);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, cur_q ? cur_q : "", -1);
	pango_layout_set_width(lo, (DLG_W - 2 * PAD) * PANGO_SCALE);
	pango_layout_set_wrap(lo, PANGO_WRAP_WORD_CHAR);
	int w, h;
	pango_layout_get_pixel_size(lo, &w, &h);
	(void)w;
	g_object_unref(lo);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	return h;
}

static void layout_current(void)
{
	int qh = question_h();
	dlg_h = PAD + qh + Q_GAP + n_btns * BTN_H +
		(n_btns - 1) * BTN_GAP + PAD;
	int y = PAD + qh + Q_GAP;
	for (int i = 0; i < n_btns; i++) {
		btn_y[i] = y;
		y += BTN_H + BTN_GAP;
	}
	hover_btn = -1;
}

/* Write <base>.answer then unlink the .q (answer-first, see header).
 * Skips the write when the .q already vanished (double-instance race). */
static void answer_current(const char *text)
{
	if (!cur_base[0])
		return;
	char *qn = g_strconcat(cur_base, ".q", NULL);
	char *qp = g_build_filename(queue_dir, qn, NULL);
	if (access(qp, F_OK) == 0) {
		char *an = g_strconcat(cur_base, ".answer", NULL);
		char *ap = g_build_filename(queue_dir, an, NULL);
		GError *err = NULL;
		if (!g_file_set_contents(ap, text, -1, &err)) {
			fprintf(stderr,
				"notification-gui: answer write failed: %s\n",
				err ? err->message : "?");
			g_clear_error(&err);
		}
		if (unlink(qp) != 0)
			fprintf(stderr,
				"notification-gui: unlink %s failed\n", qn);
		g_free(an);
		g_free(ap);
	}
	g_free(qn);
	g_free(qp);
}

/* Dismiss = unlink .q with no .answer (waiter exits 2). */
static void dismiss_current(void)
{
	if (!cur_base[0])
		return;
	char *qn = g_strconcat(cur_base, ".q", NULL);
	char *qp = g_build_filename(queue_dir, qn, NULL);
	if (unlink(qp) != 0 && access(qp, F_OK) == 0)
		fprintf(stderr, "notification-gui: dismiss unlink %s failed\n",
			qn);
	g_free(qn);
	g_free(qp);
}

/* Next question or exit when the queue drained. */
static void advance(void)
{
	if (!load_oldest())
		exit(0);
	layout_current();
	zwlr_layer_surface_v1_set_size(layer_surf, DLG_W, dlg_h);
	wl_surface_commit(surf);
	render_frame();
}

/* ---------- rendering ---------- */

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

static void render_frame(void)
{
	int w = DLG_W, h = dlg_h;
	if (h < 100)
		h = 100;
	if (w != pool_w || h != pool_h) {
		if (shm_pool)
			wl_shm_pool_destroy(shm_pool);
		if (shm_fd >= 0)
			close(shm_fd);
		if (shm_data)
			munmap(shm_data, shm_size);
		size_t need = (size_t)w * h * 4;
		shm_fd = memfd_create("notification-gui", MFD_CLOEXEC);
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

	/* Dialog body: dark rounded rect, amber border. */
	pill_rect(cr, 0.5, 0.5, w - 1, h - 1, 12);
	cairo_set_source_rgb(cr, 0.10, 0.10, 0.11);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.85, 0.68, 0.25);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);

	/* Question, fully wrapped (writer caps 512B). */
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(GUI_FONT);
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, cur_q ? cur_q : "", -1);
	pango_layout_set_width(lo, (w - 2 * PAD) * PANGO_SCALE);
	pango_layout_set_wrap(lo, PANGO_WRAP_WORD_CHAR);
	cairo_set_source_rgb(cr, 0.96, 0.96, 0.96);
	cairo_move_to(cr, PAD, PAD);
	pango_cairo_show_layout(cr, lo);
	g_object_unref(lo);

	/* Answer buttons, full width, stacked. */
	for (int i = 0; i < n_btns; i++) {
		int by = btn_y[i];
		int bw = w - 2 * PAD;
		int hov = (i == hover_btn);
		pill_rect(cr, PAD, by, bw, BTN_H, 8);
		if (btn_is_yesno) {
			if (i == 0) { /* Yes: green */
				cairo_set_source_rgb(cr,
						     hov ? 0.18 : 0.13,
						     hov ? 0.40 : 0.32,
						     hov ? 0.21 : 0.16);
			} else { /* No: red */
				cairo_set_source_rgb(cr,
						     hov ? 0.42 : 0.34,
						     hov ? 0.19 : 0.14,
						     hov ? 0.19 : 0.14);
			}
		} else {
			cairo_set_source_rgb(cr, hov ? 0.33 : 0.28,
						 hov ? 0.28 : 0.23,
						 hov ? 0.14 : 0.10);
		}
		cairo_fill_preserve(cr);
		if (btn_is_yesno) {
			if (i == 0)
				cairo_set_source_rgb(cr, 0.30, 0.75, 0.38);
			else
				cairo_set_source_rgb(cr, 0.80, 0.32, 0.32);
		} else {
			cairo_set_source_rgb(cr, 0.85, 0.68, 0.25);
		}
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		PangoLayout *bl = pango_cairo_create_layout(cr);
		PangoFontDescription *bf =
			pango_font_description_from_string(GUI_FONT);
		pango_layout_set_font_description(bl, bf);
		pango_font_description_free(bf);
		/* Display label: Title Case for yes/no ("Yes"/"No"), verbatim
		 * (ellipsized) for custom answers. The FILE always gets the
		 * lowercase/verbatim btn_val. */
		if (btn_is_yesno)
			pango_layout_set_text(bl, i == 0 ? "Yes" : "No", -1);
		else
			pango_layout_set_text(bl, btn_val[i], -1);
		pango_layout_set_width(bl, (bw - 16) * PANGO_SCALE);
		pango_layout_set_ellipsize(bl, PANGO_ELLIPSIZE_END);
		int tw, th;
		pango_layout_get_pixel_size(bl, &tw, &th);
		(void)tw;
		if (btn_is_yesno && i == 0)
			cairo_set_source_rgb(cr, 0.78, 1.0, 0.80);
		else if (btn_is_yesno)
			cairo_set_source_rgb(cr, 1.0, 0.80, 0.78);
		else
			cairo_set_source_rgb(cr, 1.0, 0.88, 0.55);
		cairo_move_to(cr, PAD + 8, by + (BTN_H - th) / 2);
		pango_cairo_show_layout(cr, bl);
		g_object_unref(bl);
	}

	cairo_destroy(cr);
	cairo_surface_destroy(cs);

	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, w, h);
	wl_surface_commit(surf);
	wl_display_flush(dpy);
}

static int btn_at(double x, double y)
{
	if (x < PAD || x > DLG_W - PAD)
		return -1;
	for (int i = 0; i < n_btns; i++) {
		if (y >= btn_y[i] && y <= btn_y[i] + BTN_H)
			return i;
	}
	return -1;
}

/* ---------- wayland listeners ---------- */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *s,
			    uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	(void)s;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(s, serial);
	configured = 1;
	render_frame();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *s)
{
	(void)data;
	(void)s;
	_exit(0); /* compositor killed our dialog; nothing to save */
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void pointer_enter(void *d, struct wl_pointer *p, uint32_t s,
			  struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)s;
	(void)x;
	(void)y;
	pointer_on_dlg = (sf == surf);
}

static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s,
			  struct wl_surface *sf)
{
	(void)d;
	(void)p;
	(void)s;
	(void)sf;
	pointer_on_dlg = 0;
	pointer_x = -1;
	pointer_y = -1;
	if (hover_btn != -1) {
		hover_btn = -1;
		if (configured)
			render_frame();
	}
}

static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t,
			   wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)t;
	pointer_x = wl_fixed_to_double(x);
	pointer_y = wl_fixed_to_double(y);
	int b = btn_at(pointer_x, pointer_y);
	if (b != hover_btn) {
		hover_btn = b;
		if (configured)
			render_frame();
	}
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial,
			   uint32_t time, uint32_t button, uint32_t state)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED &&
	    pointer_on_dlg && pointer_x >= 0 && pointer_y >= 0) {
		int b = btn_at(pointer_x, pointer_y);
		if (b >= 0) {
			answer_current(btn_val[b]);
			advance();
		}
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

/* No-op tail of wl_pointer_listener: the compositor may send
 * frame/axis_source/axis_stop/axis_discrete; a NULL handler aborts. */
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
	(void)a;
	(void)n;
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

static void keyboard_keymap(void *d, struct wl_keyboard *k, uint32_t f, int fd,
			    uint32_t sz)
{
	(void)d;
	(void)k;
	(void)f;
	(void)fd;
	(void)sz;
}

static void keyboard_enter(void *d, struct wl_keyboard *k, uint32_t s,
			   struct wl_surface *sf, struct wl_array *keys)
{
	(void)d;
	(void)k;
	(void)s;
	(void)sf;
	(void)keys;
}

static void keyboard_leave(void *d, struct wl_keyboard *k, uint32_t s,
			   struct wl_surface *sf)
{
	(void)d;
	(void)k;
	(void)s;
	(void)sf;
}

static void keyboard_key(void *d, struct wl_keyboard *k, uint32_t s,
			 uint32_t t, uint32_t key, uint32_t st)
{
	(void)d;
	(void)k;
	(void)s;
	(void)t;
	/* WHY numeric compare, no xkbcommon: Esc is the only key we need and
	 * its code is fixed (wayland key = evdev + 8; KEY_ESC evdev = 1). */
	if (st == WL_KEYBOARD_KEY_STATE_PRESSED && key == KEY_ESC + 8) {
		dismiss_current();
		advance();
	}
}

static void keyboard_modifiers(void *d, struct wl_keyboard *k, uint32_t s,
			       uint32_t dep, uint32_t lat, uint32_t lock,
			       uint32_t grp)
{
	(void)d;
	(void)k;
	(void)s;
	(void)dep;
	(void)lat;
	(void)lock;
	(void)grp;
}

static void keyboard_repeat_info(void *d, struct wl_keyboard *k, int32_t r,
				 int32_t dly)
{
	(void)d;
	(void)k;
	(void)r;
	(void)dly;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = keyboard_enter,
	.leave = keyboard_leave,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
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

static gboolean wl_dispatch_cb(GIOChannel *src, GIOCondition cond,
			       gpointer unused)
{
	(void)src;
	(void)cond;
	(void)unused;
	while (wl_display_prepare_read(dpy) != 0)
		wl_display_dispatch_pending(dpy);
	if (wl_display_read_events(dpy) < 0)
		_exit(1); /* compositor gone */
	if (wl_display_dispatch_pending(dpy) < 0)
		_exit(1);
	wl_display_flush(dpy);
	return G_SOURCE_CONTINUE;
}


/* ---------- TASK-010 preview popup (notification-gui --preview) ----------
 * Tiny layer-shell list near the bar icon: up to 3 oldest plain rows
 * (first lines, FIFO) + at most 1 question row (oldest .q). Plain row
 * click unlinks that file (read) and refreshes; question row spawns the
 * answer deck above and closes this popup. Closes on selection, empty
 * queue, pointer-leave, or Esc. Same caps as the bar (160-char display,
 * first line, utf8, "(unreadable)"); unlinks confined to queue_dir; no
 * shell anywhere. Headless model via --preview --dry-run. */

#define PREV_W 380
#define PREV_PAD 10
#define PREV_ROW_H 34
#define PREV_GAP 6
#define PREV_MAX_PLAIN 3
#define PREV_TXT_PAD 4 /* top/bottom inset of wrapped text in an expanded row */
#define PREV_MAX_LINES 10 /* wrapped text never measures taller than 10 lines */
#define PREV_MAX_TOTAL_H 600 /* popup never taller; top-right anchor grows down */

static char prev_plain_id[PREV_MAX_PLAIN][256];
static char prev_plain_text[PREV_MAX_PLAIN][161];
static long prev_plain_mtime[PREV_MAX_PLAIN];
static long prev_plain_mtime_ns[PREV_MAX_PLAIN];
static long prev_plain_size[PREV_MAX_PLAIN];
static int prev_n_plain = 0;
static int prev_has_q = 0;
static char prev_q_id[256];
static char prev_q_text[161];
static long prev_q_mtime = 0;
static long prev_q_mtime_ns = 0;
static long prev_q_size = 0;
static int prev_n_rows = 0;
static int prev_row_y[PREV_MAX_PLAIN + 1];
/* TASK-012: per-row heights assigned by preview_layout. Compact rows are
 * PREV_ROW_H; the hovered row may be taller (full wrapped text). Kept
 * beside prev_row_y so preview_row_at hit-tests the CURRENT (possibly
 * expanded) geometry instead of assuming uniform rows. */
static int prev_row_h[PREV_MAX_PLAIN + 1];
static int prev_h = 0;
static int prev_hover = -1;
static double prev_px = -1, prev_py = -1;
static int prev_on = 0;

/* First-line, stripped, capped display text for one queue file. Reads at
 * most 4K (never the whole file): queue .txt files are untrusted in size
 * and a hostile multi-MB file must not spike a 1Hz tick. The deck's
 * load_oldest keeps its whole-file read on purpose — .q files are
 * writer-capped at ~512B by the `notification` CLI contract, so that read
 * is already bounded; only this preview path needs the cap. */
static void preview_cap_text(const char *path, char out[161])
{
	char tmp[4097];
	ssize_t n;
	int fd;
	out[0] = '\0';
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, tmp, 4096);
	close(fd);
	if (n <= 0)
		return;
	tmp[n] = '\0';
	{
		char *nl = memchr(tmp, '\n', (size_t)n);
		if (nl)
			*nl = '\0';
	}
	g_strstrip(tmp);
	if (!*tmp || !g_utf8_validate(tmp, -1, NULL)) {
		snprintf(out, 161, "(unreadable)");
		return;
	}
	if ((int)strlen(tmp) > 160) {
		/* WHY char-boundary cut: byte-cutting at 160 can split a
		 * multi-byte sequence, failing validation and degrading the
		 * whole row to "(unreadable)". Back off to the last valid
		 * boundary so long CJK/emoji first-lines keep their prefix. */
		size_t cut = 160;
		tmp[cut] = '\0';
		while (cut > 0 && !g_utf8_validate(tmp, -1, NULL)) {
			cut--;
			tmp[cut] = '\0';
		}
		if (!cut) {
			snprintf(out, 161, "(unreadable)");
			return;
		}
	}
	g_strlcpy(out, tmp, 161);
}

/* Sorted oldest-first basenames for one suffix (.txt or .q). */
static GPtrArray *preview_list(const char *sfx)
{
	GPtrArray *names = g_ptr_array_new();
	if (queue_dir) {
		GDir *d = g_dir_open(queue_dir, 0, NULL);
		if (d) {
			const char *e;
			size_t sl = strlen(sfx);
			while ((e = g_dir_read_name(d))) {
				size_t n = strlen(e);
				if (n > sl && !strcmp(e + n - sl, sfx))
					g_ptr_array_add(names,
							g_strdup(e));
			}
			g_dir_close(d);
		}
	}
	g_ptr_array_sort_values(names, (GCompareFunc)g_strcmp0);
	return names;
}

static void preview_free_list(GPtrArray *l)
{
	for (guint i = 0; i < l->len; i++)
		g_free(l->pdata[i]);
	g_ptr_array_free(l, FALSE);
}

/* Reload the model from disk. Returns total rows (0 = empty). Preserves
 * the hover highlight across reloads by row id (basename), so a tick
 * refresh never flickers the highlight under a steady cursor. Rows whose
 * files vanish mid-scan are skipped (stat gate), never rendered empty. */
static int preview_reload(void)
{
	GPtrArray *plains, *qs;
	char hover_id[256] = "";
	if (prev_hover >= 0 && prev_hover < prev_n_rows) {
		if (prev_has_q && prev_hover == prev_n_rows - 1)
			g_strlcpy(hover_id, prev_q_id, sizeof hover_id);
		else if (prev_hover < prev_n_plain)
			g_strlcpy(hover_id, prev_plain_id[prev_hover],
				  sizeof hover_id);
	}
	prev_n_plain = 0;
	prev_has_q = 0;
	prev_n_rows = 0;
	prev_plain_id[0][0] = '\0';
	prev_q_id[0] = '\0';
	prev_plain_text[0][0] = '\0';
	prev_q_text[0] = '\0';
	plains = preview_list(".txt");
	qs = preview_list(".q");
	for (guint i = 0; i < plains->len && prev_n_plain < PREV_MAX_PLAIN;
	     i++) {
		const char *nm = plains->pdata[i];
		char *p = g_build_filename(queue_dir, nm, NULL);
		struct stat st;
		if (g_strlcpy(prev_plain_id[prev_n_plain], nm, 256) < 256 &&
		    stat(p, &st) == 0) {
			preview_cap_text(p,
					 prev_plain_text[prev_n_plain]);
			prev_plain_mtime[prev_n_plain] = (long)st.st_mtime;
			prev_plain_mtime_ns[prev_n_plain] =
				(long)st.st_mtim.tv_nsec;
			prev_plain_size[prev_n_plain] = (long)st.st_size;
			prev_n_plain++;
		}
		g_free(p);
	}
	if (qs->len) {
		const char *nm = qs->pdata[0];
		char *p = g_build_filename(queue_dir, nm, NULL);
		struct stat st;
		if (g_strlcpy(prev_q_id, nm, 256) < 256 &&
		    stat(p, &st) == 0) {
			preview_cap_text(p, prev_q_text);
			prev_q_mtime = (long)st.st_mtime;
			prev_q_mtime_ns = (long)st.st_mtim.tv_nsec;
			prev_q_size = (long)st.st_size;
			prev_has_q = 1;
		}
		g_free(p);
	}
	preview_free_list(plains);
	preview_free_list(qs);
	prev_n_rows = prev_n_plain + (prev_has_q ? 1 : 0);
	/* Compact fallback geometry; preview_layout refines it (expanding the
	 * hovered row) right after every reload, and owns prev_h. */
	for (int i = 0; i < prev_n_rows; i++)
		prev_row_h[i] = PREV_ROW_H;
	prev_h = PREV_PAD * 2 + prev_n_rows * PREV_ROW_H +
		(prev_n_rows > 0 ? (prev_n_rows - 1) * PREV_GAP : 0);
	prev_hover = -1;
	if (hover_id[0]) {
		for (int i = 0; i < prev_n_plain; i++) {
			if (!strcmp(prev_plain_id[i], hover_id)) {
				prev_hover = i;
				break;
			}
		}
		if (prev_hover < 0 && prev_has_q &&
		    !strcmp(prev_q_id, hover_id))
			prev_hover = prev_n_rows - 1;
	}
	return prev_n_rows;
}

static int preview_dry_run(void)
{
	int n = preview_reload();
	if (!n) {
		printf("preview: queue empty\n");
		return 0;
	}
	printf("preview: total %d (plain %d + question %d)\n", n,
	       prev_n_plain, prev_has_q ? 1 : 0);
	for (int i = 0; i < prev_n_plain; i++)
		printf("plain %d %s %s\n", i + 1, prev_plain_id[i],
		       prev_plain_text[i]);
	if (prev_has_q)
		printf("question %s %s\n", prev_q_id, prev_q_text);
	return 0;
}

static int preview_row_at(double x, double y)
{
	int i;
	if (x < PREV_PAD || x > PREV_W - PREV_PAD)
		return -1;
	for (i = 0; i < prev_n_rows; i++) {
		/* TASK-012: hit-test the CURRENT per-row heights (the hovered
		 * row may be expanded), not a uniform PREV_ROW_H. */
		if (y >= prev_row_y[i] &&
		    y <= prev_row_y[i] + prev_row_h[i])
			return i;
	}
	return -1;
}

/* Unlink one basename confined to queue_dir. Plain rows only; the caller
 * guarantees no '/' and the .txt suffix. */
static void preview_dismiss_plain(const char *id)
{
	char *p;
	if (!id || !*id || strchr(id, '/'))
		return;
	if (strlen(id) <= 4 || strcmp(id + strlen(id) - 4, ".txt"))
		return;
	p = g_build_filename(queue_dir, id, NULL);
	/* WHY ENOENT is silent: another popup instance, a manual delete, or
	 * the tick's empty-queue path may have taken the file first — that
	 * is the dismissal we wanted anyway, not an error. */
	if (unlink(p) != 0 && errno != ENOENT)
		fprintf(stderr, "notification-gui: preview dismiss %s failed\n",
			id);
	g_free(p);
}

static void spawn_answer_deck(void)
{
	char self[4096];
	ssize_t n;
	char *slash, *helper;
	char *argv[] = { NULL, NULL };
	GError *err = NULL;
	/* WHY same guard idiom as the panel: a question click while the
	 * deck is already open must not stack a second one. -x anchors the
	 * full cmdline, so this preview (which carries --preview) never
	 * matches itself.
	 * WHY the residual TOCTOU is accepted here: guard + spawn are not
	 * atomic, so two spawns in the same instant could fork twice.
	 * Benign: both decks show the same oldest question and answer-first
	 * wins — the loser finds the .q gone and skips the write instead of
	 * littering a stale .answer, and Esc-dismiss is idempotent. The GUI
	 * path cannot double-click us anyway (single-instance mkdir lock in
	 * preview_main: only one preview exists, and its first question
	 * click exits it). */
	char *pargv[] = { "pgrep", "-f", "-x",
			  ".*/notification-gui", NULL };
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
		g_clear_error(&perr);
	}
	n = readlink("/proc/self/exe", self, sizeof self - 1);
	if (n <= 0)
		return;
	self[n] = '\0';
	slash = strrchr(self, '/');
	if (!slash)
		return;
	*slash = '\0';
	helper = g_strconcat(self, "/notification-gui", NULL);
	argv[0] = helper;
	if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
			   NULL, NULL, &err)) {
		fprintf(stderr, "notification-gui: preview: spawn deck failed: %s\n",
			err ? err->message : "unknown");
		g_clear_error(&err);
	}
	g_free(helper);
}

/* TASK-012 hover-to-expand: build the EXACT display string preview_render
 * draws for row i ("✉ %s" plain, "? %s  → answer" question), from the
 * already-capped 160-char validated text. Render calls this too, so the
 * measured string and the drawn string can never drift apart. */
static void preview_row_line(int i, char *out, size_t n)
{
	int is_q = (prev_has_q && i == prev_n_rows - 1);
	if (is_q)
		snprintf(out, n, "? %s  → answer", prev_q_text);
	else
		snprintf(out, n, "\xe2\x9c\x89 %s", prev_plain_text[i]);
}

/* Pixel height of `text` in the live GUI_FONT at the live row-text width
 * (bw-16). wrap=1: WORD_CHAR wrap, no ellipsis (= expanded measure);
 * wrap=0: single line (= line-height unit). Scratch image surface, no
 * Wayland — safe on tick/motion paths. Measured only on hover change /
 * reload, never per-frame, so the 1Hz tick budget is untouched. */
static int preview_text_h(const char *text, int wrap)
{
	cairo_surface_t *cs =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 8, 8);
	cairo_t *cr = cairo_create(cs);
	PangoLayout *lo = pango_cairo_create_layout(cr);
	PangoFontDescription *fd =
		pango_font_description_from_string(GUI_FONT);
	int w, h;
	pango_layout_set_font_description(lo, fd);
	pango_font_description_free(fd);
	pango_layout_set_text(lo, text, -1);
	pango_layout_set_width(lo, (PREV_W - 2 * PREV_PAD - 16) *
			       PANGO_SCALE);
	if (wrap)
		pango_layout_set_wrap(lo, PANGO_WRAP_WORD_CHAR);
	pango_layout_get_pixel_size(lo, &w, &h);
	(void)w;
	g_object_unref(lo);
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	return h;
}

/* Expanded height for the hovered row: full wrapped text + padding. Short
 * rows collapse back to PREV_ROW_H, so hovering them changes nothing and
 * rows below never shift for text that already fits on one line. */
static int preview_expanded_h(int i)
{
	char line[512];
	int wrap, cap, h;
	preview_row_line(i, line, sizeof line);
	wrap = preview_text_h(line, 1);
	/* WHY lines, not pixels: the cap must fit the worst case the queue
	 * contract allows (160-char capped text + "? … → answer" suffix) at
	 * PREV_W in the live font. The NOTIFGUI_TEST harness measures 8 lines for all-wide 160-char input (harness worst case)
	 * covers all-wide input with headroom; pixels derive from the measured
	 * line height, never a magic number. */
	cap = preview_text_h("Ag", 0) * PREV_MAX_LINES;
	if (wrap > cap)
		wrap = cap;
	h = wrap + 2 * PREV_TXT_PAD;
	if (h < PREV_ROW_H)
		h = PREV_ROW_H;
	return h;
}

static void preview_layout(void)
{
	int y = PREV_PAD;
	if (prev_n_rows == 0) {
		prev_h = PREV_PAD * 2;
		return;
	}
	for (int i = 0; i < prev_n_rows; i++) {
		/* Only the hovered row grows; the rest stay single-line
		 * ellipsis exactly as before. */
		int h = (i == prev_hover) ? preview_expanded_h(i) :
					    PREV_ROW_H;
		prev_row_h[i] = h;
		prev_row_y[i] = y;
		y += h + PREV_GAP;
	}
	prev_h = y - PREV_GAP + PREV_PAD;
	/* Cap the total so the popup can't run off-screen (only the
	 * expanded row can push it over: compact max is 174px). The anchor
	 * stays top-right, so growth is always downward. Never shrink below
	 * compact height — worst case the row shows its first lines. */
	if (prev_h > PREV_MAX_TOTAL_H && prev_hover >= 0 &&
	    prev_hover < prev_n_rows) {
		int rest = prev_h - prev_row_h[prev_hover];
		int max_exp = PREV_MAX_TOTAL_H - rest;
		if (max_exp < PREV_ROW_H)
			max_exp = PREV_ROW_H;
		if (prev_row_h[prev_hover] > max_exp) {
			prev_row_h[prev_hover] = max_exp;
			y = PREV_PAD;
			for (int i = 0; i < prev_n_rows; i++) {
				prev_row_y[i] = y;
				y += prev_row_h[i] + PREV_GAP;
			}
			prev_h = y - PREV_GAP + PREV_PAD;
		}
	}
}

static void preview_render(void);

static void preview_refresh_or_close(void)
{
	if (!preview_reload())
		exit(0);
	preview_layout();
	if (layer_surf)
		zwlr_layer_surface_v1_set_size(layer_surf, PREV_W, prev_h);
	if (surf)
		wl_surface_commit(surf);
	preview_render();
}

static void preview_render(void)
{
	int w = PREV_W, h = prev_h;
	cairo_surface_t *cs;
	cairo_t *cr;
	if (h < 32 || !shm || !surf)
		return;
	if (w != pool_w || h != pool_h) {
		if (shm_pool)
			wl_shm_pool_destroy(shm_pool);
		if (shm_fd >= 0)
			close(shm_fd);
		if (shm_data)
			munmap(shm_data, shm_size);
		shm_size = (size_t)w * h * 4;
		shm_fd = memfd_create("notif-preview", MFD_CLOEXEC);
		if (shm_fd < 0 || ftruncate(shm_fd, shm_size) < 0)
			return;
		shm_data = mmap(NULL, shm_size, PROT_READ | PROT_WRITE,
				 MAP_SHARED, shm_fd, 0);
		if (shm_data == MAP_FAILED) {
			close(shm_fd);
			shm_fd = -1;
			return;
		}
		shm_pool = wl_shm_create_pool(shm, shm_fd, shm_size);
		pool_w = w;
		pool_h = h;
	}
	if (buf)
		wl_buffer_destroy(buf);
	buf = wl_shm_pool_create_buffer(shm_pool, 0, w, h, w * 4,
					WL_SHM_FORMAT_ARGB8888);
	cs = cairo_image_surface_create_for_data(shm_data,
						 CAIRO_FORMAT_ARGB32, w, h,
						 w * 4);
	cr = cairo_create(cs);
	pill_rect(cr, 0.5, 0.5, w - 1, h - 1, 10);
	cairo_set_source_rgb(cr, 0.10, 0.10, 0.11);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.85, 0.68, 0.25);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);
	for (int i = 0; i < prev_n_rows; i++) {
		int by = prev_row_y[i];
		int bw = w - 2 * PREV_PAD;
		int rh = prev_row_h[i];
		int hov = (i == prev_hover);
		int is_q = (prev_has_q && i == prev_n_rows - 1);
		char line[512];
		PangoLayout *bl;
		PangoFontDescription *bf;
		int tw, th;
		preview_row_line(i, line, sizeof line);
		pill_rect(cr, PREV_PAD, by, bw, rh, 7);
		if (is_q) {
			cairo_set_source_rgb(cr, hov ? 0.33 : 0.28,
						 hov ? 0.28 : 0.23,
						 hov ? 0.14 : 0.10);
		} else {
			cairo_set_source_rgb(cr, hov ? 0.22 : 0.17,
						 hov ? 0.20 : 0.16,
						 hov ? 0.12 : 0.10);
		}
		cairo_fill_preserve(cr);
		cairo_set_source_rgb(cr, 0.85, 0.68, 0.25);
		cairo_set_line_width(cr, 1);
		cairo_stroke(cr);
		bl = pango_cairo_create_layout(cr);
		bf = pango_font_description_from_string(GUI_FONT);
		pango_layout_set_font_description(bl, bf);
		pango_font_description_free(bf);
		pango_layout_set_text(bl, line, -1);
		pango_layout_set_width(bl, (bw - 16) * PANGO_SCALE);
		if (hov && rh > PREV_ROW_H) {
			/* TASK-012: the hovered (grown) row shows its FULL
			 * text — wrap, no ellipsis, top-aligned. Clip to the
			 * pill so a pathological font can never paint past
			 * the rounded rect. Non-hovered and short-hovered
			 * rows keep the single-line ellipsis path below. */
			pango_layout_set_wrap(bl, PANGO_WRAP_WORD_CHAR);
			cairo_save(cr);
			cairo_rectangle(cr, PREV_PAD + 1, by + 1, bw - 2,
					rh - 2);
			cairo_clip(cr);
			cairo_set_source_rgb(cr, 1.0, 0.88, 0.55);
			cairo_move_to(cr, PREV_PAD + 8,
				      by + PREV_TXT_PAD);
			pango_cairo_show_layout(cr, bl);
			cairo_restore(cr);
		} else {
			pango_layout_set_ellipsize(bl,
						   PANGO_ELLIPSIZE_END);
			pango_layout_get_pixel_size(bl, &tw, &th);
			(void)tw;
			cairo_set_source_rgb(cr, 1.0, 0.88, 0.55);
			cairo_move_to(cr, PREV_PAD + 8,
				      by + (PREV_ROW_H - th) / 2);
			pango_cairo_show_layout(cr, bl);
		}
		g_object_unref(bl);
	}
	cairo_destroy(cr);
	cairo_surface_destroy(cs);
	wl_surface_attach(surf, buf, 0, 0);
	wl_surface_damage_buffer(surf, 0, 0, w, h);
	wl_surface_commit(surf);
	wl_display_flush(dpy);
}

static void preview_enter(void *d, struct wl_pointer *p, uint32_t s,
			  struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{
	(void)d;
	(void)p;
	(void)s;
	prev_on = (sf == surf);
	prev_px = wl_fixed_to_double(x);
	prev_py = wl_fixed_to_double(y);
}

static void preview_leave(void *d, struct wl_pointer *p, uint32_t s,
			  struct wl_surface *sf)
{
	(void)d;
	(void)p;
	(void)s;
	(void)sf;
	/* Pointer left the popup: close (no selection). */
	exit(0);
}

static void preview_motion(void *d, struct wl_pointer *p, uint32_t t,
			   wl_fixed_t x, wl_fixed_t y)
{
	int b;
	(void)d;
	(void)p;
	(void)t;
	prev_px = wl_fixed_to_double(x);
	prev_py = wl_fixed_to_double(y);
	b = preview_row_at(prev_px, prev_py);
	if (b != prev_hover) {
		/* TASK-012: hover CHANGED, so the geometry changed (the new
		 * row grows, the old one collapses, rows below shift). Take
		 * the full refresh path — reload (preserves hover by row
		 * id) + layout + resize + commit + render — not render-only,
		 * or rows would paint at stale offsets. Steady hover takes
		 * no path at all (no per-motion work); the tick's dirty
		 * check skips unchanged queues the same way. */
		prev_hover = b;
		if (configured)
			preview_refresh_or_close();
	}
}

static void preview_click(void *data, struct wl_pointer *p, uint32_t serial,
			  uint32_t time, uint32_t button, uint32_t state)
{
	int b;
	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	if (button != BTN_LEFT ||
	    state != WL_POINTER_BUTTON_STATE_PRESSED || !prev_on)
		return;
	if (prev_px < 0 || prev_py < 0)
		return;
	b = preview_row_at(prev_px, prev_py);
	if (b < 0)
		return;
	if (prev_has_q && b == prev_n_rows - 1) {
		spawn_answer_deck();
		exit(0);
	}
	if (b < prev_n_plain)
		preview_dismiss_plain(prev_plain_id[b]);
	preview_refresh_or_close();
}

static void preview_axis(void *d, struct wl_pointer *p, uint32_t t,
			 uint32_t a, wl_fixed_t v)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
	(void)v;
}

static void preview_frame(void *d, struct wl_pointer *p)
{
	(void)d;
	(void)p;
}

static void preview_axis_src(void *d, struct wl_pointer *p, uint32_t s)
{
	(void)d;
	(void)p;
	(void)s;
}

static void preview_axis_stop(void *d, struct wl_pointer *p, uint32_t t,
			      uint32_t a)
{
	(void)d;
	(void)p;
	(void)t;
	(void)a;
}

static void preview_axis_disc(void *d, struct wl_pointer *p, uint32_t a,
			      int32_t n)
{
	(void)d;
	(void)p;
	(void)a;
	(void)n;
}

static const struct wl_pointer_listener preview_pointer_listener = {
	.enter = preview_enter,
	.leave = preview_leave,
	.motion = preview_motion,
	.button = preview_click,
	.axis = preview_axis,
	.frame = preview_frame,
	.axis_source = preview_axis_src,
	.axis_stop = preview_axis_stop,
	.axis_discrete = preview_axis_disc,
};

static void preview_key(void *d, struct wl_keyboard *k, uint32_t s,
			uint32_t t, uint32_t key, uint32_t st)
{
	(void)d;
	(void)k;
	(void)s;
	(void)t;
	if (st == WL_KEYBOARD_KEY_STATE_PRESSED && key == KEY_ESC + 8)
		exit(0);
}

static void preview_keymap(void *d, struct wl_keyboard *k, uint32_t f,
			   int fd, uint32_t sz)
{
	(void)d;
	(void)k;
	(void)f;
	(void)fd;
	(void)sz;
}

static void preview_kenter(void *d, struct wl_keyboard *k, uint32_t s,
			   struct wl_surface *sf, struct wl_array *keys)
{
	(void)d;
	(void)k;
	(void)s;
	(void)sf;
	(void)keys;
}

static void preview_kleave(void *d, struct wl_keyboard *k, uint32_t s,
			   struct wl_surface *sf)
{
	(void)d;
	(void)k;
	(void)s;
	(void)sf;
}

static void preview_mods(void *d, struct wl_keyboard *k, uint32_t s,
			 uint32_t dep, uint32_t lat, uint32_t lock,
			 uint32_t grp)
{
	(void)d;
	(void)k;
	(void)s;
	(void)dep;
	(void)lat;
	(void)lock;
	(void)grp;
}

static void preview_repeat(void *d, struct wl_keyboard *k, int32_t r,
			   int32_t dly)
{
	(void)d;
	(void)k;
	(void)r;
	(void)dly;
}

static const struct wl_keyboard_listener preview_keyboard_listener = {
	.keymap = preview_keymap,
	.enter = preview_kenter,
	.leave = preview_kleave,
	.key = preview_key,
	.modifiers = preview_mods,
	.repeat_info = preview_repeat,
};

static void preview_seat_caps(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &preview_pointer_listener,
					NULL);
	}
	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &preview_keyboard_listener,
					 NULL);
	}
}

static void preview_seat_name(void *d, struct wl_seat *s, const char *n)
{
	(void)d;
	(void)s;
	(void)n;
}

static const struct wl_seat_listener preview_seat_listener = {
	preview_seat_caps, preview_seat_name
};

static void preview_configure(void *data, struct zwlr_layer_surface_v1 *s,
			      uint32_t serial, uint32_t w, uint32_t h)
{
	(void)data;
	(void)s;
	(void)w;
	(void)h;
	zwlr_layer_surface_v1_ack_configure(s, serial);
	configured = 1;
	preview_render();
}

static void preview_closed(void *data, struct zwlr_layer_surface_v1 *s)
{
	(void)data;
	(void)s;
	_exit(0);
}

static const struct zwlr_layer_surface_v1_listener preview_layer_listener = {
	.configure = preview_configure,
	.closed = preview_closed,
};

/* True when the on-disk queue matches the live model (same visible
 * basenames with the same mtime+size): the tick can skip the reload +
 * re-render entirely, so the hover highlight never flickers under a
 * steady cursor. Rows past the PREV_MAX_PLAIN cap are invisible, so an
 * arrival beyond the cap correctly counts as unchanged. */
static int preview_model_current(GPtrArray *plains, GPtrArray *qs)
{
	guint want_plain = plains->len;
	int want_q = (qs->len != 0);
	if (want_plain > (guint)PREV_MAX_PLAIN)
		want_plain = (guint)PREV_MAX_PLAIN;
	if ((int)want_plain != prev_n_plain || want_q != prev_has_q)
		return 0;
	for (guint i = 0; i < want_plain; i++) {
		const char *nm = plains->pdata[i];
		char *p;
		struct stat st;
		if (strcmp(prev_plain_id[i], nm))
			return 0;
		p = g_build_filename(queue_dir, nm, NULL);
		if (stat(p, &st) != 0 ||
		    (long)st.st_mtime != prev_plain_mtime[i] ||
		    (long)st.st_mtim.tv_nsec != prev_plain_mtime_ns[i] ||
		    (long)st.st_size != prev_plain_size[i]) {
			g_free(p);
			return 0;
		}
		g_free(p);
	}
	if (want_q) {
		const char *nm = qs->pdata[0];
		char *p;
		struct stat st;
		if (strcmp(prev_q_id, nm))
			return 0;
		p = g_build_filename(queue_dir, nm, NULL);
		if (stat(p, &st) != 0 ||
		    (long)st.st_mtime != prev_q_mtime ||
		    (long)st.st_mtim.tv_nsec != prev_q_mtime_ns ||
		    (long)st.st_size != prev_q_size) {
			g_free(p);
			return 0;
		}
		g_free(p);
	}
	return 1;
}

static gboolean preview_tick(gpointer unused)
{
	GPtrArray *pl, *ql;
	(void)unused;
	pl = preview_list(".txt");
	ql = preview_list(".q");
	if (pl->len == 0 && ql->len == 0) {
		preview_free_list(pl);
		preview_free_list(ql);
		exit(0);
	}
	/* Unchanged queue (ids+mtimes match): skip reload + re-render, so a
	 * steady hover never flickers. preview_reload preserves prev_hover
	 * by row id on the dirty path. */
	if (preview_model_current(pl, ql)) {
		preview_free_list(pl);
		preview_free_list(ql);
		return G_SOURCE_CONTINUE;
	}
	preview_free_list(pl);
	preview_free_list(ql);
	/* Queue changed under the open popup: reload so rows never go stale.
	 * Cheap: two readdirs + at most 4 first-line reads per second. */
	preview_refresh_or_close();
	return G_SOURCE_CONTINUE;
}

static void preview_reg_global(void *data, struct wl_registry *r,
			       uint32_t name, const char *iface, uint32_t ver)
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
		wl_seat_add_listener(seat, &preview_seat_listener, NULL);
	}
}

static void preview_reg_remove(void *d, struct wl_registry *r, uint32_t n)
{
	(void)d;
	(void)r;
	(void)n;
}

static const struct wl_registry_listener preview_reg_listener = {
	preview_reg_global, preview_reg_remove
};

/* Single-instance lock for the preview popup (TASK-010 icon
 * double-click TOCTOU): the panel's pgrep guard + g_spawn_async are not
 * atomic, so two rapid icon clicks can both pass the guard and fork. The
 * mkdir below is the real serialization — atomic in the kernel, exactly
 * one winner. The loser sees EEXIST, confirms a live holder via the pid
 * file (kill + /proc cmdline, never a pgrep that would match itself),
 * and exits 0 quietly: a second icon click while open is a no-op. The
 * winner writes its pid and releases (unlink + rmdir) at exit via atexit.
 * A stale dir (crash, or a _exit() path that skips atexit) is reclaimed
 * on the next start: dead pid or unrelated cmdline means nobody holds
 * it, so we unlink + rmdir and retry once. */
static char *preview_lock_dir(void)
{
	return g_build_filename(queue_dir, ".preview-lock", NULL);
}

static int preview_lock_holder_live(const char *lock)
{
	char *pidp = g_build_filename(lock, "pid", NULL);
	char *pidtxt = NULL;
	int live = 0;
	if (g_file_get_contents(pidp, &pidtxt, NULL, NULL) && pidtxt) {
		long pid = atol(pidtxt);
		if (pid > 1 && pid != (long)getpid() &&
		    kill((pid_t)pid, 0) == 0) {
			char *cp =
				g_strdup_printf("/proc/%ld/cmdline", pid);
			char *cmd = NULL;
			if (g_file_get_contents(cp, &cmd, NULL, NULL) &&
			    cmd)
				live = (strstr(cmd,
						"notification-gui") != NULL);
			else
				live = 1; /* unreadable cmdline: assume live */
			g_free(cmd);
			g_free(cp);
		}
		g_free(pidtxt);
	}
	g_free(pidp);
	return live;
}

/* Saved lock path for release. WHY saved, not rebuilt from queue_dir:
 * main() frees queue_dir before atexit handlers run, so resolving the
 * path at release time would read freed memory. Release is idempotent. */
static char *preview_lock_saved = NULL;

static void preview_lock_release(void)
{
	if (preview_lock_saved) {
		char *pidp =
			g_build_filename(preview_lock_saved, "pid", NULL);
		unlink(pidp);
		rmdir(preview_lock_saved);
		g_free(pidp);
		g_free(preview_lock_saved);
		preview_lock_saved = NULL;
	}
}

/* Returns 1 when this process now holds the lock (pid file written,
 * atexit release armed). Returns 0 when another live preview holds it.
 * No-pid-file + fresh dir = the winner is mid-startup (pid write follows
 * mkdir by µs): back off quietly. No-pid-file + ancient dir = the winner
 * died before writing: reclaim (bounds the stuck window to ~10s). */
static int preview_lock_acquire(void)
{
	char *lock = preview_lock_dir();
	char *pidp = g_build_filename(lock, "pid", NULL);
	char *pidtxt = NULL;
	int e = 0;
	int held = 0;
	if (mkdir(lock, 0700) == 0) {
		held = 1;
	} else {
		e = errno;
		if (e == EEXIST &&
		    g_file_get_contents(pidp, &pidtxt, NULL, NULL) &&
		    pidtxt) {
			/* Holder wrote its pid: reclaim only when dead. */
			if (!preview_lock_holder_live(lock)) {
				unlink(pidp);
				rmdir(lock);
				if (mkdir(lock, 0700) == 0)
					held = 1;
			}
			g_free(pidtxt);
		} else if (e == EEXIST) {
			struct stat st;
			if (stat(lock, &st) == 0 &&
			    time(NULL) - (long)st.st_mtime > 10) {
				rmdir(lock);
				if (mkdir(lock, 0700) == 0)
					held = 1;
			}
		}
	}
	if (held) {
		char *txt = g_strdup_printf("%ld\n", (long)getpid());
		/* Best effort: a failed write only weakens stale detection;
		 * the dir itself is the mutex. */
		g_file_set_contents(pidp, txt, -1, NULL);
		g_free(txt);
		g_free(preview_lock_saved);
		preview_lock_saved = g_strdup(lock);
		atexit(preview_lock_release);
	}
	g_free(pidp);
	g_free(lock);
	return held;
}

static int preview_main(void)
{
	/* Single-instance: a second icon click while open exits quietly. */
	if (!preview_lock_acquire())
		return 0;
	if (!preview_reload())
		return 0;
	preview_layout();
	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "notification-gui: no Wayland display\n");
		return 1;
	}
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &preview_reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !layer_shell) {
		fprintf(stderr,
			"notification-gui: compositor lacks layer-shell\n");
		return 1;
	}
	surf = wl_compositor_create_surface(comp);
	layer_surf = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surf, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "notification-preview");
	/* WHY top-right under the bar: the icon that spawned us lives there;
	 * margin-top clears the 28px bar, margin-right matches the clock pad.
	 * Exclusive zone 0 reserves nothing; EXCLUSIVE keyboard for Esc. */
	zwlr_layer_surface_v1_set_anchor(layer_surf,
					 ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
					 ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_margin(layer_surf, 34, 8, 0, 0);
	zwlr_layer_surface_v1_set_size(layer_surf, PREV_W, prev_h);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surf, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		layer_surf,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(layer_surf, &preview_layer_listener,
					   NULL);
	wl_surface_commit(surf);
	wl_display_flush(dpy);
	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GIOChannel *ch = g_io_channel_unix_new(wl_display_get_fd(dpy));
	g_io_add_watch(ch, G_IO_IN, wl_dispatch_cb, NULL);
	g_io_channel_unref(ch);
	g_timeout_add_seconds(1, preview_tick, NULL);
	g_main_loop_run(loop);
	return 0;
}

static void print_help(void)
{
	printf("usage: notification-gui [--help | --dry-run | --preview [--dry-run]]\n"
	       "\n"
	       "Centered layer-shell answer deck for question notifications.\n"
	       "Shows the oldest *.q in $XDG_DATA_HOME/monowl/notifications\n"
	       "with one button per answer (Yes/No when the question carries\n"
	       "no answers), oldest-first, auto-closing when the queue drains.\n"
	       "Click an answer to write <base>.answer; Esc dismisses without\n"
	       "answering. Survives Super+D (layer-shell, not a client).\n"
	       "\n"
	       "Tiny preview popup (TASK-010, spawned by the bar icon): up to 3\n"
	       "oldest plain rows + 1 question row. Plain click dismisses that\n"
	       "file; question click opens this deck; closes on selection,\n"
	       "empty queue, pointer-leave, or Esc. Hovering a row expands it to its\n"
       "full wrapped text (popup grows downward). Single open at a time: a\n"
       "second icon click while open exits quietly (mkdir lock).\n"
	       "\n"
	       "  --help      show this help (no display needed)\n"
	       "  --dry-run   list queued questions+answers (no display needed)\n"
	       "  --preview   open the tiny preview popup (empty = exit 0)\n"
	       "  --preview --dry-run  print preview rows (no display needed)\n");
}

static int dry_run(void)
{
	GPtrArray *names = queue_qs();
	if (!names->len)
		printf("notification-gui: queue empty\n");
	for (guint i = 0; i < names->len; i++) {
		char *p = g_build_filename(queue_dir, names->pdata[i], NULL);
		char *data = NULL;
		if (g_file_get_contents(p, &data, NULL, NULL) && data) {
			char **lines = g_strsplit(data, "\n", -1);
			printf("Q: %s\n",
			       lines[0] && *lines[0] ? lines[0]
						     : "(empty question)");
			int n = 0;
			for (int j = 1; lines[j]; j++) {
				g_strstrip(lines[j]);
				if (!*lines[j])
					continue;
				printf("  %d. %s\n", ++n, lines[j]);
			}
			if (!n)
				printf("  Yes/No\n");
			g_strfreev(lines);
			g_free(data);
		}
		g_free(p);
	}
	for (guint i = 0; i < names->len; i++)
		g_free(names->pdata[i]);
	g_ptr_array_free(names, FALSE);
	return 0;
}

#ifdef NOTIFGUI_TEST
/* TASK-012 headless harness: proves the hover-to-expand geometry without a
 * display (temp queue dir + scratch image surfaces, same font/width/wrap as
 * the live render). Covers: compact layout exactness, expanded-height math
 * (full wrapped text fits, line-based cap respected, total capped),
 * hit-testing against expanded geometry, unhover restoration, short-row
 * invariance, hover preserved by row id across reloads, and the tick
 * dirty-check (unchanged queue = current). Needs glib + cairo only. */
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
	char *tmp, *a, *b, *c, *q, *junk;
	char longtxt[201], longq[151], worst[161];
	GPtrArray *pl, *ql;
	char line[512];
	int line_h, wrap_long, wrap_short, wrap_worst, cap;
	int cy[PREV_MAX_PLAIN + 1];
	(void)argc;
	(void)argv;
	tmp = g_dir_make_tmp("notifgui-test-XXXXXX", NULL);
	TCHECK(tmp != NULL, "mktemp dir");
	if (!tmp)
		return 1;
	g_free(queue_dir);
	queue_dir = g_strdup(tmp);
	/* FIFO order: a=short, b=long (200 chars -> capped 160), c=short. */
	memset(longtxt, 'W', 200);
	longtxt[200] = '\0';
	memset(longq, 'm', 150);
	longq[150] = '\0';
	memset(worst, 'W', 160);
	worst[160] = '\0';
	a = g_build_filename(tmp, "a.txt", NULL);
	b = g_build_filename(tmp, "b.txt", NULL);
	c = g_build_filename(tmp, "c.txt", NULL);
	q = g_build_filename(tmp, "q1.q", NULL);
	junk = g_build_filename(tmp, "z.tmp", NULL);
	TCHECK(g_file_set_contents(a, "hi", -1, NULL), "write a.txt");
	TCHECK(g_file_set_contents(b, longtxt, -1, NULL), "write b.txt");
	TCHECK(g_file_set_contents(c, "ok", -1, NULL), "write c.txt");
	TCHECK(g_file_set_contents(q, longq, -1, NULL), "write q1.q");
	TCHECK(g_file_set_contents(junk, "junk", -1, NULL), "write junk");
	TCHECK(preview_reload() == 4, "3 plain + 1 question");
	TCHECK(prev_n_plain == 3 && prev_has_q == 1, "split counts");
	TCHECK(!strcmp(prev_plain_id[0], "a.txt") &&
	       !strcmp(prev_plain_id[1], "b.txt") &&
	       !strcmp(prev_plain_id[2], "c.txt"),
	       "plain FIFO oldest-first");
	TCHECK(strlen(prev_plain_text[1]) == 160, "long text capped at 160");
	/* Row strings match the render contract exactly. */
	preview_row_line(0, line, sizeof line);
	TCHECK(!strcmp(line, "\xe2\x9c\x89 hi"), "plain row exact");
	preview_row_line(3, line, sizeof line);
	TCHECK(!strncmp(line, "? ", 2) &&
	       strstr(line, "  → answer") != NULL,
	       "question row suffix");
	/* Compact layout is exactly the old uniform formula. */
	prev_hover = -1;
	preview_layout();
	for (int i = 0; i < 4; i++)
		cy[i] = PREV_PAD + i * (PREV_ROW_H + PREV_GAP);
	for (int i = 0; i < 4; i++)
		TCHECK(prev_row_y[i] == cy[i] &&
		       prev_row_h[i] == PREV_ROW_H,
		       "compact row geometry uniform");
	TCHECK(prev_h == PREV_PAD * 2 + 4 * PREV_ROW_H + 3 * PREV_GAP,
	       "compact total 174");
	/* Compact hit-testing: rows, gaps, outside pad. */
	TCHECK(preview_row_at(190, 15) == 0, "hit row 0");
	TCHECK(preview_row_at(190, 135) == 3, "hit row 3");
	TCHECK(preview_row_at(190, 46) == -1, "gap is no row");
	TCHECK(preview_row_at(5, 15) == -1, "outside pad is no row");
	/* Measure units: long text wraps multi-line, short stays one. */
	line_h = preview_text_h("Ag", 0);
	wrap_short = preview_text_h("\xe2\x9c\x89 hi", 1);
	preview_row_line(1, line, sizeof line);
	wrap_long = preview_text_h(line, 1);
	TCHECK(line_h > 0 && wrap_short <= line_h, "short text one line");
	TCHECK(wrap_long > line_h, "long text wraps multi-line");
	/* Worst case the contract allows (160 wide chars + question suffix)
	 * fits inside the 8-line cap: the cap never truncates real content. */
	{
		char wl[512];
		snprintf(wl, sizeof wl, "? %s  → answer", worst);
		wrap_worst = preview_text_h(wl, 1);
	}
	cap = line_h * PREV_MAX_LINES;
	printf("measure: line=%d short=%d long=%d worst=%d cap=%d\n",
	       line_h, wrap_short, wrap_long, wrap_worst, cap);
	TCHECK(wrap_worst <= cap, "worst case fits the line cap");
	/* Hover the long row: it grows to its full wrapped text, rows below
	 * shift down, total stays capped. */
	prev_hover = 1;
	preview_layout();
	TCHECK(prev_row_h[1] == wrap_long + 2 * PREV_TXT_PAD,
	       "expanded height = wrapped + padding");
	TCHECK(prev_row_h[1] > PREV_ROW_H, "long row grows on hover");
	TCHECK(prev_row_y[2] == prev_row_y[1] + prev_row_h[1] +
	       PREV_GAP,
	       "rows below shift down");
	TCHECK(prev_h <= PREV_MAX_TOTAL_H, "total height capped");
	TCHECK(preview_row_at(190, prev_row_y[1] + prev_row_h[1] - 2) == 1,
	       "hit-test uses expanded geometry");
	TCHECK(preview_row_at(190, prev_row_h[1] + PREV_PAD + 2) == 1,
	       "old row-2 offset now hits the grown row");
	/* Hover the long question row: suffix included, still expanded. */
	prev_hover = 3;
	preview_layout();
	TCHECK(prev_row_h[3] > PREV_ROW_H, "long question row grows");
	TCHECK(prev_h <= PREV_MAX_TOTAL_H, "total capped with q expanded");
	/* Hover a short row: geometry identical to compact. */
	prev_hover = 0;
	preview_layout();
	for (int i = 0; i < 4; i++)
		TCHECK(prev_row_y[i] == cy[i] &&
		       prev_row_h[i] == PREV_ROW_H,
		       "short hover leaves geometry untouched");
	TCHECK(prev_h == PREV_PAD * 2 + 4 * PREV_ROW_H + 3 * PREV_GAP,
	       "short hover leaves total untouched");
	TCHECK(preview_row_at(190, 15) == 0, "short hover still hits");
	/* Unhover restores the exact compact layout. */
	prev_hover = -1;
	preview_layout();
	for (int i = 0; i < 4; i++)
		TCHECK(prev_row_y[i] == cy[i], "unhover restores rows");
	TCHECK(prev_h == PREV_PAD * 2 + 4 * PREV_ROW_H + 3 * PREV_GAP,
	       "unhover restores total");
	/* Hover preserved by row id across reloads (motion path relies on
	 * this: it sets prev_hover then reloads). */
	prev_hover = 1;
	TCHECK(preview_reload() == 4, "reload keeps rows");
	TCHECK(prev_hover == 1 && !strcmp(prev_plain_id[1], "b.txt"),
	       "hover preserved by id, same index");
	unlink(a); /* drop row 0: b.txt slides to index 0, id follows */
	TCHECK(preview_reload() == 3, "reload after dismiss");
	TCHECK(prev_hover == 0 && !strcmp(prev_plain_id[0], "b.txt"),
	       "hover follows row id across shift");
	TCHECK(g_file_set_contents(a, "hi", -1, NULL), "restore a.txt");
	/* Tick dirty-check: unchanged queue reads as current (no flicker);
	 * a size change dirties it (refresh path runs). */
	prev_hover = -1;
	TCHECK(preview_reload() == 4, "reload restored");
	pl = preview_list(".txt");
	ql = preview_list(".q");
	TCHECK(preview_model_current(pl, ql), "unchanged queue is current");
	preview_free_list(pl);
	preview_free_list(ql);
	TCHECK(g_file_set_contents(c, "changed text here", -1, NULL),
	       "touch c.txt");
	pl = preview_list(".txt");
	ql = preview_list(".q");
	TCHECK(!preview_model_current(pl, ql), "touched queue is dirty");
	preview_free_list(pl);
	preview_free_list(ql);
	TCHECK(preview_dry_run() == 0, "dry-run still works");
	unlink(a);
	unlink(b);
	unlink(c);
	unlink(q);
	unlink(junk);
	rmdir(tmp);
	g_free(a);
	g_free(b);
	g_free(c);
	g_free(q);
	g_free(junk);
	g_free(tmp);
	if (!t_fail)
		printf("preview hover PASS\n");
	return t_fail;
}
#else
int main(int argc, char **argv)
{
	queue_dir = g_build_filename(g_get_user_data_dir(), "monowl",
				     "notifications", NULL);
	g_mkdir_with_parents(queue_dir, 0700);

	{
		int want_preview = 0, want_dry = 0;
		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ||
			    !strcmp(argv[i], "help")) {
				print_help();
				return 0;
			} else if (!strcmp(argv[i], "--preview")) {
				want_preview = 1;
			} else if (!strcmp(argv[i], "--dry-run")) {
				want_dry = 1;
			} else {
				fprintf(stderr, "notification-gui: unknown arg '%s' (see --help)\n",
					argv[i]);
				return 1;
			}
		}
		if (want_preview && want_dry) {
			int rc = preview_dry_run();
			g_free(queue_dir);
			return rc;
		}
		if (want_preview) {
			int rc = preview_main();
			g_free(queue_dir);
			return rc;
		}
		if (want_dry) {
			int rc = dry_run();
			g_free(queue_dir);
			return rc;
		}
	}

	/* Empty queue: exit before touching Wayland. */
	if (!load_oldest())
		return 0;
	layout_current();

	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "notification-gui: no Wayland display\n");
		return 1;
	}
	reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!comp || !shm || !layer_shell) {
		fprintf(stderr,
			"notification-gui: compositor lacks layer-shell\n");
		return 1;
	}
	surf = wl_compositor_create_surface(comp);
	layer_surf = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, surf, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "notification-gui");
	/* WHY anchor 0: with no edge anchored the compositor centers both
	 * axes; explicit size stops any stretch. Exclusive zone 0 reserves
	 * nothing. EXCLUSIVE keyboard so Esc works over fullscreen. */
	zwlr_layer_surface_v1_set_anchor(layer_surf, 0);
	zwlr_layer_surface_v1_set_size(layer_surf, DLG_W, dlg_h);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surf, 0);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		layer_surf,
		ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
	zwlr_layer_surface_v1_add_listener(layer_surf, &layer_listener, NULL);
	wl_surface_commit(surf);
	wl_display_flush(dpy);

	GMainLoop *loop = g_main_loop_new(NULL, FALSE);
	GIOChannel *ch = g_io_channel_unix_new(wl_display_get_fd(dpy));
	g_io_add_watch(ch, G_IO_IN, wl_dispatch_cb, NULL);
	g_io_channel_unref(ch);
	g_main_loop_run(loop);
	return 0;
}
#endif /* NOTIFGUI_TEST */
