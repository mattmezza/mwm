#include "tray.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ADAPTER "org.bluez.Adapter1"
#define DEVICE "org.bluez.Device1"
#define AGENT "org.bluez.Agent1"
#define AGENT_MANAGER "org.bluez.AgentManager1"
#define AGENT_PATH "/org/mwm/Tray/Bluetooth"

static const char agent_xml[] =
	"<node><interface name='" AGENT "'>"
	"<method name='Release'/>"
	"<method name='RequestPinCode'><arg type='o' direction='in'/><arg type='s' direction='out'/></method>"
	"<method name='DisplayPinCode'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='RequestPasskey'><arg type='o' direction='in'/><arg type='u' direction='out'/></method>"
	"<method name='DisplayPasskey'><arg type='o' direction='in'/><arg type='u' direction='in'/><arg type='q' direction='in'/></method>"
	"<method name='RequestConfirmation'><arg type='o' direction='in'/><arg type='u' direction='in'/></method>"
	"<method name='RequestAuthorization'><arg type='o' direction='in'/></method>"
	"<method name='AuthorizeService'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='Cancel'/>"
	"</interface></node>";

typedef struct {
	char *adapter, *discovery, *device, *pairing, *prompt_device;
	GDBusMethodInvocation *request;
	guint agent, scan_timeout;
	gboolean registered, default_agent, discovery_pending;
} Bluetooth;

static void
bt_result(Tray *tray, GVariant *reply, GError *error, gpointer context)
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
stop_scan(Tray *tray)
{
	Bluetooth *bt = tray->backend;
	g_autofree char *path = g_steal_pointer(&bt->discovery);
	if (bt->scan_timeout) {
		g_source_remove(bt->scan_timeout);
		bt->scan_timeout = 0;
	}
	if (path)
		tray_call(tray, path, ADAPTER, "StopDiscovery", NULL, 8000, NULL, NULL);
	tray_refresh(tray);
}

static gboolean
scan_timeout(gpointer data)
{
	Tray *tray = data;
	Bluetooth *bt = tray->backend;
	bt->scan_timeout = 0;
	stop_scan(tray);
	return G_SOURCE_REMOVE;
}

static void
scan_started(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)reply;
	Bluetooth *bt = tray->backend;
	g_autofree char *adapter = data;
	bt->discovery_pending = FALSE;
	if (error)
		tray_error(tray, error, "Could not scan for devices");
	else if (g_strcmp0(adapter, bt->adapter))
		tray_call(tray, adapter, ADAPTER, "StopDiscovery", NULL, 8000, NULL, NULL);
	else {
		bt->discovery = g_steal_pointer(&adapter);
		bt->scan_timeout = g_timeout_add_seconds(30, scan_timeout, tray);
	}
	tray_refresh(tray);
}

static void
answer_pairing(Tray *tray, gboolean accepted)
{
	Bluetooth *bt = tray->backend;
	GDBusMethodInvocation *request = bt->request;
	if (accepted && request && tray->prompt.count) {
		const char *value = tray->prompt.fields[0].value;
		const char *method = g_dbus_method_invocation_get_method_name(request);
		if (!*value || strlen(value) > (!strcmp(method, "RequestPinCode") ? 16u : 6u)) {
			tray_status(tray, TRUE, "Enter the code shown by the device");
			return;
		}
		if (!strcmp(method, "RequestPinCode"))
			g_dbus_method_invocation_return_value(request, g_variant_new("(s)", value));
		else
			g_dbus_method_invocation_return_value(request, g_variant_new("(u)", (guint32)strtoul(value, NULL, 10)));
	} else if (request) {
		if (accepted)
			g_dbus_method_invocation_return_value(request, NULL);
		else
			g_dbus_method_invocation_return_dbus_error(request, "org.bluez.Error.Rejected", "Pairing declined");
	} else if (!accepted && bt->pairing) {
		tray_call(tray, bt->pairing, DEVICE, "CancelPairing", NULL, 5000, NULL, NULL);
	}
	g_clear_object(&bt->request);
	g_clear_pointer(&bt->prompt_device, g_free);
	tray_clear_prompt(tray);
}

static void
cancel_agent(Tray *tray)
{
	Bluetooth *bt = tray->backend;
	if (bt->request)
		g_dbus_method_invocation_return_dbus_error(bt->request, "org.bluez.Error.Canceled", "Pairing canceled");
	g_clear_object(&bt->request);
	g_clear_pointer(&bt->prompt_device, g_free);
	if (tray->answer == answer_pairing)
		tray_clear_prompt(tray);
}

static void
pairing_request(GDBusConnection *connection, const char *sender, const char *path,
                const char *interface, const char *method, GVariant *parameters,
                GDBusMethodInvocation *request, gpointer data)
{
	(void)connection;
	(void)path;
	(void)interface;
	Tray *tray = data;
	Bluetooth *bt = tray->backend;
	if (g_strcmp0(sender, tray->owner)) {
		g_dbus_method_invocation_return_dbus_error(request, "org.freedesktop.DBus.Error.AccessDenied",
		                                           "Only BlueZ can use this agent");
		return;
	}
	if (!strcmp(method, "Release") || !strcmp(method, "Cancel")) {
		cancel_agent(tray);
		if (!strcmp(method, "Release"))
			bt->registered = FALSE;
		g_dbus_method_invocation_return_value(request, NULL);
		return;
	}
	const char *device;
	g_variant_get_child(parameters, 0, "&o", &device);
	g_autoptr(GDBusProxy) proxy = tray_proxy(tray, device, DEVICE);
	g_autofree char *name = tray_string(proxy, "Alias");
	if (!name)
		name = tray_string(proxy, "Address");
	if (!name)
		name = g_strdup("Bluetooth device");
	char title[128] = "Pair Bluetooth device";
	char detail[400];
	const char *accept = "Allow";
	const char *field = NULL;
	gboolean numeric = FALSE, display = FALSE;
	guint32 code;
	if (!strcmp(method, "RequestConfirmation")) {
		g_variant_get_child(parameters, 1, "u", &code);
		snprintf(detail, sizeof detail, "%06u · Confirm this code on %s", code, name);
		accept = "Pair";
	} else if (!strcmp(method, "RequestPasskey") || !strcmp(method, "RequestPinCode")) {
		numeric = !strcmp(method, "RequestPasskey");
		field = numeric ? "Passkey" : "PIN code";
		snprintf(detail, sizeof detail, "Enter the code for %s", name);
		accept = "Pair";
	} else if (!strcmp(method, "DisplayPasskey")) {
		guint16 entered;
		g_variant_get_child(parameters, 1, "u", &code);
		g_variant_get_child(parameters, 2, "q", &entered);
		snprintf(detail, sizeof detail, "Type %06u on %s · %u/6 entered", code, name, entered);
		display = TRUE;
	} else if (!strcmp(method, "DisplayPinCode")) {
		const char *pin;
		g_variant_get_child(parameters, 1, "&s", &pin);
		snprintf(detail, sizeof detail, "Type %s on %s", pin, name);
		display = TRUE;
	} else if (!strcmp(method, "AuthorizeService")) {
		const char *uuid;
		g_variant_get_child(parameters, 1, "&s", &uuid);
		strcpy(title, "Bluetooth service request");
		snprintf(detail, sizeof detail, "%s requests service %s", name, uuid);
	} else {
		snprintf(detail, sizeof detail, "Allow %s to pair?", name);
		accept = "Pair";
	}
	if (display && tray->answer == answer_pairing && !bt->request && !g_strcmp0(device, bt->prompt_device)) {
		g_free(tray->prompt.detail);
		tray->prompt.detail = tray_label(detail);
	} else {
		tray_prompt(tray, title, detail, display ? NULL : accept, answer_pairing);
		bt->prompt_device = g_strdup(device);
		if (field)
			tray_field(tray, "code", field, "", FALSE, numeric, TRUE);
	}
	if (display)
		g_dbus_method_invocation_return_value(request, NULL);
	else
		bt->request = g_object_ref(request);
	tray_popup(tray);
	tray_changed(tray);
}

static const GDBusInterfaceVTable agent_vtable = {.method_call = pairing_request};

static void
default_result(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)reply;
	(void)data;
	if (error)
		tray_error(tray, error, "Could not become the default Bluetooth pairing agent");
}

static void
agent_registered(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)reply;
	(void)data;
	Bluetooth *bt = tray->backend;
	bt->registered = error == NULL;
	if (!bt->registered)
		tray_error(tray, error, "Could not register the Bluetooth pairing agent");
	else if (tray->config.bluetooth_default_agent)
		tray_call(tray, "/org/bluez", AGENT_MANAGER, "RequestDefaultAgent",
		          g_variant_new("(o)", AGENT_PATH), 8000, default_result, NULL);
}

void
bluetooth_configure(Tray *tray)
{
	Bluetooth *bt = tray->backend;
	if (!bt)
		return;
	gboolean make_default = tray->config.bluetooth_default_agent != 0;
	if (bt->registered && make_default != bt->default_agent) {
		if (!make_default)
			tray_call(tray, "/org/bluez", AGENT_MANAGER, "UnregisterAgent",
			          g_variant_new("(o)", AGENT_PATH), 8000, NULL, NULL);
		tray_call(tray, "/org/bluez", AGENT_MANAGER, make_default ? "RequestDefaultAgent" : "RegisterAgent",
		          make_default ? g_variant_new("(o)", AGENT_PATH) : g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"),
		          8000, make_default ? default_result : agent_registered, NULL);
	}
	bt->default_agent = make_default;
	tray_refresh(tray);
}

void
bluetooth_owner(Tray *tray)
{
	Bluetooth *bt = tray->backend;
	if (!bt)
		tray->backend = bt = g_new0(Bluetooth, 1);
	stop_scan(tray);
	cancel_agent(tray);
	bt->registered = FALSE;
	bt->default_agent = tray->config.bluetooth_default_agent != 0;
	if (!tray->connected)
		return;
	if (!bt->agent) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(agent_xml, &error);
		if (info)
			bt->agent = g_dbus_connection_register_object(tray_connection(tray), AGENT_PATH, info->interfaces[0],
			                                             &agent_vtable, tray, NULL, &error);
		if (!bt->agent) {
			tray_error(tray, error, "Could not start the Bluetooth pairing agent");
			return;
		}
	}
	tray_call(tray, "/org/bluez", AGENT_MANAGER, "RegisterAgent",
	          g_variant_new("(os)", AGENT_PATH, "KeyboardDisplay"), 8000, agent_registered, NULL);
}

static gint
device_order(gconstpointer left, gconstpointer right)
{
	const TrayEntry *a = *(TrayEntry *const *)left, *b = *(TrayEntry *const *)right;
	if (a->active != b->active)
		return b->active - a->active;
	if (a->saved != b->saved)
		return b->saved - a->saved;
	return g_utf8_collate(a->label, b->label);
}

static enum TrayIcon
device_icon(GDBusProxy *device)
{
	g_autofree char *name = tray_string(device, "Icon");
	if (!name)
		return TrayDevice;
	if (g_str_has_prefix(name, "audio-"))
		return TrayHeadphones;
	if (!strcmp(name, "input-keyboard"))
		return TrayKeyboard;
	if (!strcmp(name, "input-mouse"))
		return TrayMouse;
	if (!strcmp(name, "input-gaming"))
		return TrayGamepad;
	return !strcmp(name, "computer") ? TrayComputer : TrayDevice;
}

void
bluetooth_model(Tray *tray)
{
	Bluetooth *bt = tray->backend;
	if (!tray->connected || !bt) {
		tray_model(tray, "Bluetooth unavailable", "BlueZ is offline");
		return;
	}
	GList *objects = g_dbus_object_manager_get_objects(G_DBUS_OBJECT_MANAGER(tray->manager));
	g_clear_pointer(&bt->adapter, g_free);
	g_autoptr(GDBusProxy) adapter = NULL;
	for (GList *it = objects; it; it = it->next) {
		const char *path = g_dbus_object_get_object_path(it->data);
		g_autoptr(GDBusProxy) candidate = tray_proxy(tray, path, ADAPTER);
		if (!candidate)
			continue;
		if (*tray->config.bluetooth_adapter && strcmp(strrchr(path, '/') + 1, tray->config.bluetooth_adapter))
			continue;
		int powered = tray_bool(candidate, "Powered"), current = tray_bool(adapter, "Powered");
		if (!adapter || powered > current || (powered == current && strcmp(path, bt->adapter) < 0)) {
			g_free(bt->adapter);
			bt->adapter = g_strdup(path);
			g_set_object(&adapter, candidate);
		}
	}
	if (!adapter) {
		tray_model(tray, "No Bluetooth adapter", "Connect or enable an adapter");
		g_list_free_full(objects, g_object_unref);
		return;
	}
	tray->available = TRUE;
	tray->enabled = tray_bool(adapter, "Powered");
	tray->discoverable = tray_bool(adapter, "Discoverable");
	if (bt->discovery && (!tray->enabled || strcmp(bt->adapter, bt->discovery)))
		stop_scan(tray);
	tray->scanning = bt->discovery || bt->discovery_pending;
	tray->can_scan = tray->enabled && !bt->discovery_pending;
	unsigned connected = 0;
	const char *last = NULL;
	for (GList *it = objects; tray->enabled && it; it = it->next) {
		const char *path = g_dbus_object_get_object_path(it->data);
		g_autoptr(GDBusProxy) device = tray_proxy(tray, path, DEVICE);
		g_autofree char *on_adapter = tray_string(device, "Adapter");
		if (g_strcmp0(on_adapter, bt->adapter))
			continue;
		g_autofree char *name = tray_string(device, "Alias");
		g_autofree char *address = tray_string(device, "Address");
		if (!name || !*name) {
			g_free(name);
			name = g_strdup(address ? address : "Bluetooth device");
		}
		int active = tray_bool(device, "Connected"), paired = tray_bool(device, "Paired");
		/* Newly found devices belong in the list during an explicit scan. */
		if (!active && !paired && !tray->scanning)
			continue;
		g_autoptr(GDBusProxy) battery = tray_proxy(tray, path, "org.bluez.Battery1");
		char detail[64];
		if (active && battery)
			snprintf(detail, sizeof detail, "Connected · %u%%", tray_uint(battery, "Percentage"));
		else
			snprintf(detail, sizeof detail, "%s", active ? "Connected" : paired ? "Paired" : "Nearby");
		TrayEntry *row = tray_entry(tray, path, name, detail, device_icon(device), active, paired, -1);
		if (active) {
			last = row->label;
			++connected;
		}
	}
	g_ptr_array_sort(tray->rows, device_order);
	g_list_free_full(objects, g_object_unref);
	tray->strength = connected > 0;
	char count[64];
	snprintf(count, sizeof count, "%u devices connected", connected);
	tray_model(tray, !tray->enabled ? "Bluetooth off" : connected == 1 ? last : connected ? count : "Bluetooth on",
	           tray->scanning ? "Looking for nearby devices…" : !tray->enabled ? "Turn on to connect devices"
	           : tray->discoverable ? "Visible to nearby devices" : connected == 1 ? "1 connected device"
	           : connected ? "Ready to pair more devices" : "Ready to connect");
}

static void
pair_result(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	Bluetooth *bt = tray->backend;
	g_autofree char *device = data;
	g_clear_pointer(&bt->pairing, g_free);
	cancel_agent(tray);
	if (error) {
		bt_result(tray, reply, error, "Could not pair with the device");
		return;
	}
	/* Pairing takes place only after the user explicitly selects this device. */
	tray_set(tray, device, DEVICE, "Trusted", g_variant_new_boolean(TRUE), NULL, NULL);
	g_autoptr(GDBusProxy) proxy = tray_proxy(tray, device, DEVICE);
	if (tray_bool(proxy, "Connected"))
		bt_result(tray, NULL, NULL, NULL);
	else {
		tray_status(tray, FALSE, "Connecting…");
		tray_call(tray, device, DEVICE, "Connect", NULL, 45000, bt_result, "Could not connect to the device");
	}
}

static void
answer_forget(Tray *tray, gboolean accepted)
{
	Bluetooth *bt = tray->backend;
	g_autofree char *device = g_steal_pointer(&bt->device);
	tray_clear_prompt(tray);
	if (!accepted || !device || !bt->adapter)
		return;
	tray->busy = TRUE;
	tray_status(tray, FALSE, "Removing device…");
	tray_call(tray, bt->adapter, ADAPTER, "RemoveDevice", g_variant_new("(o)", device),
	          15000, bt_result, "Could not remove the device");
}

static void
pairable(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	g_autofree char *adapter = data;
	if (error)
		bt_result(tray, reply, error, "Could not enable pairing");
	else
		tray_set(tray, adapter, ADAPTER, "Discoverable", g_variant_new_boolean(TRUE),
		         bt_result, "Could not make the adapter visible");
}

void
bluetooth_action(Tray *tray, enum TrayAction action, const char *id)
{
	Bluetooth *bt = tray->backend;
	if (!bt->adapter)
		return;
	switch (action) {
	case TrayToggle:
		stop_scan(tray);
		tray->busy = TRUE;
		tray_set(tray, bt->adapter, ADAPTER, "Powered", g_variant_new_boolean(!tray->enabled),
		         bt_result, "Could not change Bluetooth power");
		return;
	case TrayScan:
		if (bt->discovery)
			stop_scan(tray);
		else if (tray->can_scan) {
			bt->discovery_pending = TRUE;
			tray_call(tray, bt->adapter, ADAPTER, "StartDiscovery", NULL, 10000,
			          scan_started, g_strdup(bt->adapter));
			tray_refresh(tray);
		}
		return;
	case TrayHidden:
	case TrayClear:
	case TrayOpenEditor:
	case TrayMirror:
	case TrayIdentify:
	case TrayKeep:
	case TrayRevert:
		return;
	case TrayVisible: {
		if (!tray->enabled)
			return;
		g_autoptr(GDBusProxy) adapter = tray_proxy(tray, bt->adapter, ADAPTER);
		tray->busy = TRUE;
		if (!tray->discoverable && !tray_bool(adapter, "Pairable"))
			tray_set(tray, bt->adapter, ADAPTER, "Pairable", g_variant_new_boolean(TRUE), pairable, g_strdup(bt->adapter));
		else
			tray_set(tray, bt->adapter, ADAPTER, "Discoverable", g_variant_new_boolean(!tray->discoverable),
			         bt_result, "Could not change Bluetooth visibility");
		return;
	}
	case TrayForget:
	case TrayRow:
		break;
	}
	if (!tray->enabled)
		return;
	g_autoptr(GDBusProxy) device = tray_proxy(tray, id, DEVICE);
	if (!device) {
		tray_status(tray, TRUE, "The device is no longer available");
		return;
	}
	gboolean paired = tray_bool(device, "Paired"), active = tray_bool(device, "Connected");
	if (action == TrayForget) {
		if (!paired)
			return;
		g_autofree char *name = tray_string(device, "Alias");
		tray_prompt(tray, "Forget this device?", name, "Forget", answer_forget);
		bt->device = g_strdup(id);
		return;
	}
	if (!active && !paired && !bt->registered) {
		tray_status(tray, TRUE, "Bluetooth pairing agent is unavailable");
		return;
	}
	tray->busy = TRUE;
	if (active || paired) {
		tray_status(tray, FALSE, active ? "Disconnecting…" : "Connecting…");
		tray_call(tray, id, DEVICE, active ? "Disconnect" : "Connect", NULL, 45000,
		          bt_result, active ? "Could not disconnect the device" : "Could not connect to the device");
	} else {
		g_free(bt->pairing);
		bt->pairing = g_strdup(id);
		tray_status(tray, FALSE, "Pairing…");
		tray_call(tray, id, DEVICE, "Pair", NULL, 120000, pair_result, g_strdup(id));
	}
}
