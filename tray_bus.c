#include "tray.h"

#include <string.h>

static void
free_entry(gpointer data)
{
	TrayEntry *entry = data;
	g_free(entry->id);
	g_free(entry->label);
	g_free(entry->detail);
	g_free(entry->body);
	g_free(entry);
}

char *
tray_label(const char *label)
{
	char *text = g_utf8_make_valid(label ? label : "", -1);
	for (char *p = text; *p; ++p)
		if ((unsigned char)*p < ' ' || *p == 127)
			*p = ' ';
	return text;
}

void
tray_model(Tray *tray, const char *headline, const char *subtitle)
{
	g_free(tray->headline);
	g_free(tray->subtitle);
	tray->headline = tray_label(headline);
	tray->subtitle = tray_label(subtitle);
}

TrayEntry *
tray_entry(Tray *tray, const char *id, const char *label, const char *detail,
           enum TrayIcon icon, gboolean active, gboolean saved, int strength)
{
	TrayEntry *entry = g_new0(TrayEntry, 1);
	*entry = (TrayEntry){.id = g_strdup(id), .label = tray_label(label), .detail = tray_label(detail),
	                    .icon = icon, .active = active, .saved = saved, .strength = strength};
	g_ptr_array_add(tray->rows, entry);
	return entry;
}

void
tray_status(Tray *tray, gboolean failed, const char *message)
{
	g_free(tray->message);
	tray->message = tray_label(message);
	tray->failed = failed;
	tray_changed(tray);
}

void
tray_error(Tray *tray, GError *error, const char *fallback)
{
	if (error)
		g_dbus_error_strip_remote_error(error);
	tray_status(tray, TRUE, error ? error->message : fallback);
}

void
tray_clear_prompt(Tray *tray)
{
	TrayPrompt *p = &tray->prompt;
	g_free(p->title);
	g_free(p->detail);
	g_free(p->accept);
	for (int n = 0; n < p->count; ++n) {
		g_free(p->fields[n].label);
		g_free(p->fields[n].key);
		volatile char *value = p->fields[n].value;
		for (size_t i = 0; i < sizeof p->fields[n].value; ++i)
			value[i] = 0;
	}
	*p = (TrayPrompt){0};
	tray->answer = NULL;
	++tray->prompt_serial;
	tray_changed(tray);
}

void
tray_prompt(Tray *tray, const char *title, const char *detail, const char *accept, TrayAnswer answer)
{
	if (tray->answer)
		tray->answer(tray, FALSE);
	tray_clear_prompt(tray);
	tray_status(tray, FALSE, "");
	tray->prompt.title = tray_label(title);
	tray->prompt.detail = tray_label(detail);
	tray->prompt.accept = tray_label(accept);
	tray->answer = answer;
}

void
tray_field(Tray *tray, const char *key, const char *label, const char *value,
           gboolean secret, gboolean digits, gboolean required)
{
	TrayPrompt *p = &tray->prompt;
	g_assert(p->count < TrayMaxFields);
	TrayField *field = &p->fields[p->count++];
	*field = (TrayField){.key = g_strdup(key), .label = tray_label(label), .secret = secret,
	                    .digits = digits, .required = required};
	g_strlcpy(field->value, value ? value : "", sizeof field->value);
}

GDBusConnection *
tray_connection(Tray *tray)
{
	return tray->manager ? g_dbus_object_manager_client_get_connection(tray->manager) : tray->session;
}

GDBusProxy *
tray_proxy(Tray *tray, const char *path, const char *interface)
{
	if (!tray->manager || !path || !strcmp(path, "/"))
		return NULL;
	return G_DBUS_PROXY(g_dbus_object_manager_get_interface(G_DBUS_OBJECT_MANAGER(tray->manager),
	                                                      path, interface));
}

GVariant *
tray_property(GDBusProxy *proxy, const char *name)
{
	return proxy ? g_dbus_proxy_get_cached_property(proxy, name) : NULL;
}

gboolean
tray_bool(GDBusProxy *proxy, const char *name)
{
	g_autoptr(GVariant) value = tray_property(proxy, name);
	return value && g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(value);
}

guint
tray_uint(GDBusProxy *proxy, const char *name)
{
	g_autoptr(GVariant) value = tray_property(proxy, name);
	return !value ? 0 : g_variant_is_of_type(value, G_VARIANT_TYPE_BYTE) ? g_variant_get_byte(value)
	                  : g_variant_is_of_type(value, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(value) : 0;
}

char *
tray_string(GDBusProxy *proxy, const char *name)
{
	g_autoptr(GVariant) value = tray_property(proxy, name);
	return value && (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)
	                 || g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH))
	       ? g_variant_dup_string(value, NULL) : NULL;
}

static gboolean
refresh(gpointer data)
{
	Tray *tray = data;
	tray->refresh_id = 0;
	g_ptr_array_set_size(tray->rows, 0);
	tray->available = tray->enabled = tray->scanning = tray->can_scan = tray->discoverable = FALSE;
	tray->strength = 0;
	if (tray->kind == TrayWifi)
		wifi_model(tray);
	else
		bluetooth_model(tray);
	tray_changed(tray);
	return G_SOURCE_REMOVE;
}

void
tray_refresh(Tray *tray)
{
	if (!tray->refresh_id)
		tray->refresh_id = g_idle_add(refresh, tray);
}

static void
owner_changed(GObject *object, GParamSpec *property, gpointer data)
{
	(void)property;
	Tray *tray = data;
	g_free(tray->owner);
	tray->owner = g_dbus_object_manager_client_get_name_owner(G_DBUS_OBJECT_MANAGER_CLIENT(object));
	tray->connected = tray->owner != NULL;
	if (tray->kind == TrayWifi)
		wifi_owner(tray);
	else
		bluetooth_owner(tray);
	tray_refresh(tray);
}

static void
object_changed(GDBusObjectManager *manager, GDBusObject *object, gpointer data)
{
	(void)manager;
	(void)object;
	tray_refresh(data);
}

static void
interface_changed(GDBusObjectManager *manager, GDBusObject *object, GDBusInterface *interface, gpointer data)
{
	(void)interface;
	object_changed(manager, object, data);
}

static void
properties_changed(GDBusObjectManagerClient *manager, GDBusObjectProxy *object,
                   GDBusProxy *interface, GVariant *changed, const gchar *const *invalid, gpointer data)
{
	(void)manager;
	(void)object;
	(void)interface;
	(void)changed;
	(void)invalid;
	tray_refresh(data);
}

static void
manager_ready(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)source;
	Tray *tray = data;
	g_autoptr(GError) error = NULL;
	tray->manager = G_DBUS_OBJECT_MANAGER_CLIENT(g_dbus_object_manager_client_new_for_bus_finish(result, &error));
	if (!tray->manager) {
		tray_error(tray, error, "Cannot connect to the system bus");
		return;
	}
	g_signal_connect(tray->manager, "notify::name-owner", G_CALLBACK(owner_changed), tray);
	g_signal_connect(tray->manager, "object-added", G_CALLBACK(object_changed), tray);
	g_signal_connect(tray->manager, "object-removed", G_CALLBACK(object_changed), tray);
	g_signal_connect(tray->manager, "interface-added", G_CALLBACK(interface_changed), tray);
	g_signal_connect(tray->manager, "interface-removed", G_CALLBACK(interface_changed), tray);
	g_signal_connect(tray->manager, "interface-proxy-properties-changed", G_CALLBACK(properties_changed), tray);
	owner_changed(G_OBJECT(tray->manager), NULL, tray);
}

void
tray_bus_start(Tray *tray)
{
	tray->rows = g_ptr_array_new_with_free_func(free_entry);
	if (tray->kind == TrayNotifications) {
		nod_start(tray);
		return;
	}
	if (tray->kind == TrayDisplays) {
		displays_start(tray);
		return;
	}
	tray->service = tray->kind == TrayWifi ? "org.freedesktop.NetworkManager" : "org.bluez";
	tray->manager_path = tray->kind == TrayWifi ? "/org/freedesktop" : "/";
	g_dbus_object_manager_client_new_for_bus(G_BUS_TYPE_SYSTEM, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
	                                       tray->service, tray->manager_path, NULL, NULL, NULL, NULL,
	                                       manager_ready, tray);
}

typedef struct {
	Tray *tray;
	TrayReply callback;
	gpointer data;
	char *owner;
} Call;

static void
call_done(GObject *source, GAsyncResult *result, gpointer data)
{
	Call *call = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
	if (g_strcmp0(call->owner, call->tray->owner)) {
		g_clear_pointer(&reply, g_variant_unref);
		g_clear_error(&error);
		error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED, "The system service restarted");
	}
	call->callback(call->tray, reply, error, call->data);
	g_free(call->owner);
	g_free(call);
}

void
tray_call(Tray *tray, const char *path, const char *interface, const char *method, GVariant *parameters,
          int timeout, TrayReply callback, gpointer data)
{
	GDBusConnection *bus = tray_connection(tray);
	if (!bus || !tray->owner) {
		if (parameters)
			g_variant_unref(g_variant_ref_sink(parameters));
		if (callback) {
			g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CLOSED,
			                                             "The system service is unavailable");
			callback(tray, NULL, error, data);
		}
		return;
	}
	Call *call = callback ? g_new(Call, 1) : NULL;
	if (call)
		*call = (Call){tray, callback, data, g_strdup(tray->owner)};
	g_dbus_connection_call(bus, tray->owner, path, interface, method, parameters, NULL,
	                       G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, timeout, NULL,
	                       callback ? call_done : NULL, call);
}

void
tray_set(Tray *tray, const char *path, const char *interface, const char *property, GVariant *value,
         TrayReply callback, gpointer data)
{
	tray_call(tray, path, PROPERTIES, "Set", g_variant_new("(ssv)", interface, property, value),
	          15000, callback, data);
}

void
tray_action(Tray *tray, enum TrayAction action, const char *id)
{
	if ((tray->kind == TrayBluetooth && !tray->available) || (tray->busy && action != TrayScan))
		return;
	tray_status(tray, FALSE, "");
	if (tray->kind == TrayWifi)
		wifi_action(tray, action, id);
	else if (tray->kind == TrayBluetooth)
		bluetooth_action(tray, action, id);
	else if (tray->kind == TrayDisplays)
		displays_action(tray, action, id);
	else
		nod_action(tray, action, id);
}
