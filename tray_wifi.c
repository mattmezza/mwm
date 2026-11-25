#include "tray_wifi.h"

#include <stdio.h>
#include <string.h>

enum Security {
	Open, Owe, Wep, Psk, Sae, Enterprise, Enterprise192
};

enum {
	ApPrivacy = 1, ApPsk = 0x100, ApEnterprise = 0x200, ApSae = 0x400,
	ApOwe = 0x800, ApOweTransition = 0x1000, ApEnterprise192 = 0x2000,
	NmDeviceWifi = 2, NmActiveActivated = 2, NmActiveDeactivated = 4,
};

typedef struct {
	GVariant *settings;
	guint64 version;
	gboolean loading;
} Profile;

typedef struct {
	char *id;
	guint64 version;
} ProfileLoad;

typedef struct {
	GBytes *ssid;
	char *ap, *device;
	enum Security security;
	gboolean hidden;
} Join;

typedef struct {
	char *device, *active_ap, *activating, *forget;
	guint scan_timer, activation_timer;
	gint64 last_scan;
	GHashTable *profiles;
	Join join;
} Wifi;

GVariant *
wifi_setting(GVariant *settings, const char *name)
{
	return settings ? g_variant_lookup_value(settings, name, G_VARIANT_TYPE_VARDICT) : NULL;
}

const char *
wifi_value(GVariant *setting, const char *key)
{
	const char *value = "";
	if (setting)
		g_variant_lookup(setting, key, "&s", &value);
	return value;
}

const char *
wifi_psk_key(const char *key_mgmt, guint index)
{
	static const char *wep[] = {"wep-key0", "wep-key1", "wep-key2", "wep-key3"};
	if (!strcmp(key_mgmt, "none"))
		return wep[MIN(index, 3)];
	if (!strcmp(key_mgmt, "ieee8021x"))
		return "leap-password";
	return "psk";
}

static GBytes *
bytes_of(GVariant *value)
{
	if (!value || !g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING))
		return NULL;
	gsize length;
	const void *bytes = g_variant_get_fixed_array(value, &length, 1);
	return length ? g_bytes_new(bytes, length) : NULL;
}

static GBytes *
ap_ssid(GDBusProxy *proxy)
{
	g_autoptr(GVariant) value = tray_property(proxy, "Ssid");
	return bytes_of(value);
}

static char *
ssid_name(GBytes *ssid)
{
	if (!ssid)
		return g_strdup("Hidden network");
	gsize length;
	const char *bytes = g_bytes_get_data(ssid, &length);
	return g_utf8_make_valid(bytes, length);
}

static guint
ap_modes(GDBusProxy *proxy)
{
	guint flags = tray_uint(proxy, "WpaFlags") | tray_uint(proxy, "RsnFlags");
	guint modes = 0;
	if (flags & ApPsk)
		modes |= 1u << Psk;
	if (flags & ApSae)
		modes |= 1u << Sae;
	if (flags & (ApOwe | ApOweTransition))
		modes |= 1u << Owe;
	if (flags & ApEnterprise)
		modes |= 1u << Enterprise;
	if (flags & ApEnterprise192)
		modes |= 1u << Enterprise192;
	return modes ? modes : 1u << (tray_uint(proxy, "Flags") & ApPrivacy ? Wep : Open);
}

static enum Security
preferred_security(guint modes)
{
	for (int mode = Enterprise192; mode > Open; --mode)
		if (modes & (1u << mode))
			return mode;
	return Open;
}

static enum Security
profile_security(GVariant *profile)
{
	g_autoptr(GVariant) security = wifi_setting(profile, WIFI_SECURITY);
	if (!security)
		return Open;
	const char *key_mgmt = wifi_value(security, "key-mgmt");
	if (!strcmp(key_mgmt, "wpa-psk"))
		return Psk;
	if (!strcmp(key_mgmt, "sae"))
		return Sae;
	if (!strcmp(key_mgmt, "owe"))
		return Owe;
	if (!strcmp(key_mgmt, "wpa-eap-suite-b-192"))
		return Enterprise192;
	if (!strcmp(key_mgmt, "wpa-eap") || !strcmp(key_mgmt, "ieee8021x"))
		return Enterprise;
	return Wep;
}

static void
free_profile(gpointer data)
{
	Profile *p = data;
	g_clear_pointer(&p->settings, g_variant_unref);
	g_free(p);
}

static void
profile_loaded(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)error;
	ProfileLoad *request = data;
	Wifi *wf = tray->backend;
	Profile *p = g_hash_table_lookup(wf->profiles, request->id);
	if (p && p->loading && p->version == request->version) {
		p->loading = FALSE;
		if (reply)
			p->settings = g_variant_get_child_value(reply, 0);
	}
	g_free(request->id);
	g_free(request);
	/* A denied profile has no useful settings. Wait for its next revision. */
	if (reply)
		tray_refresh(tray);
}

static void
load_profile(Tray *tray, GDBusProxy *proxy, const char *path)
{
	Wifi *wf = tray->backend;
	g_autoptr(GVariant) prop = tray_property(proxy, "VersionId");
	guint64 version = prop && g_variant_is_of_type(prop, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(prop) : 0;
	Profile *p = g_hash_table_lookup(wf->profiles, path);
	if (p && p->version == version)
		return;
	p = g_new0(Profile, 1);
	p->version = version;
	p->loading = TRUE;
	g_hash_table_replace(wf->profiles, g_strdup(path), p);
	ProfileLoad *request = g_new(ProfileLoad, 1);
	*request = (ProfileLoad){g_strdup(path), version};
	tray_call(tray, path, NM_PROFILE, "GetSettings", NULL, 8000, profile_loaded, request);
}

static char *
find_profile(Tray *tray, GBytes *ssid, guint modes)
{
	Wifi *wf = tray->backend;
	g_autoptr(GDBusProxy) device = tray_proxy(tray, wf->device, NM_DEVICE);
	g_autoptr(GVariant) available = tray_property(device, "AvailableConnections");
	const char *path, *best = NULL;
	guint64 newest = 0;
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, wf->profiles);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		path = key;
		Profile *profile = value;
		g_autoptr(GVariant) wifi = wifi_setting(profile->settings, WIFI_SETTING);
		if (!wifi || !(modes & (1u << profile_security(profile->settings))))
			continue;
		g_autoptr(GVariant) property = g_variant_lookup_value(wifi, "ssid", G_VARIANT_TYPE_BYTESTRING);
		g_autoptr(GBytes) saved = bytes_of(property);
		if (!saved || !g_bytes_equal(ssid, saved))
			continue;
		g_autoptr(GVariant) connection = wifi_setting(profile->settings, "connection");
		guint64 stamp = 0;
		if (connection)
			g_variant_lookup(connection, "timestamp", "t", &stamp);
		/* An interface-bound profile should never be offered on the wrong radio. */
		const char *bound = wifi_value(connection, "interface-name");
		g_autofree char *iface = tray_string(device, "Interface");
		if (*bound && g_strcmp0(bound, iface))
			continue;
		gboolean listed = FALSE;
		if (available) {
			GVariantIter paths;
			const char *candidate;
			g_variant_iter_init(&paths, available);
			while (g_variant_iter_next(&paths, "&o", &candidate))
				listed |= !strcmp(candidate, path);
		}
		if (available && !listed)
			continue;
		if (!best || stamp > newest || (stamp == newest && strcmp(path, best) < 0)) {
			best = path;
			newest = stamp;
		}
	}
	return g_strdup(best);
}

static char *
active_for(Tray *tray, const char *profile)
{
	g_autoptr(GDBusProxy) nm = tray_proxy(tray, NM_PATH, NM);
	g_autoptr(GVariant) connections = tray_property(nm, "ActiveConnections");
	if (!connections || !g_variant_is_of_type(connections, G_VARIANT_TYPE_OBJECT_PATH_ARRAY))
		return NULL;
	GVariantIter items;
	const char *path;
	g_variant_iter_init(&items, connections);
	while (g_variant_iter_next(&items, "&o", &path)) {
		g_autoptr(GDBusProxy) proxy = tray_proxy(tray, path, NM_ACTIVE);
		g_autofree char *current = tray_string(proxy, "Connection");
		if (!g_strcmp0(profile, current) && tray_uint(proxy, "State") < NmActiveDeactivated)
			return g_strdup(path);
	}
	return NULL;
}

static void
finish_activation(Tray *tray, gboolean failed, const char *message)
{
	Wifi *wf = tray->backend;
	if (wf->activation_timer) {
		g_source_remove(wf->activation_timer);
		wf->activation_timer = 0;
	}
	g_clear_pointer(&wf->activating, g_free);
	tray->busy = FALSE;
	tray_status(tray, failed, message);
}

static gboolean
activation_timeout(gpointer data)
{
	Tray *tray = data;
	Wifi *wf = tray->backend;
	wf->activation_timer = 0;
	finish_activation(tray, TRUE, "Connection timed out");
	return G_SOURCE_REMOVE;
}

static void
wifi_result(Tray *tray, GVariant *reply, GError *error, gpointer context)
{
	(void)reply;
	tray->busy = FALSE;
	if (error)
		tray_error(tray, error, context);
	else
		tray_status(tray, FALSE, "");
	tray_refresh(tray);
}

static void
activation_started(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	Wifi *wf = tray->backend;
	if (!reply) {
		wifi_result(tray, reply, error, "Could not start the connection");
		return;
	}
	const char *path;
	g_variant_get_child(reply, GPOINTER_TO_INT(data), "&o", &path);
	g_free(wf->activating);
	wf->activating = g_strdup(path);
	wf->activation_timer = g_timeout_add_seconds(120, activation_timeout, tray);
	tray_refresh(tray);
}

static void
activation_state(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
                 GDBusProxy *interface, const char *sender, const char *signal,
                 GVariant *parameters, gpointer data)
{
	(void)manager;
	(void)object;
	(void)sender;
	Tray *tray = data;
	Wifi *wf = tray->backend;
	if (!wf->activating || strcmp(g_dbus_proxy_get_object_path(interface), wf->activating) ||
	    strcmp(g_dbus_proxy_get_interface_name(interface), NM_ACTIVE) || strcmp(signal, "StateChanged"))
		return;
	guint state, reason;
	g_variant_get(parameters, "(uu)", &state, &reason);
	if (state == NmActiveDeactivated) {
		const char *message = reason == 9 ? "Credentials were missing or declined" :
		                      reason == 10 ? "Sign-in failed. Check the credentials" :
		                      reason == 6 || reason == 7 ? "The connection timed out" : "The connection could not be established";
		finish_activation(tray, TRUE, message);
	}
}

static void
active_removed(GDBusObjectManager *manager, GDBusObject *object, gpointer data)
{
	(void)manager;
	Tray *tray = data;
	Wifi *wf = tray->backend;
	if (wf->activating && !strcmp(wf->activating, g_dbus_object_get_object_path(object)))
		finish_activation(tray, TRUE, "The connection ended before it was ready");
}

void
wifi_owner(Tray *tray)
{
	Wifi *wf = tray->backend;
	if (!wf) {
		tray->backend = wf = g_new0(Wifi, 1);
		wf->profiles = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, free_profile);
		g_signal_connect(tray->manager, "interface-proxy-signal", G_CALLBACK(activation_state), tray);
		g_signal_connect(tray->manager, "object-removed", G_CALLBACK(active_removed), tray);
	}
	g_hash_table_remove_all(wf->profiles);
	wf->last_scan = 0;
	if (wf->scan_timer) {
		g_source_remove(wf->scan_timer);
		wf->scan_timer = 0;
	}
	if (wf->activating)
		finish_activation(tray, TRUE, "NetworkManager restarted");
	wifi_secrets_owner(tray);
}

void
wifi_configure(Tray *tray)
{
	if (tray->backend) {
		wifi_secrets_configure(tray);
		tray_refresh(tray);
	}
}

static gboolean
scan_finished(gpointer data)
{
	Tray *tray = data;
	Wifi *wf = tray->backend;
	wf->scan_timer = 0;
	tray_refresh(tray);
	return G_SOURCE_REMOVE;
}

static void
scan_result(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)reply;
	if (error) {
		Wifi *wf = tray->backend;
		if (wf->scan_timer)
			g_source_remove(wf->scan_timer);
		scan_finished(tray);
		if (data)
			tray_error(tray, error, "Could not scan for networks");
	}
}

static void
scan(Tray *tray, gboolean manual)
{
	Wifi *wf = tray->backend;
	if (!tray->can_scan || !wf->device || (!manual && shell_seconds()*1000-wf->last_scan < 25000))
		return;
	wf->scan_timer = g_timeout_add_seconds(12, scan_finished, tray);
	GVariantBuilder options;
	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	tray_call(tray, wf->device, NM_WIFI, "RequestScan", g_variant_new("(a{sv})", &options),
	          8000, scan_result, GINT_TO_POINTER(manual));
	tray_refresh(tray);
}

void
wifi_show(Tray *tray)
{
	if (tray->backend)
		scan(tray, FALSE);
}

static gint
row_order(gconstpointer left, gconstpointer right)
{
	const TrayEntry *a = *(TrayEntry *const *)left, *b = *(TrayEntry *const *)right;
	if (a->active != b->active)
		return b->active - a->active;
	if (a->saved != b->saved)
		return b->saved - a->saved;
	if (a->strength != b->strength)
		return b->strength - a->strength;
	return g_utf8_collate(a->label, b->label);
}

static void
access_points(Tray *tray, GDBusProxy *device)
{
	Wifi *wf = tray->backend;
	g_autoptr(GVariant) aps = tray_property(device, "AccessPoints");
	if (!aps)
		return;
	GVariantIter paths;
	const char *path;
	g_autoptr(GHashTable) seen = g_hash_table_new_full(g_bytes_hash, g_bytes_equal,
	                                                (GDestroyNotify)g_bytes_unref, NULL);
	g_variant_iter_init(&paths, aps);
	while (g_variant_iter_next(&paths, "&o", &path)) {
		g_autoptr(GDBusProxy) ap = tray_proxy(tray, path, NM_AP);
		g_autoptr(GBytes) ssid = ap_ssid(ap);
		if (!ssid)
			continue;
		gboolean active = !g_strcmp0(path, wf->active_ap);
		guint modes = ap_modes(ap);
		enum Security security = preferred_security(modes);
		int strength = tray_uint(ap, "Strength");
		g_autofree char *profile = find_profile(tray, ssid, modes);
		g_autofree char *name = ssid_name(ssid);
		char detail[100];
		const char *kind = security == Open ? "Open" : security == Owe ? "Enhanced open"
		                   : security == Enterprise || security == Enterprise192 ? "Secured · Enterprise" : "Secured";
		snprintf(detail, sizeof detail, "%s%s  ·  %d%%", active ? "Connected · " : profile ? "Saved · " : "", kind, strength);
		/* Same SSID and security can be advertised by several radios. */
		gsize length;
		const char *raw = g_bytes_get_data(ssid, &length);
		char key[33];
		if (length > 32)
			continue;
		memcpy(key, raw, length);
		key[length] = (char)security;
		g_autoptr(GBytes) identity = g_bytes_new(key, length+1);
		TrayEntry *previous = g_hash_table_lookup(seen, identity);
		if (!previous) {
			TrayEntry *row = tray_entry(tray, path, name, detail, TraySignal, active, profile != NULL, strength);
			g_hash_table_insert(seen, g_bytes_ref(identity), row);
		} else if (!previous->active && (active || strength > previous->strength)) {
			g_free(previous->id);
			g_free(previous->detail);
			previous->id = g_strdup(path);
			previous->detail = g_strdup(detail);
			previous->active = active;
			previous->saved = profile != NULL;
			previous->strength = strength;
		}
		if (active)
			tray->strength = strength;
	}
}

void
wifi_model(Tray *tray)
{
	Wifi *wf = tray->backend;
	if (!tray->connected || !wf) {
		tray_model(tray, "Wi-Fi unavailable", "NetworkManager is offline");
		return;
	}
	g_autoptr(GDBusProxy) nm = tray_proxy(tray, NM_PATH, NM);
	tray->enabled = tray_bool(nm, "WirelessEnabled");
	g_clear_pointer(&wf->device, g_free);
	g_clear_pointer(&wf->active_ap, g_free);
	g_autoptr(GDBusProxy) wifi = NULL;
	int best = -1;
	GList *objects = g_dbus_object_manager_get_objects(G_DBUS_OBJECT_MANAGER(tray->manager));
	for (GList *it = objects; it; it = it->next) {
		const char *path = g_dbus_object_get_object_path(it->data);
		g_autoptr(GDBusProxy) profile = tray_proxy(tray, path, NM_PROFILE);
		if (profile)
			load_profile(tray, profile, path);
		g_autoptr(GDBusProxy) candidate = tray_proxy(tray, path, NM_WIFI);
		if (!candidate)
			continue;
		g_autoptr(GDBusProxy) device = tray_proxy(tray, path, NM_DEVICE);
		g_autofree char *name = tray_string(device, "Interface");
		if (tray_uint(device, "DeviceType") != NmDeviceWifi ||
		    (*tray->config.wifi_interface && g_strcmp0(tray->config.wifi_interface, name)))
			continue;
		g_autofree char *ap = tray_string(candidate, "ActiveAccessPoint");
		g_autofree char *active_connection = tray_string(device, "ActiveConnection");
		g_autofree char *primary = tray_string(nm, "PrimaryConnection");
		int score = ap && strcmp(ap, "/") ? (!g_strcmp0(active_connection, primary) ? 3 : 2)
		            : tray_bool(device, "Managed") ? 1 : 0;
		if (score > best || (score == best && strcmp(path, wf->device) < 0)) {
			best = score;
			g_set_object(&wifi, candidate);
			g_free(wf->device);
			wf->device = g_strdup(path);
			g_free(wf->active_ap);
			wf->active_ap = g_steal_pointer(&ap);
		}
	}
	g_list_free_full(objects, g_object_unref);
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, wf->profiles);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		const char *path = key;
		g_autoptr(GDBusProxy) still_present = tray_proxy(tray, path, NM_PROFILE);
		if (!still_present) {
			g_hash_table_iter_remove(&iter);
			continue;
		}
		Profile *profile = value;
		g_autoptr(GVariant) connection = wifi_setting(profile->settings, "connection");
		const char *type = wifi_value(connection, "type");
		if (!strcmp(type, "vpn") || !strcmp(type, "wireguard")) {
			g_autofree char *active = active_for(tray, path);
			tray_entry(tray, path, wifi_value(connection, "id"), active ? "Connected" :
			           !strcmp(type, "wireguard") ? "WireGuard" : "VPN profile", TrayVpn, active != NULL, TRUE, -1);
		}
	}
	tray->available = wifi != NULL && tray_bool(nm, "WirelessHardwareEnabled");
	g_autoptr(GVariant) scan_property = tray_property(wifi, "LastScan");
	gint64 last = scan_property && g_variant_is_of_type(scan_property, G_VARIANT_TYPE_INT64) ?
	              g_variant_get_int64(scan_property) : 0;
	if (last != wf->last_scan) {
		wf->last_scan = last;
		if (wf->scan_timer) {
			g_source_remove(wf->scan_timer);
			wf->scan_timer = 0;
		}
	}
	tray->scanning = wf->scan_timer > 0;
	tray->can_scan = tray->available && tray->enabled && !tray->scanning;
	if (wifi && tray->enabled)
		access_points(tray, wifi);
	g_ptr_array_sort(tray->rows, row_order);
	g_autoptr(GDBusProxy) active = tray_proxy(tray, wf->active_ap, NM_AP);
	g_autoptr(GBytes) ssid = ap_ssid(active);
	g_autofree char *network = ssid_name(ssid);
	guint connectivity = tray_uint(nm, "Connectivity");
	const char *detail = connectivity == 2 ? "Sign-in required · Captive portal" : connectivity == 3 ?
	                     "Connected · Limited internet" : "Connected";
	tray_model(tray, !wifi ? "No Wi-Fi adapter" : !tray->available ? "Wi-Fi hardware blocked" : !tray->enabled ? "Wi-Fi off" :
	           active ? network : "Wi-Fi on",
	           !wifi ? "Saved VPN profiles are available over any connection" : !tray->available ? "Enable the radio in hardware" :
	           !tray->enabled ? "Turn on to see nearby networks" : active ? detail :
	           tray->scanning ? "Looking for nearby networks…" : "Ready to connect");
	if (wf->activating) {
		g_autoptr(GDBusProxy) connection = tray_proxy(tray, wf->activating, NM_ACTIVE);
		guint state = tray_uint(connection, "State");
		if (state == NmActiveActivated || state == NmActiveDeactivated)
			finish_activation(tray, state == NmActiveDeactivated, state == NmActiveActivated ? "" : "The connection failed");
	}
}

static void
join_reset(Join *join)
{
	g_clear_pointer(&join->ssid, g_bytes_unref);
	g_free(join->ap);
	g_free(join->device);
	*join = (Join){0};
}

static void
put(GVariantBuilder *b, const char *key, const char *value)
{
	g_variant_builder_add(b, "{sv}", key, g_variant_new_string(value));
}

static void
put_cert(GVariantBuilder *b, const char *key, const char *path)
{
	g_autofree char *uri = g_strconcat("file://", path, NULL);
	g_variant_builder_add(b, "{sv}", key, g_variant_new_bytestring(uri));
}

static void
connect_new(Tray *tray, const char *password, const TrayPrompt *enterprise)
{
	Wifi *wf = tray->backend;
	Join *j = &wf->join;
	GVariantBuilder settings, props, options;
	g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	g_autofree char *name = ssid_name(j->ssid);
	put(&props, "id", name);
	put(&props, "type", WIFI_SETTING);
	g_variant_builder_add(&settings, "{sa{sv}}", "connection", &props);
	g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
	gsize length;
	const void *bytes = g_bytes_get_data(j->ssid, &length);
	g_variant_builder_add(&props, "{sv}", "ssid", g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, bytes, length, 1));
	put(&props, "mode", "infrastructure");
	if (j->hidden)
		g_variant_builder_add(&props, "{sv}", "hidden", g_variant_new_boolean(TRUE));
	g_variant_builder_add(&settings, "{sa{sv}}", WIFI_SETTING, &props);
	if (j->security != Open) {
		const char *modes[] = {"", "owe", "none", "wpa-psk", "sae", "wpa-eap", "wpa-eap-suite-b-192"};
		g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
		put(&props, "key-mgmt", modes[j->security]);
		if (j->security == Psk || j->security == Sae)
			put(&props, "psk", password);
		if (j->security == Wep) {
			put(&props, "wep-key0", password);
			g_variant_builder_add(&props, "{sv}", "wep-key-type", g_variant_new_uint32(1));
		}
		g_variant_builder_add(&settings, "{sa{sv}}", WIFI_SECURITY, &props);
	}
	if (enterprise) {
		const TrayField *f = enterprise->fields;
		const char *eap[] = {f[0].value, NULL};
		gboolean tls = !strcmp(eap[0], "tls"), pwd = !strcmp(eap[0], "pwd");
		g_variant_builder_init(&props, G_VARIANT_TYPE_VARDICT);
		g_variant_builder_add(&props, "{sv}", "eap", g_variant_new_strv(eap, 1));
		put(&props, "identity", f[1].value);
		if (!pwd) {
			put(&props, "domain-suffix-match", f[3].value);
			if (*f[4].value)
				put_cert(&props, "ca-cert", f[4].value);
			else
				g_variant_builder_add(&props, "{sv}", "system-ca-certs", g_variant_new_boolean(TRUE));
		}
		if (tls) {
			put_cert(&props, "client-cert", f[5].value);
			put_cert(&props, "private-key", f[6].value);
			if (*f[2].value)
				put(&props, "private-key-password", f[2].value);
		} else {
			put(&props, "password", f[2].value);
			if (!pwd) {
				const char *inner = *f[7].value ? f[7].value : !strcmp(eap[0], "peap") ? "mschapv2" : "pap";
				if (g_str_has_prefix(inner, "eap-"))
					put(&props, "phase2-autheap", inner+4);
				else
					put(&props, "phase2-auth", inner);
			}
		}
		g_variant_builder_add(&settings, "{sa{sv}}", "802-1x", &props);
	}
	g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
	tray->busy = TRUE;
	tray_status(tray, FALSE, "Connecting…");
	tray_call(tray, NM_PATH, NM, "AddAndActivateConnection2",
	          g_variant_new("(a{sa{sv}}ooa{sv})", &settings, j->device, j->ap, &options),
	          30000, activation_started, GINT_TO_POINTER(1));
	join_reset(j);
	tray_clear_prompt(tray);
}

static gboolean
is_hex(const char *text)
{
	for (; *text; ++text)
		if (!g_ascii_isxdigit(*text))
			return FALSE;
	return TRUE;
}

static gboolean
file_path(const char *path)
{
	return g_path_is_absolute(path) && g_file_test(path, G_FILE_TEST_IS_REGULAR);
}

static void
answer_join(Tray *tray, gboolean accepted)
{
	Wifi *wf = tray->backend;
	if (!accepted) {
		join_reset(&wf->join);
		tray_clear_prompt(tray);
		return;
	}
	TrayField *f = tray->prompt.fields;
	enum Security security = wf->join.security;
	const char *secret = f[0].value;
	size_t length = strlen(secret);
	if (security == Psk && ((length < 8 || length > 63) && !(length == 64 && is_hex(secret)))) {
		tray_status(tray, TRUE, "Use 8–63 characters or a 64-digit hex key");
		return;
	}
	if (security == Wep && length != 5 && length != 13 && !((length == 10 || length == 26) && is_hex(secret))) {
		tray_status(tray, TRUE, "Use a 5- or 13-character key, or 10 or 26 hex digits");
		return;
	}
	if (security == Sae && length > 63) {
		tray_status(tray, TRUE, "Use a WPA3 passphrase of up to 63 characters");
		return;
	}
	if (security == Enterprise || security == Enterprise192) {
		const char *method = f[0].value;
		gboolean tls = !strcmp(method, "tls"), pwd = !strcmp(method, "pwd");
		if ((!tls && !pwd && strcmp(method, "peap") && strcmp(method, "ttls")) ||
		    (security == Enterprise192 && !tls)) {
			tray_status(tray, TRUE, security == Enterprise192 ? "WPA3 192-bit networks require EAP-TLS" :
			            "Choose peap, ttls, tls, or pwd");
			return;
		}
		if (!pwd && !*f[3].value) {
			tray_status(tray, TRUE, "Enter the expected authentication server domain");
			return;
		}
		if ((!pwd && *f[4].value && !file_path(f[4].value)) ||
		    (tls && (!file_path(f[5].value) || !file_path(f[6].value)))) {
			tray_status(tray, TRUE, "Use absolute paths to the certificate and key files");
			return;
		}
		if (!tls && !*f[2].value) {
			tray_status(tray, TRUE, "Enter your enterprise password");
			return;
		}
		connect_new(tray, NULL, &tray->prompt);
	} else {
		connect_new(tray, secret, NULL);
	}
}

static void
join_prompt(Tray *tray)
{
	Wifi *wf = tray->backend;
	Join *j = &wf->join;
	if (j->security == Open || j->security == Owe) {
		connect_new(tray, NULL, NULL);
		return;
	}
	g_autofree char *network = ssid_name(j->ssid);
	gboolean enterprise = j->security == Enterprise || j->security == Enterprise192;
	tray_prompt(tray, enterprise ? "Enterprise Wi-Fi" : "Wi-Fi password", network, "Connect", answer_join);
	if (!enterprise) {
		tray_field(tray, "psk", j->security == Wep ? "WEP key" : "Passphrase", "", TRUE, FALSE, TRUE);
		return;
	}
	tray_field(tray, "eap", "EAP method", j->security == Enterprise192 ? "tls" : "peap", FALSE, FALSE, TRUE);
	tray_field(tray, "identity", "Identity", "", FALSE, FALSE, TRUE);
	tray_field(tray, "password", "Password or private key password", "", TRUE, FALSE, FALSE);
	tray_field(tray, "domain", "Authentication server domain", "", FALSE, FALSE, FALSE);
	tray_field(tray, "ca", "CA certificate path (blank uses system CAs)", "", FALSE, FALSE, FALSE);
	tray_field(tray, "cert", "Client certificate path (TLS)", "", FALSE, FALSE, FALSE);
	tray_field(tray, "key", "Private key path (TLS)", "", FALSE, FALSE, FALSE);
	tray_field(tray, "inner", "Inner authentication (PEAP or TTLS)", "", FALSE, FALSE, FALSE);
}

static void
answer_hidden(Tray *tray, gboolean accepted)
{
	Wifi *wf = tray->backend;
	if (!accepted) {
		tray_clear_prompt(tray);
		return;
	}
	const char *ssid = tray->prompt.fields[0].value, *security = tray->prompt.fields[1].value;
	const char *names[] = {"open", "owe", "wep", "wpa2", "wpa3", "enterprise", "enterprise192"};
	int mode = 0;
	while (mode < (int)G_N_ELEMENTS(names) && g_ascii_strcasecmp(security, names[mode]))
		++mode;
	if (strlen(ssid) > 32 || mode == (int)G_N_ELEMENTS(names)) {
		tray_status(tray, TRUE, strlen(ssid) > 32 ? "The network name can use up to 32 bytes" :
		            "Use open, owe, wep, wpa2, wpa3, or enterprise");
		return;
	}
	g_autoptr(GBytes) name = g_bytes_new(ssid, strlen(ssid));
	tray_clear_prompt(tray);
	join_reset(&wf->join);
	wf->join = (Join){.ssid = g_bytes_ref(name), .device = g_strdup(wf->device), .ap = g_strdup("/"),
	                 .security = mode, .hidden = TRUE};
	join_prompt(tray);
}

static void
activate(Tray *tray, const char *profile, const char *device, const char *ap)
{
	tray->busy = TRUE;
	tray_status(tray, FALSE, "Connecting…");
	tray_call(tray, NM_PATH, NM, "ActivateConnection", g_variant_new("(ooo)", profile, device, ap),
	          30000, activation_started, GINT_TO_POINTER(0));
}

static void
deactivate(Tray *tray, const char *path)
{
	tray->busy = TRUE;
	tray_status(tray, FALSE, "Disconnecting…");
	tray_call(tray, NM_PATH, NM, "DeactivateConnection", g_variant_new("(o)", path),
	          15000, wifi_result, "Could not disconnect");
}

static void
answer_remove(Tray *tray, gboolean accepted)
{
	Wifi *wf = tray->backend;
	g_autofree char *profile = g_steal_pointer(&wf->forget);
	tray_clear_prompt(tray);
	if (accepted && profile) {
		tray->busy = TRUE;
		tray_call(tray, profile, NM_PROFILE, "Delete", NULL, 15000, wifi_result, "Could not forget the network");
	}
}

void
wifi_action(Tray *tray, enum TrayAction action, const char *id)
{
	Wifi *wf = tray->backend;
	if (!wf || !tray->connected)
		return;
	if (action == TrayToggle && tray->available) {
		tray->busy = TRUE;
		tray_set(tray, NM_PATH, NM, "WirelessEnabled", g_variant_new_boolean(!tray->enabled),
		         wifi_result, "Could not change Wi-Fi power");
		return;
	}
	if (action == TrayScan) {
		scan(tray, TRUE);
		return;
	}
	if (action == TrayHidden && tray->available && tray->enabled) {
		tray_prompt(tray, "Join hidden network", "Enter the network name and security type", "Continue", answer_hidden);
		tray_field(tray, "ssid", "Network name", "", FALSE, FALSE, TRUE);
		tray_field(tray, "security", "Security (open, owe, wep, wpa2, wpa3, enterprise)", "wpa2", FALSE, FALSE, TRUE);
		return;
	}
	if (!id || (action != TrayRow && action != TrayForget))
		return;
	Profile *profile = g_hash_table_lookup(wf->profiles, id);
	if (profile) {
		if (action == TrayRow) {
			g_autofree char *active = active_for(tray, id);
			if (active)
				deactivate(tray, active);
			else
				activate(tray, id, "/", "/");
		}
		return;
	}
	if (!tray->enabled || !wf->device)
		return;
	g_autoptr(GDBusProxy) ap = tray_proxy(tray, id, NM_AP);
	g_autoptr(GBytes) ssid = ap_ssid(ap);
	if (!ssid) {
		tray_status(tray, TRUE, "The network is no longer visible");
		return;
	}
	guint modes = ap_modes(ap);
	g_autofree char *saved = find_profile(tray, ssid, modes);
	if (action == TrayForget && saved) {
		g_autofree char *name = ssid_name(ssid);
		tray_prompt(tray, "Forget this network?", name, "Forget", answer_remove);
		wf->forget = g_steal_pointer(&saved);
	} else if (action == TrayRow && !g_strcmp0(id, wf->active_ap)) {
		tray->busy = TRUE;
		tray_status(tray, FALSE, "Disconnecting…");
		tray_call(tray, wf->device, NM_DEVICE, "Disconnect", NULL, 15000, wifi_result, "Could not disconnect");
	} else if (action == TrayRow && saved) {
		activate(tray, saved, wf->device, id);
	} else if (action == TrayRow) {
		join_reset(&wf->join);
		wf->join = (Join){.ssid = g_bytes_ref(ssid), .device = g_strdup(wf->device), .ap = g_strdup(id),
		                  .security = preferred_security(modes)};
		join_prompt(tray);
	}
}
