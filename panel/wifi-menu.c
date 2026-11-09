/* monopanel-wifi — wifi menu + connect/disconnect helper for monopanel.
 *
 * Spawned by the bar on click; also runnable by hand. Talks to
 * NetworkManager via libnm only (never shells nmcli). UI is
 * `fuzzel --dmenu` (day-1 app): AP picker, and `--password` masked
 * prompt for PSKs. No display -> prints the list and exits 2.
 *
 * Secrecy: the PSK lives in one heap buffer, is never printed or logged
 * (only the SSID appears in messages), and is wiped with explicit_bzero
 * right after the libnm call consumes it.
 *
 * Usage: monopanel-wifi [-l LINES] [--list] [-i IFACE] [--dry-run [SSID]]
 *   --list  print "SSID|strength|sec|saved|active" lines, no fuzzel.
 *   --dry-run [SSID]  same list, then print the planned Connect/Disconnect
 *     action for SSID (or a default pick) WITHOUT calling any NM mutate
 *     method and without spawning fuzzel. Safe on the live MG-NETWORK:
 *     proves the handler routing (do_connect vs do_disconnect) with zero
 *     network change. Never prints or reads a PSK.
 */
#define _POSIX_C_SOURCE 200809L
#include <glib.h>
#include <NetworkManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *iface = "wlan0";
static int menu_lines = 12;
static int list_mode = 0;
static int dry_run = 0;
static const char *dry_target = NULL;
static GMainLoop *loop;
static NMClient *nm;

typedef struct {
	char *ssid;
	int strength;
	char *sec;
	gboolean needs_pw;
	gboolean saved;
	gboolean active;
} Ap;

static void ap_free(gpointer p)
{
	Ap *a = p;
	g_free(a->ssid);
	g_free(a->sec);
	g_free(a);
}

static const char *sec_of(NMAccessPoint *ap)
{
	NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
	NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);
	if (rsn & NM_802_11_AP_SEC_KEY_MGMT_SAE)
		return "WPA3";
	if (rsn & (NM_802_11_AP_SEC_KEY_MGMT_PSK |
		   NM_802_11_AP_SEC_KEY_MGMT_802_1X))
		return "WPA2";
	if (wpa & (NM_802_11_AP_SEC_KEY_MGMT_PSK |
		   NM_802_11_AP_SEC_KEY_MGMT_802_1X))
		return "WPA";
	if (nm_access_point_get_flags(ap) & NM_802_11_AP_FLAGS_PRIVACY) {
		if (rsn & NM_802_11_AP_SEC_KEY_MGMT_OWE)
			return "OWE";
		return "WEP";
	}
	return "open";
}

static char *ssid_of(NMAccessPoint *ap)
{
	GBytes *b = nm_access_point_get_ssid(ap);
	gsize n = 0;
	const char *s = b ? g_bytes_get_data(b, &n) : NULL;
	if (!s || !n || !g_utf8_validate(s, n, NULL))
		return NULL;
	char *out = g_malloc(n + 1);
	memcpy(out, s, n);
	out[n] = '\0';
	return out;
}

static NMDevice *find_wifi(void)
{
	const GPtrArray *devs = nm_client_get_devices(nm);
	for (guint i = 0; devs && i < devs->len; i++) {
		NMDevice *d = devs->pdata[i];
		if (NM_IS_DEVICE_WIFI(d) &&
		    !strcmp(nm_device_get_iface(d), iface))
			return d;
	}
	return NULL;
}

static gboolean ssid_is_saved(const char *ssid)
{
	const GPtrArray *conns = nm_client_get_connections(nm);
	for (guint i = 0; conns && i < conns->len; i++) {
		NMConnection *c = conns->pdata[i];
		NMSettingWireless *w = nm_connection_get_setting_wireless(c);
		if (!w)
			continue;
		GBytes *b = nm_setting_wireless_get_ssid(w);
		gsize n = 0;
		const char *s = b ? g_bytes_get_data(b, &n) : NULL;
		if (s && n == strlen(ssid) && !memcmp(s, ssid, n))
			return TRUE;
	}
	return FALSE;
}

static NMConnection *saved_conn_for(const char *ssid)
{
	const GPtrArray *conns = nm_client_get_connections(nm);
	for (guint i = 0; conns && i < conns->len; i++) {
		NMConnection *c = conns->pdata[i];
		NMSettingWireless *w = nm_connection_get_setting_wireless(c);
		if (!w)
			continue;
		GBytes *b = nm_setting_wireless_get_ssid(w);
		gsize n = 0;
		const char *s = b ? g_bytes_get_data(b, &n) : NULL;
		if (s && n == strlen(ssid) && !memcmp(s, ssid, n))
			return c;
	}
	return NULL;
}

/* best AP per SSID, sorted by strength desc */
static int ap_cmp(gconstpointer x, gconstpointer y)
{
	const Ap *a = *(Ap *const *)x, *b = *(Ap *const *)y;
	return b->strength - a->strength;
}

static GPtrArray *collect(NMDevice *dev)
{
	GHashTable *best = g_hash_table_new_full(g_str_hash, g_str_equal,
						 g_free, NULL);
	const GPtrArray *aps = nm_device_wifi_get_access_points(
		NM_DEVICE_WIFI(dev));
	NMAccessPoint *active =
		nm_device_wifi_get_active_access_point(NM_DEVICE_WIFI(dev));
	char *active_ssid = active ? ssid_of(active) : NULL;

	for (guint i = 0; aps && i < aps->len; i++) {
		char *ssid = ssid_of(aps->pdata[i]);
		if (!ssid)
			continue; /* hidden; skip (counted below) */
		NMAccessPoint *prev = g_hash_table_lookup(best, ssid);
		if (!prev || nm_access_point_get_strength(aps->pdata[i]) >
				     nm_access_point_get_strength(prev)) {
			g_hash_table_replace(best, ssid, aps->pdata[i]);
		} else {
			g_free(ssid);
		}
	}
	GPtrArray *out = g_ptr_array_new_with_free_func(ap_free);
	GHashTableIter it;
	gpointer k, v;
	g_hash_table_iter_init(&it, best);
	while (g_hash_table_iter_next(&it, &k, &v)) {
		Ap *a = g_new0(Ap, 1);
		a->ssid = g_strdup(k);
		a->strength = nm_access_point_get_strength(v);
		const char *sec = sec_of(v);
		a->sec = g_strdup(sec);
		a->needs_pw = strcmp(sec, "open") && strcmp(sec, "OWE");
		a->saved = ssid_is_saved(a->ssid);
		a->active = active_ssid && !strcmp(a->ssid, active_ssid);
		g_ptr_array_add(out, a);
	}
	g_hash_table_destroy(best);
	g_free(active_ssid);
	g_ptr_array_sort(out, ap_cmp);
	return out;
}

/* ---------- fuzzel ---------- */

/* wipe a secret so the compiler cannot elide it */
static void wipe_secret(char *s)
{
	volatile char *v = (volatile char *)s;
	while (*v)
		*v++ = '\0';
}

static char *run_fuzzel(char **argv, const char *input)
{
	/* WHY loud: the old STDERR_SILENCE made a dead menu indistinguishable
	 * from a working one (click log with no follow-up). Now fuzzel stderr
	 * lands in /tmp/monowl.log via the panel's stderr, plus notify-send,
	 * so the next failure names itself. */
	GError *err = NULL;
	GSubprocess *sp = g_subprocess_newv(
		(const char *const *)argv,
		G_SUBPROCESS_FLAGS_STDIN_PIPE |
			G_SUBPROCESS_FLAGS_STDOUT_PIPE |
			G_SUBPROCESS_FLAGS_STDERR_PIPE,
		&err);
	if (!sp) {
		g_printerr("monopanel-wifi: cannot spawn fuzzel: %s\n",
			   err->message);
		char *nargv[] = { "notify-send", "monopanel-wifi",
				  "cannot spawn fuzzel (see /tmp/monowl.log)",
				  NULL };
		g_spawn_async(NULL, nargv, NULL, G_SPAWN_SEARCH_PATH, NULL,
			      NULL, NULL, NULL);
		g_clear_error(&err);
		return NULL;
	}
	char *out = NULL, *fstderr = NULL;
	if (!g_subprocess_communicate_utf8(sp, input, NULL, &out, &fstderr,
					   &err)) {
		g_printerr("monopanel-wifi: fuzzel IO failed: %s\n",
			   err ? err->message : "unknown");
		g_clear_error(&err);
		g_object_unref(sp);
		g_free(out);
		g_free(fstderr);
		return NULL;
	}
	if (fstderr && *fstderr)
		g_printerr("monopanel-wifi: fuzzel stderr: %.500s\n", fstderr);
	g_free(fstderr);
	if (!g_subprocess_get_successful(sp)) {
		int st = g_subprocess_get_exit_status(sp);
		g_printerr("monopanel-wifi: fuzzel exit status %d\n", st);
	}
	g_object_unref(sp);
	if (!out)
		return NULL;
	g_strstrip(out);
	return out; /* caller frees; empty string = user aborted */
}

static void notify(const char *msg)
{
	if (g_find_program_in_path("notify-send")) {
		char *argv[] = { "notify-send", "monopanel-wifi", (char *)msg,
				 NULL };
		g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL,
			      NULL, NULL, NULL);
	}
	printf("monopanel-wifi: %s\n", msg);
}

/* ---------- NM operations (short-lived helper) ----------
 * Method _finish only means "activation started"; we then watch the
 * active-connection state until ACTIVATED before reporting success. */

typedef struct {
	GMainLoop *loop;
	guint timeout;
	gboolean ok;
	gboolean is_add; /* TRUE: add_and_activate finish, else activate */
	NMActiveConnection *watched; /* ref held while waiting for settle */
} ConnOp;

static gboolean conn_timeout(gpointer p)
{
	ConnOp *op = p;
	op->ok = FALSE;
	g_main_loop_quit(op->loop);
	return G_SOURCE_REMOVE;
}

static void ac_state_cb(NMActiveConnection *ac, GParamSpec *ps, gpointer p)
{
	ConnOp *op = p;
	(void)ps;
	NMActiveConnectionState st = nm_active_connection_get_state(ac);
	if (st == NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
		op->ok = TRUE;
		g_main_loop_quit(op->loop);
	} else if (st == NM_ACTIVE_CONNECTION_STATE_DEACTIVATED) {
		op->ok = FALSE;
		g_main_loop_quit(op->loop);
	}
}

/* shared finish for activate + add_and_activate: the method result only
 * means "activation started", so watch the active-connection state until
 * ACTIVATED (real success) before reporting. Caller sets op->is_add to
 * pick the right _finish. */
static void connect_finish(GObject *src, GAsyncResult *res, gpointer p)
{
	ConnOp *op = p;
	GError *err = NULL;
	NMActiveConnection *ac;
	(void)src;
	if (op->is_add) /* picked by caller: which _finish to use */
		ac = nm_client_add_and_activate_connection_finish(nm, res,
								  &err);
	else
		ac = nm_client_activate_connection_finish(nm, res, &err);
	op->ok = FALSE;
	if (err) {
		g_printerr("monopanel-wifi: request failed: %s\n",
			   err->message);
		g_clear_error(&err);
		g_main_loop_quit(op->loop);
		return;
	}
	if (!ac) {
		g_main_loop_quit(op->loop);
		return;
	}
	if (nm_active_connection_get_state(ac) ==
	    NM_ACTIVE_CONNECTION_STATE_ACTIVATED) {
		op->ok = TRUE;
		g_object_unref(ac);
		g_main_loop_quit(op->loop);
		return;
	}
	/* still activating: wait for settled state on the same loop */
	op->watched = ac; /* keep alive until the run below returns */
	g_signal_connect(ac, "notify::state", G_CALLBACK(ac_state_cb), op);
}

static void deactivate_done(GObject *src, GAsyncResult *res, gpointer p)
{
	ConnOp *op = p;
	GError *err = NULL;
	op->ok = nm_client_deactivate_connection_finish(nm, res, &err);
	(void)src;
	if (err) {
		g_printerr("monopanel-wifi: disconnect failed: %s\n",
			   err->message);
		g_clear_error(&err);
	}
	g_main_loop_quit(op->loop);
}

static gboolean do_connect(NMDevice *dev, Ap *a, const char *psk)
{
	ConnOp op = { g_main_loop_new(NULL, FALSE), 0, FALSE, FALSE, NULL };
	op.timeout = g_timeout_add_seconds(30, conn_timeout, &op);
	NMConnection *saved = saved_conn_for(a->ssid);
	if (saved) {
		op.is_add = FALSE;
		nm_client_activate_connection_async(nm, saved, dev, NULL,
						    NULL, connect_finish,
						    &op);
	} else {
		NMConnection *c = nm_simple_connection_new();
		NMSettingConnection *sc = (NMSettingConnection *)
			nm_setting_connection_new();
		char *uuid = nm_utils_uuid_generate();
		g_object_set(sc, NM_SETTING_CONNECTION_ID, a->ssid,
			     NM_SETTING_CONNECTION_UUID, uuid,
			     NM_SETTING_CONNECTION_TYPE,
			     NM_SETTING_WIRELESS_SETTING_NAME, NULL);
		g_free(uuid);
		nm_connection_add_setting(c, (NMSetting *)sc);
		NMSettingWireless *sw =
			(NMSettingWireless *)nm_setting_wireless_new();
		GBytes *ssid = g_bytes_new(a->ssid, strlen(a->ssid));
		g_object_set(sw, NM_SETTING_WIRELESS_SSID, ssid,
			     NM_SETTING_WIRELESS_MODE, "infrastructure",
			     NULL);
		g_bytes_unref(ssid);
		nm_connection_add_setting(c, (NMSetting *)sw);
		if (psk) {
			NMSettingWirelessSecurity *ws =
				(NMSettingWirelessSecurity *)
					nm_setting_wireless_security_new();
			g_object_set(ws,
				     NM_SETTING_WIRELESS_SECURITY_KEY_MGMT,
				     "wpa-psk", NM_SETTING_WIRELESS_SECURITY_PSK,
				     psk, NULL);
			nm_connection_add_setting(c, (NMSetting *)ws);
		}
		op.is_add = TRUE;
		nm_client_add_and_activate_connection_async(
			nm, c, dev, NULL, NULL, connect_finish, &op);
		g_object_unref(c);
	}
	g_main_loop_run(op.loop);
	g_source_remove(op.timeout);
	g_clear_object(&op.watched);
	gboolean ok = op.ok;
	g_main_loop_unref(op.loop);
	return ok;
}

static gboolean do_disconnect(NMDevice *dev)
{
	NMActiveConnection *ac = nm_device_get_active_connection(dev);
	if (!ac)
		return TRUE;
	ConnOp op = { g_main_loop_new(NULL, FALSE), 0, FALSE, FALSE, NULL };
	op.timeout = g_timeout_add_seconds(10, conn_timeout, &op);
	nm_client_deactivate_connection_async(nm, ac, NULL, deactivate_done,
					      &op);
	g_main_loop_run(op.loop);
	g_source_remove(op.timeout);
	gboolean ok = op.ok;
	g_main_loop_unref(op.loop);
	return ok;
}

/* ---------- flows ---------- */

static void interactive(NMDevice *dev, GPtrArray *aps)
{
	/* WHY no env gate: the panel itself often has no WAYLAND_DISPLAY in
	 * env (started via `monowl -s` before setenv, connects via the
	 * default socket) yet renders fine — and so can fuzzel, which uses
	 * the same default. Gating on getenv here silently turned every bar
	 * click into a list-dump. Try fuzzel; the new run_fuzzel logging
	 * reports a real display failure loudly instead. */
	if (!g_getenv("WAYLAND_DISPLAY") && !g_getenv("DISPLAY"))
		g_printerr("monopanel-wifi: no WAYLAND_DISPLAY/DISPLAY in env, trying default socket anyway\n");
	GString *menu = g_string_new(NULL);
	for (guint i = 0; i < aps->len; i++) {
		Ap *a = aps->pdata[i];
		g_string_append_printf(menu, "%s %s  %d%%  %s%s\n",
				       a->active ? "*" : " ", a->ssid,
				       a->strength, a->sec,
				       a->saved ? "  [saved]" : "");
	}
	char *nlines = g_strdup_printf("%d", menu_lines);
	char *pick_argv[] = { "fuzzel", "--dmenu", "--index", "-l", nlines,
			      "-p", "Wi-Fi: ", "--anchor=top",
			      "--y-margin=32", "--layer=overlay", "-w", "50",
			      NULL };
	char *pick = run_fuzzel(pick_argv, menu->str);
	g_string_free(menu, TRUE);
	g_free(nlines);
	if (!pick || !*pick) {
		if (!pick)
			g_printerr("monopanel-wifi: menu aborted (fuzzel failed, see above)\n");
		else
			fprintf(stderr, "monopanel-wifi: menu dismissed by user\n");
		g_free(pick);
		g_main_loop_quit(loop);
		return; /* aborted */
	}
	int idx = atoi(pick);
	g_free(pick);
	if (idx < 0 || (guint)idx >= aps->len) {
		g_main_loop_quit(loop);
		return;
	}
	Ap *a = aps->pdata[idx];
	if (a->active) {
		notify(do_disconnect(dev) ? "disconnected" :
						    "disconnect failed");
	} else if (!a->needs_pw || a->saved) {
		char *msg = g_strdup_printf("connecting to %s…", a->ssid);
		notify(msg);
		g_free(msg);
		msg = g_strdup_printf(do_connect(dev, a, NULL) ?
						      "connected" :
						      "connect failed");
		notify(msg);
		g_free(msg);
	} else {
		char *prompt = g_strdup_printf("Password for %s: ", a->ssid);
		char *pw_argv[] = { "fuzzel", "--dmenu", "--password",
				    "--prompt-only", "--lines=0", "-p", prompt,
				    "--anchor=top", "--y-margin=32",
				    "--layer=overlay", NULL };
		char *psk = run_fuzzel(pw_argv, NULL);
		g_free(prompt);
		if (!psk || !*psk) {
			g_free(psk);
			g_main_loop_quit(loop);
			return; /* aborted */
		}
		gboolean ok = do_connect(dev, a, psk);
		wipe_secret(psk); /* wipe secret at once */
		g_free(psk);
		notify(ok ? "connected" : "connect failed (wrong password?)");
	}
	g_main_loop_quit(loop);
}

static gboolean scan_done(gpointer p)
{
	(void)p;
	NMDevice *dev = find_wifi();
	if (!dev) {
		g_printerr("monopanel-wifi: no wifi device '%s'\n", iface);
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	GPtrArray *aps = collect(dev);
	if (dry_run) {
		/* Proof routing without mutation: same Ap rows the real
		 * menu uses, then the handler interactive() would call. */
		for (guint i = 0; i < aps->len; i++) {
			Ap *a = aps->pdata[i];
			printf("%s|%d|%s|%s|%s\n", a->ssid, a->strength,
			       a->sec, a->saved ? "saved" : "-",
			       a->active ? "active" : "-");
		}
		Ap *t = NULL;
		if (dry_target) {
			for (guint i = 0; i < aps->len && !t; i++)
				if (!strcmp(((Ap *)aps->pdata[i])->ssid,
					    dry_target))
					t = aps->pdata[i];
			if (!t) {
				printf("dry-run: SSID '%s' not in scan; "
				       "no action (NOT executed)\n",
				       dry_target);
				g_ptr_array_unref(aps);
				g_main_loop_quit(loop);
				return G_SOURCE_REMOVE;
			}
		} else {
			for (guint i = 0; i < aps->len && !t; i++)
				if (!((Ap *)aps->pdata[i])->active)
					t = aps->pdata[i];
			if (!t && aps->len)
				t = aps->pdata[0];
		}
		if (!t) {
			printf("dry-run: no APs visible; no action "
			       "(NOT executed)\n");
		} else if (t->active) {
			printf("dry-run: would Disconnect '%s' via "
			       "do_disconnect (NOT executed)\n",
			       t->ssid);
		} else if (!t->needs_pw || t->saved) {
			printf("dry-run: would Connect '%s' via do_connect "
			       "(sec=%s saved=%s, NOT executed)\n",
			       t->ssid, t->sec, t->saved ? "yes" : "no");
		} else {
			printf("dry-run: would prompt PSK then Connect '%s' "
			       "via do_connect (sec=%s, NOT executed)\n",
			       t->ssid, t->sec);
		}
		g_ptr_array_unref(aps);
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	if (list_mode) {
		for (guint i = 0; i < aps->len; i++) {
			Ap *a = aps->pdata[i];
			printf("%s|%d|%s|%s|%s\n", a->ssid, a->strength,
			       a->sec, a->saved ? "saved" : "-",
			       a->active ? "active" : "-");
		}
		g_ptr_array_unref(aps);
		g_main_loop_quit(loop);
		return G_SOURCE_REMOVE;
	}
	interactive(dev, aps);
	g_ptr_array_unref(aps);
	return G_SOURCE_REMOVE; /* interactive() already quit or will */
}

static void scan_req_done(GObject *src, GAsyncResult *res, gpointer p)
{
	(void)src;
	(void)p;
	GError *err = NULL;
	/* Best effort: cached AP list is usable even if rescan is denied. */
	if (!nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(find_wifi()),
						res, &err))
		g_clear_error(&err);
}

static void on_client(GObject *src, GAsyncResult *res, gpointer p)
{
	(void)src;
	(void)p;
	GError *err = NULL;
	nm = nm_client_new_finish(res, &err);
	if (!nm) {
		g_printerr("monopanel-wifi: NM unavailable: %s\n",
			   err ? err->message : "?");
		g_clear_error(&err);
		g_main_loop_quit(loop);
		return;
	}
	NMDevice *dev = find_wifi();
	if (!dev) {
		g_printerr("monopanel-wifi: no wifi device '%s'\n", iface);
		g_main_loop_quit(loop);
		return;
	}
	/* WHY instant menu: show the cached AP list right away (NetworkManager
	 * keeps it fresh with its own background scans) and fire a rescan
	 * for the *next* open, instead of making every click wait 3s. */
	nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(dev), NULL,
					  scan_req_done, NULL);
	g_idle_add(scan_done, NULL);
}

int main(int argc, char **argv)
{
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--list"))
			list_mode = 1;
		else if (!strcmp(argv[i], "--dry-run")) {
			dry_run = 1;
			if (i + 1 < argc && argv[i + 1][0] != '-')
				dry_target = argv[++i];
		} else if (!strcmp(argv[i], "-l") && i + 1 < argc) {
			/* WHY temp first: CLAMP evaluates its arg 2-3x, so
			 * atoi(argv[++i]) inside it walks past argv into NULL
			 * and segfaults (click path always passes -l). */
			int v = atoi(argv[++i]);
			menu_lines = CLAMP(v, 4, 40);
		} else if (!strcmp(argv[i], "-i") && i + 1 < argc)
			iface = argv[++i];
		else {
			g_printerr("usage: %s [-l LINES] [--list] [--dry-run [SSID]] [-i IFACE]\n",
				   argv[0]);
			return 2;
		}
	}
	loop = g_main_loop_new(NULL, FALSE);
	nm_client_new_async(NULL, on_client, NULL);
	g_main_loop_run(loop);
	g_clear_object(&nm);
	return 0;
}
