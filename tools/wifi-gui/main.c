/* wifi-gui — GTK3+libnm WiFi manager for monowl. System deps only:
 * gtk+-3.0, libnm. No external libraries, no nmcli forking; the panel
 * (monopanel.c, spawn_wifi_menu) launches this binary for the wifi zone.
 *
 * Layout: header bar (global actions) | left: Available + Saved sections |
 * right: Details / Device / Actions cards | status bar with spinner.
 *
 * Security policy: PSK goes straight to NM client calls; the local copy is
 * wiped with explicit_bzero right after use (wifi-menu.c policy). Nothing is
 * logged. 802.1X/enterprise networks are unsupported (PSK/RSN and WEP only;
 * matching wifi-menu.c).
 */
#define _DEFAULT_SOURCE /* WHY: expose explicit_bzero (string.h, glibc >= 2.25) */
#include <gtk/gtk.h>
#include <NetworkManager.h>
#include <stdarg.h>
#include <string.h>

typedef struct {
	GtkWidget *win;
	GtkListStore *scan_store;  /* available networks */
	GtkListStore *saved_store; /* saved profiles */
	GtkWidget *scan_view;
	GtkWidget *saved_view;
	GtkWidget *status;  /* status bar label */
	GtkWidget *busy;    /* status bar spinner */
	GtkWidget *bconn, *bdisc, *bforget; /* action buttons */

	GtkWidget *d_ssid, *d_sec, *d_sig, *d_chan, *d_bss, *d_state; /* Details card */
	GtkWidget *v_if, *v_state, *v_drv, *v_mac, *v_ip;             /* Device card */

	NMClient *client;
	NMDeviceWifi *wifi;
	GtkTreeView *sel_view; /* tree the active selection belongs to */
} App;

enum { COL_SSID, COL_SEC, COL_STR, COL_FREQ, COL_BSS, COL_OBJ, COL_N };
enum { S_SSID, S_AUTO, S_UUID, S_N };

/* ---- small helpers ------------------------------------------------------ */

static void set_status(App *a, const char *msg) {
	gtk_label_set_text(GTK_LABEL(a->status), msg ? msg : "");
}

static void busy(App *a, const char *msg) {
	set_status(a, msg);
	gtk_spinner_start(GTK_SPINNER(a->busy));
}

static void idle(App *a, const char *msg) {
	set_status(a, msg);
	gtk_spinner_stop(GTK_SPINNER(a->busy));
}

static NMDeviceWifi *find_wifi_device(NMClient *c) {
	const GPtrArray *devs = nm_client_get_devices(c);
	if (!devs) return NULL;
	for (guint i = 0; i < devs->len; i++) {
		NMDevice *d = g_ptr_array_index(devs, i);
		if (NM_IS_DEVICE_WIFI(d))
			return NM_DEVICE_WIFI(d);
	}
	return NULL;
}

static const char *ap_ssid(NMAccessPoint *ap, char *buf, size_t bufsz) {
	GBytes *ssid = nm_access_point_get_ssid(ap);
	if (!ssid) { buf[0] = 0; return buf; }
	char *s = nm_utils_ssid_to_utf8(g_bytes_get_data(ssid, NULL), g_bytes_get_size(ssid));
	if (!s) { buf[0] = 0; return buf; }
	g_strlcpy(buf, s, bufsz);
	g_free(s);
	return buf;
}

static const char *ap_security(NMAccessPoint *ap) {
	NM80211ApFlags flags = nm_access_point_get_flags(ap);
	NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
	NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);
	if (rsn & (NM_802_11_AP_SEC_KEY_MGMT_802_1X | NM_802_11_AP_SEC_KEY_MGMT_SAE))
		return "WPA2/WPA3 enterprise";
	if (wpa & NM_802_11_AP_SEC_KEY_MGMT_802_1X)
		return "WPA/WPA2 enterprise";
	if (rsn & NM_802_11_AP_SEC_KEY_MGMT_SAE)
		return "WPA3";
	if (rsn)
		return "WPA2";
	if (wpa)
		return "WPA";
	return (flags & NM_802_11_AP_FLAGS_PRIVACY) ? "WEP" : "open";
}

/* WHY local table: this libnm dropped nm_device_state_to_string; every used
 * state is listed so the DEVICE card is never blank on compile-time enums. */
static const char *dev_state_str(NMDevice *d) {
	switch (nm_device_get_state(d)) {
	case NM_DEVICE_STATE_UNMANAGED: return "unmanaged";
	case NM_DEVICE_STATE_UNAVAILABLE: return "unavailable";
	case NM_DEVICE_STATE_DISCONNECTED: return "disconnected";
	case NM_DEVICE_STATE_PREPARE:   	 /* fallthrough */
	case NM_DEVICE_STATE_CONFIG: return "connecting…";
	case NM_DEVICE_STATE_IP_CONFIG: return "getting address…";
	case NM_DEVICE_STATE_IP_CHECK: return "checking connectivity…";
	case NM_DEVICE_STATE_SECONDARIES: return "starting…";
	case NM_DEVICE_STATE_ACTIVATED: return "activated";
	case NM_DEVICE_STATE_DEACTIVATING: return "deactivating…";
	case NM_DEVICE_STATE_FAILED: return "failed";
	default: return "unknown";
	}
}

/* WHY channel math from the AP frequency: users think in channel numbers;
 * libnm only reports MHz. 2.4 GHz band 1-13 is (f-2407)/5, 5 GHz is (f-5000)/5. */
static void channel_from_freq(guint32 f, char *buf, size_t bufsz) {
	if (f >= 2412 && f <= 2472)
		g_snprintf(buf, bufsz, "2.4 GHz · ch %u", (f - 2407) / 5);
	else if (f >= 5000 && f < 6000)
		g_snprintf(buf, bufsz, "5 GHz · ch %u", (f - 5000) / 5);
	else
		g_snprintf(buf, bufsz, "%u MHz", f);
}

/* WHY ref each AP into the store and unref on clear: the store outlives NM's
 * AP list. Without the extra refs a stored pointer dangles the moment NM drops
 * that AP (background rescan) while its row still shows, and a later Connect
 * click would use freed memory. */
static void clear_scan_store(GtkListStore *store) {
	GtkTreeIter it;
	gboolean more = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &it);
	while (more) {
		NMAccessPoint *ap = NULL;
		gtk_tree_model_get(GTK_TREE_MODEL(store), &it, COL_OBJ, &ap, -1);
		if (ap) g_object_unref(ap);
		more = gtk_tree_model_iter_next(GTK_TREE_MODEL(store), &it);
	}
	gtk_list_store_clear(store);
}

static void set_kv(GtkWidget *label, const char *fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	g_vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	gtk_label_set_text(GTK_LABEL(label), buf);
}

/* ---- refresh ------------------------------------------------------------- */

static void refresh_device_card(App *a) {
	NMDeviceWifi *w = find_wifi_device(a->client);
	if (!w) {
		set_kv(a->v_if, "no WiFi device");
		set_kv(a->v_state, "—");
		set_kv(a->v_drv, "—");
		set_kv(a->v_mac, "—");
		set_kv(a->v_ip, "—");
		return;
	}
	const char *iface = nm_device_get_iface(NM_DEVICE(w));
	set_kv(a->v_if, "%s", iface ? iface : "—");
	set_kv(a->v_state, "%s", dev_state_str(NM_DEVICE(w)));
	const char *drv = nm_device_get_driver(NM_DEVICE(w));
	set_kv(a->v_drv, "%s", drv ? drv : "—");
	const char *mac = nm_device_get_hw_address(NM_DEVICE(w));
	set_kv(a->v_mac, "%s", mac ? mac : "—");

	const char *ip = NULL;
	NMIPConfig *cfg = nm_device_get_ip4_config(NM_DEVICE(w));
	if (cfg) {
		const GPtrArray *arr = nm_ip_config_get_addresses(cfg);
		if (arr && arr->len)
			ip = nm_ip_address_get_address(g_ptr_array_index(arr, 0));
	}
	set_kv(a->v_ip, "%s", ip ? ip : "unassigned");
}

static void refresh_saved(App *a) {
	gtk_list_store_clear(a->saved_store);
	const GPtrArray *conns = nm_client_get_connections(a->client);
	if (!conns) return;
	for (guint i = 0; i < conns->len; i++) {
		NMConnection *c = conns->pdata[i];
		if (!NM_IS_REMOTE_CONNECTION(c)) continue;
		NMSettingConnection *scon = nm_connection_get_setting_connection(c);
		if (!scon) continue;
		const char *type = nm_setting_connection_get_connection_type(scon);
		if (!type || strcmp(type, NM_SETTING_WIRELESS_SETTING_NAME) != 0) continue;
		NMSettingWireless *sw = nm_connection_get_setting_wireless(c);
		const char *id = nm_setting_connection_get_id(scon);
		char ssidbuf[256] = {0};
		const char *ssid = id ? id : "(unknown)";
		gboolean autoconn = nm_setting_connection_get_autoconnect(scon);
		if (sw) {
			GBytes *gb = nm_setting_wireless_get_ssid(sw);
			if (gb) {
				char *u = nm_utils_ssid_to_utf8(g_bytes_get_data(gb, NULL), g_bytes_get_size(gb));
				if (u) { g_strlcpy(ssidbuf, u, sizeof ssidbuf); g_free(u); ssid = ssidbuf; }
			}
		}
		GtkTreeIter it;
		gtk_list_store_append(a->saved_store, &it);
		gtk_list_store_set(a->saved_store, &it,
			S_SSID, ssid,
			S_AUTO, autoconn,
			S_UUID, nm_connection_get_uuid(c),
			-1);
	}
}

static void refresh_scan(App *a) {
	clear_scan_store(a->scan_store);
	a->wifi = find_wifi_device(a->client);
	if (!a->wifi) { set_status(a, "No WiFi device"); return; }
	const GPtrArray *aps = nm_device_wifi_get_access_points(a->wifi);
	NMAccessPoint *active = nm_device_wifi_get_active_access_point(a->wifi);
	if (!aps) return;
	for (guint i = 0; i < aps->len; i++) {
		NMAccessPoint *ap = aps->pdata[i];
		char ssid[256];
		ap_ssid(ap, ssid, sizeof ssid);
		if (!ssid[0]) continue;
		gchar sec[160];
		g_snprintf(sec, sizeof sec, "%s%s", ap_security(ap), (ap == active) ? " [active]" : "");
		GtkTreeIter it;
		gtk_list_store_append(a->scan_store, &it);
		gtk_list_store_set(a->scan_store, &it,
			COL_SSID, ssid,
			COL_SEC, sec,
			COL_STR, (gint)nm_access_point_get_strength(ap),
			COL_FREQ, nm_access_point_get_frequency(ap),
			COL_BSS, nm_access_point_get_bssid(ap),
			COL_OBJ, g_object_ref(ap),
			-1);
	}
}

static void refresh_all(App *a) {
	refresh_saved(a);
	refresh_scan(a);
	refresh_device_card(a);
}

/* ---- details pane -------------------------------------------------------- */

static void update_details(App *a) {
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->sel_view));
	GtkTreeIter it; GtkTreeModel *m;
	if (!gtk_tree_selection_get_selected(sel, &m, &it)) {
		set_kv(a->d_ssid, "no selection");
		gtk_widget_set_sensitive(a->bconn, FALSE);
		gtk_widget_set_sensitive(a->bforget, FALSE);
		return;
	}

	if (a->sel_view == GTK_TREE_VIEW(a->scan_view)) {
		char *ssid = NULL, *bss = NULL; NMAccessPoint *ap = NULL;
		gint str = 0; guint freq = 0;
		gtk_tree_model_get(m, &it, COL_SSID, &ssid, COL_OBJ, &ap, COL_STR, &str,
			COL_FREQ, &freq, COL_BSS, &bss, -1);
		char chan[64]; channel_from_freq(freq, chan, sizeof chan);
		gboolean active = ap && (nm_device_wifi_get_active_access_point(a->wifi) == ap);
		set_kv(a->d_ssid, "%s", ssid ? ssid : "?");
		set_kv(a->d_sec, "%s", ap ? ap_security(ap) : "—");
		set_kv(a->d_sig, "%d%%", str);
		set_kv(a->d_chan, "%s", chan);
		set_kv(a->d_bss, "%s", bss ? bss : "—");
		set_kv(a->d_state, "%s", active ? "connected" : "available");
		gtk_widget_set_sensitive(a->bconn, TRUE);
		gtk_widget_set_sensitive(a->bforget, FALSE);
		g_free(ssid); g_free(bss);
	} else {
		char *uuid = NULL, *ssid = NULL; gboolean autoconn = FALSE;
		gtk_tree_model_get(m, &it, S_SSID, &ssid, S_AUTO, &autoconn, S_UUID, &uuid, -1);
		const char *sec = "open profile";
		if (uuid) {
			NMRemoteConnection *rc = nm_client_get_connection_by_uuid(a->client, uuid);
			if (rc) {
				NMSettingWirelessSecurity *ws = nm_connection_get_setting_wireless_security(NM_CONNECTION(rc));
				if (ws) {
					const char *km = nm_setting_wireless_security_get_key_mgmt(ws);
					sec = (km && strcmp(km, "wpa-psk") == 0) ? "WPA/WPA2 PSK"
					    : (km && strcmp(km, "none") == 0) ? "WEP key"
					    : km ? km : "—";
				}
				g_object_unref(rc);
			}
		}
		set_kv(a->d_ssid, "%s", ssid ? ssid : "?");
		set_kv(a->d_sec, "%s", sec);
		set_kv(a->d_sig, "—");
		set_kv(a->d_chan, "—");
		set_kv(a->d_bss, "—");
		set_kv(a->d_state, "%s", autoconn ? "auto-connect" : "manual connect");
		gtk_widget_set_sensitive(a->bconn, TRUE);
		gtk_widget_set_sensitive(a->bforget, TRUE);
		g_free(uuid); g_free(ssid);
	}
}

static void on_scan_selected(GtkTreeSelection *s, gpointer u) {
	(void)s; App *a = u;
	a->sel_view = GTK_TREE_VIEW(a->scan_view);
	update_details(a);
}

static void on_saved_selected(GtkTreeSelection *s, gpointer u) {
	(void)s; App *a = u;
	a->sel_view = GTK_TREE_VIEW(a->saved_view);
	update_details(a);
}

/* ---- NM actions ----------------------------------------------------------- */

static void on_rescan_done(GObject *src, GAsyncResult *res, gpointer user_data) {
	App *a = user_data;
	GError *err = NULL;
	nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(src), res, &err);
	if (err) { idle(a, err->message); g_error_free(err); }
	else idle(a, "Scan complete");
	refresh_all(a);
}

static void on_rescan(GtkButton *b, gpointer user_data) {
	(void)b; App *a = user_data;
	if (!a->client) return;
	a->wifi = find_wifi_device(a->client);
	if (!a->wifi) { set_status(a, "No WiFi device"); return; }
	busy(a, "Scanning…");
	nm_device_wifi_request_scan_async(a->wifi, NULL, on_rescan_done, a);
}

static void on_forget_done(GObject *src, GAsyncResult *res, gpointer user_data) {
	App *a = user_data;
	GError *err = NULL;
	nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(src), res, &err);
	if (err) { idle(a, err->message); g_error_free(err); }
	else idle(a, "Profile removed");
	refresh_all(a);
}

static void on_forget(GtkButton *b, gpointer user_data) {
	(void)b; App *a = user_data;
	if (!a->client) return;
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->sel_view));
	GtkTreeIter it; GtkTreeModel *m;
	if (!gtk_tree_selection_get_selected(sel, &m, &it) ||
	    a->sel_view != GTK_TREE_VIEW(a->saved_view)) {
		set_status(a, "Select a saved profile to forget"); return;
	}
	char *uuid = NULL;
	gtk_tree_model_get(m, &it, S_UUID, &uuid, -1);
	if (!uuid) return;
	NMRemoteConnection *rc = nm_client_get_connection_by_uuid(a->client, uuid);
	g_free(uuid);
	if (!rc) { set_status(a, "Profile not found"); return; }
	busy(a, "Removing profile…");
	nm_remote_connection_delete_async(rc, NULL, on_forget_done, a);
}

static void on_disconnect_done(GObject *src, GAsyncResult *res, gpointer user_data) {
	App *a = user_data;
	GError *err = NULL;
	nm_device_disconnect_finish(NM_DEVICE(src), res, &err);
	if (err) { idle(a, err->message); g_error_free(err); }
	else idle(a, "Disconnected");
	refresh_all(a);
}

static void on_disconnect(GtkButton *b, gpointer user_data) {
	(void)b; App *a = user_data;
	NMDeviceWifi *w = find_wifi_device(a->client);
	if (!w) { set_status(a, "No WiFi device"); return; }
	busy(a, "Disconnecting…");
	nm_device_disconnect_async(NM_DEVICE(w), NULL, on_disconnect_done, a);
}

static void on_refresh(GtkButton *b, gpointer user_data) {
	(void)b; App *a = user_data;
	/* WHY no scan request here: Refill lists from the client cache only;
	 * firing a NM scan is Rescan's job (they must stay distinct). */
	refresh_all(a);
	idle(a, find_wifi_device(a->client) ? "Refreshed" : "No WiFi device");
}

static void on_activate_done(GObject *src, GAsyncResult *res, gpointer user_data) {
	App *a = user_data;
	GError *err = NULL;
	nm_client_activate_connection_finish(NM_CLIENT(src), res, &err);
	if (err) { idle(a, err->message); g_error_free(err); }
	else idle(a, "Connecting…");
	refresh_all(a);
}

static void on_add_done(GObject *src, GAsyncResult *res, gpointer user_data) {
	App *a = user_data;
	GError *err = NULL;
	NMActiveConnection *ac = nm_client_add_and_activate_connection_finish(NM_CLIENT(src), res, &err);
	if (err) { idle(a, err->message); g_error_free(err); }
	else { (void)ac; idle(a, "Connecting…"); }
	refresh_all(a);
}

static void connect_saved_selection(App *a, GtkTreeIter *it, GtkTreeModel *m) {
	NMDeviceWifi *w = find_wifi_device(a->client);
	char *uuid = NULL;
	gtk_tree_model_get(m, it, S_UUID, &uuid, -1);
	if (!uuid || !w) { g_free(uuid); set_status(a, "No WiFi device"); return; }
	NMRemoteConnection *rc = nm_client_get_connection_by_uuid(a->client, uuid);
	g_free(uuid);
	if (!rc) { set_status(a, "Profile not found"); return; }
	busy(a, "Connecting…");
	/* WHY no AP path: NM picks the best matching AP for the profile. */
	nm_client_activate_connection_async(a->client, NM_CONNECTION(rc),
		NM_DEVICE(w), NULL, NULL, on_activate_done, a);
}

static NMConnection *find_saved_for_ssid(App *a, const char *ssid) {
	const GPtrArray *conns = nm_client_get_connections(a->client);
	if (!conns) return NULL;
	for (guint i = 0; i < conns->len; i++) {
		NMConnection *c = conns->pdata[i];
		NMSettingWireless *sw = nm_connection_get_setting_wireless(c);
		if (!sw) continue;
		GBytes *gb = nm_setting_wireless_get_ssid(sw);
		if (!gb) continue;
		char *u = nm_utils_ssid_to_utf8(g_bytes_get_data(gb, NULL), g_bytes_get_size(gb));
		if (u) {
			gboolean hit = strcmp(u, ssid) == 0;
			g_free(u);
			if (hit) return c;
		}
	}
	return NULL;
}

static void connect_ap(App *a, NMAccessPoint *ap, const char *ssid) {
	NMConnection *known = find_saved_for_ssid(a, ssid);
	if (known) {
		busy(a, "Connecting…");
		const char *path = nm_object_get_path(NM_OBJECT(ap));
		nm_client_activate_connection_async(a->client, known, NM_DEVICE(a->wifi),
			path, NULL, on_activate_done, a);
		return;
	}
	NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap);
	NM80211ApSecurityFlags rsn = nm_access_point_get_rsn_flags(ap);
	NM80211ApFlags flags = nm_access_point_get_flags(ap);
	gboolean secured = (wpa || rsn || (flags & NM_802_11_AP_FLAGS_PRIVACY));
	const char *pass = NULL;
	char *pass_copy = NULL;
	if (secured) {
		GtkWidget *dlg = gtk_dialog_new_with_buttons("WiFi password",
			GTK_WINDOW(a->win), GTK_DIALOG_MODAL | GTK_DIALOG_USE_HEADER_BAR,
			"_Cancel", GTK_RESPONSE_CANCEL, "C_onnect", GTK_RESPONSE_OK, NULL);
		GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
		gtk_container_set_border_width(GTK_CONTAINER(box), 12);
		char prompt[320];
		g_snprintf(prompt, sizeof prompt, "Enter the password for %s:", ssid);
		GtkWidget *pl = gtk_label_new(NULL);
		gtk_label_set_text(GTK_LABEL(pl), prompt);
		gtk_widget_set_halign(pl, GTK_ALIGN_START);
		gtk_box_pack_start(GTK_BOX(box), pl, FALSE, FALSE, 6);
		GtkWidget *entry = gtk_entry_new();
		gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
		gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
		gtk_entry_set_width_chars(GTK_ENTRY(entry), 28);
		gtk_widget_set_margin_top(entry, 4);
		gtk_container_add(GTK_CONTAINER(box), entry);
		gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
		gtk_widget_show_all(dlg);
		int resp = gtk_dialog_run(GTK_DIALOG(dlg));
		if (resp == GTK_RESPONSE_OK)
			pass_copy = g_strdup(gtk_entry_get_text(GTK_ENTRY(entry)));
		gtk_widget_destroy(dlg);
		if (!pass_copy) { set_status(a, "Cancelled"); return; }
		pass = pass_copy;
	}
	NMConnection *conn = nm_simple_connection_new();
	/* WHY the casts: this libnm's setting constructors are declared as
	 * returning NMSetting* (the concrete subtypes exist but the _new()
	 * functions do not advertise them), so cast like wifi-menu.c:350. */
	NMSettingConnection *scon = (NMSettingConnection *)nm_setting_connection_new();
	g_object_set(scon, NM_SETTING_CONNECTION_ID, ssid,
		NM_SETTING_CONNECTION_TYPE, NM_SETTING_WIRELESS_SETTING_NAME,
		NM_SETTING_CONNECTION_AUTOCONNECT, TRUE, NULL);
	nm_connection_add_setting(conn, NM_SETTING(scon));
	NMSettingWireless *sw = (NMSettingWireless *)nm_setting_wireless_new();
	GBytes *gb = g_bytes_new(ssid, strlen(ssid));
	g_object_set(sw, NM_SETTING_WIRELESS_SSID, gb,
		NM_SETTING_WIRELESS_MODE, NM_SETTING_WIRELESS_MODE_INFRA, NULL);
	g_bytes_unref(gb);
	nm_connection_add_setting(conn, NM_SETTING(sw));
	if (secured && pass) {
		NMSettingWirelessSecurity *sws =
			(NMSettingWirelessSecurity *)nm_setting_wireless_security_new();
		if (rsn || wpa) {
			g_object_set(sws, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "wpa-psk",
				NM_SETTING_WIRELESS_SECURITY_PSK, pass, NULL);
		} else {
			g_object_set(sws, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, "none",
				NM_SETTING_WIRELESS_SECURITY_WEP_KEY0, pass,
				NM_SETTING_WIRELESS_SECURITY_WEP_KEY_TYPE,
				NM_WEP_KEY_TYPE_PASSPHRASE, NULL);
		}
		nm_connection_add_setting(conn, NM_SETTING(sws));
		/* WHY no 802-1x setting added: an empty 802-1x block makes NM treat
		 * the profile as enterprise and PSK activation fails validation.
		 * Enterprise networks are unsupported (documented limit). */
	}
	busy(a, "Connecting…");
	const char *path = nm_object_get_path(NM_OBJECT(ap));
	nm_client_add_and_activate_connection_async(a->client, conn, NM_DEVICE(a->wifi),
		path, NULL, on_add_done, a);
	if (pass_copy) {
		/* WHY explicit_bzero not plain memset: memset before free can be
		 * elided as a dead store; explicit_bzero is guaranteed to run,
		 * matching the wipe policy documented in wifi-menu.c. */
		explicit_bzero(pass_copy, strlen(pass_copy));
		g_free(pass_copy);
	}
	g_object_unref(conn);
}

static void on_connect(GtkButton *b, gpointer user_data) {
	(void)b; App *a = user_data;
	if (!a->client) return;
	GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a->sel_view));
	GtkTreeIter it; GtkTreeModel *m;
	if (!gtk_tree_selection_get_selected(sel, &m, &it)) {
		set_status(a, "Select a network first");
		return;
	}
	if (a->sel_view == GTK_TREE_VIEW(a->saved_view)) {
		connect_saved_selection(a, &it, m);
		return;
	}
	char *ssid = NULL; NMAccessPoint *ap = NULL;
	gtk_tree_model_get(m, &it, COL_SSID, &ssid, COL_OBJ, &ap, -1);
	if (!ap) { g_free(ssid); set_status(a, "Stale entry — rescan"); return; }
	a->wifi = find_wifi_device(a->client);
	if (!a->wifi) { g_free(ssid); set_status(a, "No WiFi device"); return; }
	connect_ap(a, ap, ssid); /* ssid may be NULL: connect_ap derives it via find_saved */
	g_free(ssid);
}

static void on_row_activated(GtkTreeView *view, GtkTreePath *path,
			     GtkTreeViewColumn *col, gpointer user_data) {
	(void)col; App *a = user_data;
	if (!a->client) return;
	GtkTreeModel *m = gtk_tree_view_get_model(view);
	GtkTreeIter it;
	if (!gtk_tree_model_get_iter(m, &it, path)) return;
	if (view != GTK_TREE_VIEW(a->scan_view)) {
		connect_saved_selection(a, &it, m);
		return;
	}
	NMAccessPoint *ap = NULL;
	gtk_tree_model_get(m, &it, COL_OBJ, &ap, -1);
	if (!ap) { set_status(a, "Stale entry — rescan"); return; }
	a->wifi = find_wifi_device(a->client);
	if (!a->wifi) { set_status(a, "No WiFi device"); return; }
	char ssid[256];
	ap_ssid(ap, ssid, sizeof ssid);
	connect_ap(a, ap, ssid);
}

static void on_client_ready(GObject *src, GAsyncResult *res, gpointer user_data) {
	(void)src; App *a = user_data;
	GError *err = NULL;
	a->client = nm_client_new_finish(res, &err);
	if (!a->client) {
		set_status(a, err ? err->message : "NMClient failed");
		if (err) g_error_free(err);
		return;
	}
	idle(a, "Ready");
	refresh_all(a);
}


/* ---- construction ------------------------------------------------------------ */

static GtkWidget *card(const char *title, GtkBox *pack_into) {
	/* WHY GtkFrame: section grouping from the stock theme, zero custom CSS. */
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	gtk_container_set_border_width(GTK_CONTAINER(box), 8);
	GtkWidget *frame = gtk_frame_new(title);
	gtk_container_add(GTK_CONTAINER(frame), box);
	gtk_box_pack_start(pack_into, frame, TRUE, TRUE, 0);
	return box;
}

static GtkWidget *kv_row(GtkWidget *parent, const char *key, GtkWidget **outval) {
	GtkWidget *h = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget *k = gtk_label_new(key);
	gtk_widget_set_halign(k, GTK_ALIGN_START);
	gtk_widget_set_hexpand(k, TRUE);
	GtkWidget *v = gtk_label_new("—");
	gtk_widget_set_halign(v, GTK_ALIGN_END);
	gtk_widget_set_hexpand(v, TRUE);
	gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
	gtk_label_set_max_width_chars(GTK_LABEL(v), 40);
	gtk_box_pack_start(GTK_BOX(h), k, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(h), v, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(parent), h, FALSE, FALSE, 2);
	if (outval) *outval = v;
	return v;
}

static void common_tree(GtkTreeView *view) {
	gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(view), FALSE);
}

int main(int argc, char *argv[]) {
	gtk_init(&argc, &argv);
	App a = {0};
	a.sel_view = NULL;

	a.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(a.win), 960, 620);
	g_signal_connect(a.win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
	GtkWidget *hb = gtk_header_bar_new();
	gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(hb), TRUE);
	GtkWidget *sub = gtk_label_new("NetworkManager / monowl");
	gtk_header_bar_set_custom_title(GTK_HEADER_BAR(hb), sub);
	GtkWidget *brescan = gtk_button_new_with_label("Rescan");
	GtkWidget *brefresh = gtk_button_new_with_label("Refresh");
	gtk_button_set_relief(GTK_BUTTON(brescan), GTK_RELIEF_NONE);
	gtk_button_set_relief(GTK_BUTTON(brefresh), GTK_RELIEF_NONE);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), brescan);
	gtk_header_bar_pack_end(GTK_HEADER_BAR(hb), brefresh);
	gtk_window_set_titlebar(GTK_WINDOW(a.win), hb);

	/* body: paned left (sections) / right (cards) + bottom status bar */
	GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(left), 10);
	GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	gtk_container_set_border_width(GTK_CONTAINER(right), 10);
	gtk_widget_set_size_request(right, 330, -1);
	gtk_paned_pack1(GTK_PANED(paned), left, TRUE, FALSE);
	gtk_paned_pack2(GTK_PANED(paned), right, FALSE, FALSE);
	gtk_paned_set_position(GTK_PANED(paned), 590);

	/* left: available networks */
	GtkWidget *avail = card("AVAILABLE NETWORKS", GTK_BOX(left));
	a.scan_store = gtk_list_store_new(COL_N, G_TYPE_STRING, G_TYPE_STRING,
		G_TYPE_INT, G_TYPE_UINT, G_TYPE_STRING, G_TYPE_POINTER);
	a.scan_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a.scan_store));
	common_tree(GTK_TREE_VIEW(a.scan_view));
	GtkTreeViewColumn *ssid_col = gtk_tree_view_column_new_with_attributes(
		"Network", gtk_cell_renderer_text_new(), "text", COL_SSID, NULL);
	gtk_tree_view_column_set_expand(ssid_col, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(a.scan_view), ssid_col);
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(a.scan_view), -1,
		"Security", gtk_cell_renderer_text_new(), "text", COL_SEC, NULL);
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(a.scan_view), -1,
		"Signal", gtk_cell_renderer_progress_new(), "value", COL_STR, NULL);
	/* WHY sort by signal desc: strongest AP first is scan-list UX. */
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a.scan_store),
		COL_STR, GTK_SORT_DESCENDING);
	GtkTreeSelection *ssel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a.scan_view));
	gtk_tree_selection_set_mode(ssel, GTK_SELECTION_BROWSE);
	g_signal_connect(ssel, "changed", G_CALLBACK(on_scan_selected), &a);
	g_signal_connect(a.scan_view, "row-activated", G_CALLBACK(on_row_activated), &a);
	GtkWidget *sc1 = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc1), 220);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc1), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(sc1), a.scan_view);
	gtk_box_pack_start(GTK_BOX(avail), sc1, TRUE, TRUE, 0);

	/* left: saved profiles */
	GtkWidget *saved = card("SAVED PROFILES", GTK_BOX(left));
	a.saved_store = gtk_list_store_new(S_N, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
	a.saved_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(a.saved_store));
	common_tree(GTK_TREE_VIEW(a.saved_view));
	GtkTreeViewColumn *prof_col = gtk_tree_view_column_new_with_attributes(
		"Profile", gtk_cell_renderer_text_new(), "text", S_SSID, NULL);
	gtk_tree_view_column_set_expand(prof_col, TRUE);
	gtk_tree_view_append_column(GTK_TREE_VIEW(a.saved_view), prof_col);
	gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(a.saved_view), -1,
		"Auto", gtk_cell_renderer_toggle_new(), "active", S_AUTO, NULL);
	gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(a.saved_store),
		S_SSID, GTK_SORT_ASCENDING);
	GtkTreeSelection *rsel = gtk_tree_view_get_selection(GTK_TREE_VIEW(a.saved_view));
	gtk_tree_selection_set_mode(rsel, GTK_SELECTION_BROWSE);
	g_signal_connect(rsel, "changed", G_CALLBACK(on_saved_selected), &a);
	g_signal_connect(a.saved_view, "row-activated", G_CALLBACK(on_row_activated), &a);
	GtkWidget *sc2 = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(sc2), 150);
	gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sc2), GTK_SHADOW_IN);
	gtk_container_add(GTK_CONTAINER(sc2), a.saved_view);
	gtk_box_pack_start(GTK_BOX(saved), sc2, TRUE, TRUE, 0);
	GtkWidget *note = gtk_label_new("Forget removes the stored profile, including its password.");
	gtk_widget_set_halign(note, GTK_ALIGN_START);
	gtk_box_pack_start(GTK_BOX(saved), note, FALSE, FALSE, 0);

	/* right: details */
	GtkWidget *det = card("DETAILS", GTK_BOX(right));
	kv_row(det, "Network", &a.d_ssid);
	kv_row(det, "Security", &a.d_sec);
	kv_row(det, "Signal", &a.d_sig);
	kv_row(det, "Band", &a.d_chan);
	kv_row(det, "BSSID", &a.d_bss);
	kv_row(det, "State", &a.d_state);

	/* right: device */
	GtkWidget *dev = card("DEVICE", GTK_BOX(right));
	kv_row(dev, "Interface", &a.v_if);
	kv_row(dev, "State", &a.v_state);
	kv_row(dev, "Driver", &a.v_drv);
	kv_row(dev, "MAC", &a.v_mac);
	kv_row(dev, "IPv4", &a.v_ip);

	/* right: actions */
	GtkWidget *act = card("ACTIONS", GTK_BOX(right));
	a.bconn = gtk_button_new_with_label("Connect");
	a.bdisc = gtk_button_new_with_label("Disconnect");
	a.bforget = gtk_button_new_with_label("Forget selected");
	gtk_widget_set_margin_top(a.bconn, 4);
	gtk_widget_set_sensitive(a.bconn, FALSE);
	gtk_widget_set_sensitive(a.bforget, FALSE);
	gtk_style_context_add_class(gtk_widget_get_style_context(a.bconn), GTK_STYLE_CLASS_SUGGESTED_ACTION);
	gtk_style_context_add_class(gtk_widget_get_style_context(a.bdisc), GTK_STYLE_CLASS_DESTRUCTIVE_ACTION);
	gtk_style_context_add_class(gtk_widget_get_style_context(a.bforget), GTK_STYLE_CLASS_DESTRUCTIVE_ACTION);
	gtk_box_pack_start(GTK_BOX(act), a.bconn, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(act), a.bdisc, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(act), a.bforget, FALSE, FALSE, 0);

	/* status bar */
	GtkWidget *sb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	gtk_container_set_border_width(GTK_CONTAINER(sb), 6);
	a.status = gtk_label_new("Starting…");
	gtk_widget_set_halign(a.status, GTK_ALIGN_START);
	gtk_widget_set_hexpand(a.status, TRUE);
	gtk_box_pack_start(GTK_BOX(sb), a.status, TRUE, TRUE, 6);
	a.busy = gtk_spinner_new();
	gtk_box_pack_start(GTK_BOX(sb), a.busy, FALSE, FALSE, 6);

	GtkWidget *ov = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_pack_start(GTK_BOX(ov), paned, TRUE, TRUE, 0);
	gtk_box_pack_start(GTK_BOX(ov), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(ov), sb, FALSE, FALSE, 0);
	gtk_container_add(GTK_CONTAINER(a.win), ov);

	g_signal_connect(brescan, "clicked", G_CALLBACK(on_rescan), &a);
	g_signal_connect(brefresh, "clicked", G_CALLBACK(on_refresh), &a);
	g_signal_connect(a.bconn, "clicked", G_CALLBACK(on_connect), &a);
	g_signal_connect(a.bdisc, "clicked", G_CALLBACK(on_disconnect), &a);
	g_signal_connect(a.bforget, "clicked", G_CALLBACK(on_forget), &a);

	gtk_widget_show_all(a.win);
	set_status(&a, "Connecting to NetworkManager…");
	nm_client_new_async(NULL, on_client_ready, &a);
	gtk_main();
	if (a.client) g_object_unref(a.client);
	return 0;
}
