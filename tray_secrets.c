#include "tray_wifi.h"

#include <string.h>
#include <sys/stat.h>

#define NM_AGENT NM ".SecretAgent"
#define NM_AGENT_MANAGER NM ".AgentManager"
#define NM_AGENT_PATH NM_PATH "/SecretAgent"
#define NM_AGENT_ERROR NM_AGENT "."
#define KEYRING_SCHEMA "org.freedesktop.NetworkManager.Connection"

enum {
	AllowInteraction = 1, RequestNew = 2,
	AgentOwned = 1, NotSaved = 2, NotRequired = 4,
};

static const char xml[] =
	"<node><interface name='" NM_AGENT "'>"
	"<method name='GetSecrets'><arg type='a{sa{sv}}' direction='in'/><arg type='o' direction='in'/>"
	"<arg type='s' direction='in'/><arg type='as' direction='in'/><arg type='u' direction='in'/>"
	"<arg type='a{sa{sv}}' direction='out'/></method>"
	"<method name='CancelGetSecrets'><arg type='o' direction='in'/><arg type='s' direction='in'/></method>"
	"<method name='SaveSecrets'><arg type='a{sa{sv}}' direction='in'/><arg type='o' direction='in'/></method>"
	"<method name='DeleteSecrets'><arg type='a{sa{sv}}' direction='in'/><arg type='o' direction='in'/></method>"
	"</interface></node>";

typedef struct {
	GDBusMethodInvocation *invocation;
	GVariant *setting;
	char *path, *name, *uuid, *id;
	GStrv hints;
	guint flags, deadline, references;
	gboolean vpn, external_ui;
	GCancellable *cancel;
	GSubprocess *child;
	GPtrArray *keys;
	GHashTable *values;
} Request;

typedef struct {
	guint object, generation;
	gboolean registered, pending;
	Request *request;
} Agent;

static Agent agent;
static void answer_secret(Tray *, gboolean);
static void prompt_secret(Tray *, Request *);

static void
wipe(gpointer data)
{
	if (!data)
		return;
	volatile char *bytes = data;
	for (size_t n = strlen(data); n > 0; --n)
		*bytes++ = 0;
	g_free(data);
}

static GHashTable *
values_new(void)
{
	return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, wipe);
}

static Request *
request_ref(Request *r)
{
	++r->references;
	return r;
}

static void
request_unref(gpointer data)
{
	Request *r = data;
	if (--r->references)
		return;
	g_clear_object(&r->invocation);
	g_clear_object(&r->cancel);
	g_clear_object(&r->child);
	g_clear_pointer(&r->setting, g_variant_unref);
	g_strfreev(r->hints);
	g_free(r->path);
	g_free(r->name);
	g_free(r->uuid);
	g_free(r->id);
	g_ptr_array_unref(r->keys);
	g_hash_table_unref(r->values);
	g_free(r);
}

static void
finish(Tray *tray, const char *error, const char *message)
{
	Request *r = g_steal_pointer(&agent.request);
	if (!r)
		return;
	if (r->deadline)
		g_source_remove(r->deadline);
	g_cancellable_cancel(r->cancel);
	if (r->child)
		g_subprocess_force_exit(r->child);
	if (error) {
		g_dbus_method_invocation_return_dbus_error(r->invocation, error, message);
	} else {
		GVariantBuilder settings, fields, vpn;
		g_variant_builder_init(&settings, G_VARIANT_TYPE("a{sa{sv}}"));
		g_variant_builder_init(&fields, G_VARIANT_TYPE_VARDICT);
		if (r->vpn)
			g_variant_builder_init(&vpn, G_VARIANT_TYPE("a{ss}"));
		GHashTableIter iter;
		gpointer key, value;
		g_hash_table_iter_init(&iter, r->values);
		while (g_hash_table_iter_next(&iter, &key, &value)) {
			if (r->vpn)
				g_variant_builder_add(&vpn, "{ss}", key, value);
			else
				g_variant_builder_add(&fields, "{sv}", key, g_variant_new_string(value));
		}
		if (r->vpn)
			g_variant_builder_add(&fields, "{sv}", "secrets", g_variant_builder_end(&vpn));
		g_variant_builder_add(&settings, "{sa{sv}}", r->name, &fields);
		g_dbus_method_invocation_return_value(r->invocation, g_variant_new("(a{sa{sv}})", &settings));
	}
	if (tray->answer == answer_secret)
		tray_clear_prompt(tray);
	request_unref(r);
	tray_changed(tray);
}

static gboolean
timed_out(gpointer data)
{
	Tray *tray = data;
	agent.request->deadline = 0;
	finish(tray, NM_AGENT_ERROR "AgentCanceled", "Authentication timed out");
	return G_SOURCE_REMOVE;
}

static guint
secret_flags(GVariant *setting, const char *key, gboolean vpn)
{
	g_autofree char *flag_key = g_strconcat(key, "-flags", NULL);
	guint flags = 0;
	if (vpn) {
		g_autoptr(GVariant) data = g_variant_lookup_value(setting, "data", G_VARIANT_TYPE("a{ss}"));
		const char *raw = NULL;
		if (data && g_variant_lookup(data, flag_key, "&s", &raw))
			flags = g_ascii_strtoull(raw, NULL, 10);
	} else {
		g_variant_lookup(setting, flag_key, "u", &flags);
	}
	return flags;
}

static gboolean
allowed_key(const char *key)
{
	return key && *key && strlen(key) < 128 &&
	       strspn(key, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.:") == strlen(key);
}

static void
add_key(Request *r, const char *key)
{
	if (allowed_key(key) && !g_ptr_array_find_with_equal_func(r->keys, key, g_str_equal, NULL) &&
	    !(secret_flags(r->setting, key, r->vpn) & NotRequired) && r->keys->len < TrayMaxFields)
		g_ptr_array_add(r->keys, g_strdup(key));
}

static void
request_keys(Request *r)
{
	if (!strcmp(r->name, WIFI_SECURITY)) {
		guint32 index = 0;
		g_variant_lookup(r->setting, "wep-tx-keyidx", "u", &index);
		add_key(r, wifi_psk_key(wifi_value(r->setting, "key-mgmt"), index));
	} else if (!strcmp(r->name, "802-1x")) {
		static const char *known[] = {"password", "private-key-password", "phase2-private-key-password", "pin"};
		for (int i = 0; r->hints[i]; ++i)
			for (size_t n = 0; n < G_N_ELEMENTS(known); ++n)
				if (!strcmp(known[n], r->hints[i]))
					add_key(r, known[n]);
		if (!r->keys->len) {
			g_autoptr(GVariant) methods = g_variant_lookup_value(r->setting, "eap", G_VARIANT_TYPE_STRING_ARRAY);
			const char *method = "";
			if (methods && g_variant_n_children(methods))
				g_variant_get_child(methods, 0, "&s", &method);
			add_key(r, !strcmp(method, "tls") ? "private-key-password" :
			           !strcmp(wifi_value(r->setting, "phase2-autheap"), "tls") ? "phase2-private-key-password" :
			           !strcmp(method, "sim") || !strcmp(method, "aka") || !strcmp(method, "aka'") ? "pin" : "password");
		}
	} else if (r->vpn) {
		for (int i = 0; r->hints[i]; ++i)
			add_key(r, r->hints[i]);
		if (!r->keys->len)
			add_key(r, "password");
	} else if (!strcmp(r->name, "wireguard")) {
		add_key(r, "private-key");
	}
}

enum StoreOp { Lookup, Save, Delete };
typedef struct {
	char *setting, *key, *value;
} StoredSecret;

typedef struct {
	enum StoreOp op;
	char *tool, *uuid, *name, *label;
	GPtrArray *keys;
	GPtrArray *secrets;
	GDBusMethodInvocation *invocation;
} Store;

static void
stored_secret_free(gpointer data)
{
	StoredSecret *secret = data;
	g_free(secret->setting);
	g_free(secret->key);
	wipe(secret->value);
	g_free(secret);
}

static void
store_free(gpointer data)
{
	Store *s = data;
	g_free(s->tool);
	g_free(s->uuid);
	g_free(s->name);
	g_free(s->label);
	g_clear_pointer(&s->keys, g_ptr_array_unref);
	g_clear_pointer(&s->secrets, g_ptr_array_unref);
	g_clear_object(&s->invocation);
	g_free(s);
}

/* Use the platform keyring when there is one. Passwords go through a private
   pipe to secret-tool, with the same attributes used by nm-applet. */
static void
keyring_task(GTask *task, gpointer source, gpointer task_data, GCancellable *cancel)
{
	(void)source;
	Store *s = task_data;
	g_autoptr(GHashTable) values = values_new();
	gboolean ok = TRUE;
	guint count = s->op == Delete ? 1 : s->op == Save ? s->secrets->len : s->keys->len;
	for (guint i = 0; i < count && !g_cancellable_is_cancelled(cancel); ++i) {
		const StoredSecret *saved = s->op == Save ? s->secrets->pdata[i] : NULL;
		const char *key = saved ? saved->key : s->op == Lookup ? s->keys->pdata[i] : NULL;
		const char *actions[] = {"lookup", "store", "clear"};
		const char *args[] = {s->tool, actions[s->op], NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL};
		g_autofree char *label = g_strconcat("--label=", s->label ? s->label : "Network credential", NULL);
		int n = 2;
		if (s->op == Save)
			args[n++] = label;
		args[n++] = "xdg:schema";
		args[n++] = KEYRING_SCHEMA;
		args[n++] = "connection-uuid";
		args[n++] = s->uuid;
		if (key) {
			args[n++] = "setting-name";
			args[n++] = saved ? saved->setting : s->name;
			args[n++] = "setting-key";
			args[n++] = key;
		}
		GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDERR_SILENCE | G_SUBPROCESS_FLAGS_STDOUT_PIPE;
		if (s->op == Save)
			flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
		g_autoptr(GSubprocess) process = g_subprocess_newv(args, flags, NULL);
		if (!process) {
			ok = FALSE;
			break;
		}
		char *output = NULL;
		const char *secret = saved ? saved->value : NULL;
		if (!g_subprocess_communicate_utf8(process, secret, cancel, &output, NULL, NULL)) {
			g_subprocess_force_exit(process);
			ok = FALSE;
		} else if (g_subprocess_get_successful(process) && s->op == Lookup && output) {
			size_t len = strlen(output);
			if (len && output[len-1] == '\n')
				output[len-1] = 0;
			if (*output)
				g_hash_table_insert(values, g_strdup(key), g_strdup(output));
		} else if (s->op == Save && !g_subprocess_get_successful(process)) {
			ok = FALSE;
		}
		wipe(output);
	}
	if (!ok && s->op == Save)
		g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "The session keyring could not store the credentials");
	else
		g_task_return_pointer(task, g_steal_pointer(&values), (GDestroyNotify)g_hash_table_unref);
}

typedef struct {
	guint source;
	GCancellable *cancel;
} StoreDeadline;

static gboolean
store_deadline(gpointer data)
{
	StoreDeadline *deadline = data;
	deadline->source = 0;
	g_cancellable_cancel(deadline->cancel);
	return G_SOURCE_REMOVE;
}

static void
run_store(Store *s, guint seconds, GAsyncReadyCallback callback, gpointer data, Tray *tray)
{
	StoreDeadline *deadline = g_new0(StoreDeadline, 1);
	deadline->cancel = g_cancellable_new();
	deadline->source = g_timeout_add_seconds(seconds, store_deadline, deadline);
	g_autoptr(GTask) task = g_task_new(NULL, deadline->cancel, callback, data);
	g_object_set_data(G_OBJECT(task), "mwm-tray", tray);
	g_object_set_data_full(G_OBJECT(task), "mwm-deadline", deadline, g_free);
	g_task_set_task_data(task, s, store_free);
	g_task_run_in_thread(task, keyring_task);
}

static GHashTable *
store_result(GAsyncResult *result, GError **error)
{
	StoreDeadline *deadline = g_object_get_data(G_OBJECT(result), "mwm-deadline");
	if (deadline->source)
		g_source_remove(deadline->source);
	g_clear_object(&deadline->cancel);
	return g_task_propagate_pointer(G_TASK(result), error);
}

static void
storage_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)source;
	(void)data;
	Store *s = g_task_get_task_data(G_TASK(result));
	g_autoptr(GError) error = NULL;
	g_autoptr(GHashTable) unused = store_result(result, &error);
	if (error && s->op == Save)
		g_dbus_method_invocation_return_dbus_error(s->invocation, NM_AGENT_ERROR "Failed", "The session keyring is unavailable");
	else
		g_dbus_method_invocation_return_value(s->invocation, NULL);
}

static void
save_item(Store *s, const char *name, GVariant *setting, const char *key, const char *value, gboolean vpn)
{
	guint flags = secret_flags(setting, key, vpn);
	if (allowed_key(key) && *value && (flags & AgentOwned) && !(flags & (NotSaved | NotRequired))) {
		StoredSecret *secret = g_new(StoredSecret, 1);
		*secret = (StoredSecret){g_strdup(name), g_strdup(key), g_strdup(value)};
		g_ptr_array_add(s->secrets, secret);
	}
}

static void
save_or_delete(GVariant *settings, GDBusMethodInvocation *invocation, gboolean delete)
{
	g_autoptr(GVariant) connection = wifi_setting(settings, "connection");
	const char *uuid = wifi_value(connection, "uuid");
	g_autofree char *tool = g_find_program_in_path("secret-tool");
	if (!*uuid || !tool) {
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}
	Store *s = g_new0(Store, 1);
	*s = (Store){.op = delete ? Delete : Save, .tool = g_steal_pointer(&tool), .uuid = g_strdup(uuid),
	             .label = g_strdup_printf("Network credentials for %s", wifi_value(connection, "id")),
	             .invocation = g_object_ref(invocation)};
	if (delete) {
		run_store(s, 10, storage_finished, NULL, NULL);
		return;
	}
	s->secrets = g_ptr_array_new_with_free_func(stored_secret_free);
	static const char *names[] = {WIFI_SECURITY, "802-1x", "wireguard", "vpn"};
	for (size_t n = 0; n < G_N_ELEMENTS(names); ++n) {
		g_autoptr(GVariant) setting = wifi_setting(settings, names[n]);
		if (!setting)
			continue;
		gboolean vpn = !strcmp(names[n], "vpn");
		g_autoptr(GVariant) vpn_values = vpn ? g_variant_lookup_value(setting, "secrets", G_VARIANT_TYPE("a{ss}")) : NULL;
		GVariantIter iter;
		const char *key, *value;
		if (vpn_values) {
			g_variant_iter_init(&iter, vpn_values);
			while (g_variant_iter_next(&iter, "{&s&s}", &key, &value))
				save_item(s, names[n], setting, key, value, TRUE);
		} else if (!vpn) {
			static const char *keys[] = {"psk", "wep-key0", "wep-key1", "wep-key2", "wep-key3", "leap-password",
			                             "password", "private-key-password", "phase2-private-key-password", "pin", "private-key"};
			for (size_t i = 0; i < G_N_ELEMENTS(keys); ++i)
				save_item(s, names[n], setting, keys[i], wifi_value(setting, keys[i]), FALSE);
		}
	}
	if (s->secrets->len)
		run_store(s, 15, storage_finished, NULL, NULL);
	else {
		g_dbus_method_invocation_return_value(invocation, NULL);
		store_free(s);
	}
}

static const char *
pretty_key(const char *key)
{
	if (!strcmp(key, "psk"))
		return "Passphrase";
	if (g_str_has_prefix(key, "wep-key"))
		return "WEP key";
	if (!strcmp(key, "private-key"))
		return "Private key";
	if (g_str_has_suffix(key, "private-key-password"))
		return "Private key password";
	if (!strcmp(key, "pin"))
		return "PIN";
	if (!strcmp(key, "password") || !strcmp(key, "leap-password"))
		return "Password";
	return key;
}

static void
answer_secret(Tray *tray, gboolean accepted)
{
	Request *r = agent.request;
	if (!r)
		return;
	if (!accepted) {
		finish(tray, NM_AGENT_ERROR "UserCanceled", "Authentication canceled");
		return;
	}
	for (int i = 0; i < tray->prompt.count; ++i) {
		TrayField *f = &tray->prompt.fields[i];
		if (!*f->value)
			g_hash_table_remove(r->values, f->key);
		else
			g_hash_table_replace(r->values, g_strdup(f->key), g_strdup(f->value));
	}
	finish(tray, NULL, NULL);
}

static void
prompt_secret(Tray *tray, Request *r)
{
	const char *title = r->vpn ? "VPN authentication" : "Network authentication";
	const char *detail = r->id;
	if (r->vpn)
		for (int i = 0; r->hints[i]; ++i)
			if (g_str_has_prefix(r->hints[i], "x-vpn-message:"))
				detail = r->hints[i]+strlen("x-vpn-message:");
	tray_prompt(tray, title, detail, "Connect", answer_secret);
	for (guint i = 0; i < r->keys->len; ++i) {
		const char *key = r->keys->pdata[i];
		const char *stored = !(r->flags & RequestNew) ? g_hash_table_lookup(r->values, key) : NULL;
		tray_field(tray, key, pretty_key(key), stored, TRUE, !strcmp(key, "pin"), TRUE);
	}
	tray_popup(tray);
}

static char *
vpn_helper(const char *service, gboolean *external_ui, gboolean *hints)
{
	static const char *dirs[] = {"/etc/NetworkManager/VPN", "/usr/lib/NetworkManager/VPN",
	                             "/usr/lib64/NetworkManager/VPN", "/usr/share/NetworkManager/VPN"};
	for (size_t i = 0; i < G_N_ELEMENTS(dirs); ++i) {
		g_autoptr(GDir) dir = g_dir_open(dirs[i], 0, NULL);
		const char *name;
		while (dir && (name = g_dir_read_name(dir))) {
			if (!g_str_has_suffix(name, ".name"))
				continue;
			g_autofree char *path = g_build_filename(dirs[i], name, NULL);
			struct stat st;
			if (stat(path, &st) || st.st_uid != 0 || (st.st_mode & 022) || !S_ISREG(st.st_mode))
				continue;
			g_autoptr(GKeyFile) file = g_key_file_new();
			if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, NULL))
				continue;
			g_autofree char *candidate = g_key_file_get_string(file, "VPN Connection", "service", NULL);
			g_auto(GStrv) aliases = g_key_file_get_string_list(file, "VPN Connection", "aliases", NULL, NULL);
			if (g_strcmp0(candidate, service) && (!aliases || !g_strv_contains((const char *const *)aliases, service)))
				continue;
			g_autofree char *binary = g_key_file_get_string(file, "GNOME", "auth-dialog", NULL);
			if (!binary || !g_path_is_absolute(binary) || stat(binary, &st) ||
			    st.st_uid != 0 || (st.st_mode & 022) || !S_ISREG(st.st_mode) || !(st.st_mode & 0111))
				continue;
			*external_ui = g_key_file_get_boolean(file, "GNOME", "supports-external-ui-mode", NULL);
			*hints = g_key_file_get_boolean(file, "GNOME", "supports-hints", NULL);
			return g_steal_pointer(&binary);
		}
	}
	return NULL;
}

static void
helper_line(GString *input, const char *tag, const char *value)
{
	g_auto(GStrv) lines = g_strsplit(value, "\n", -1);
	g_string_append(input, tag);
	for (int n = 0; lines[n]; ++n)
		g_string_append_printf(input, "=%s\n", lines[n]);
}

static gboolean
external_prompt(Tray *tray, Request *r, const char *output)
{
	g_autoptr(GKeyFile) result = g_key_file_new();
	if (!g_key_file_load_from_data(result, output, strlen(output), G_KEY_FILE_NONE, NULL) ||
	    !g_key_file_has_group(result, "VPN Plugin UI"))
		return FALSE;
	g_autofree char *title = g_key_file_get_string(result, "VPN Plugin UI", "Title", NULL);
	g_autofree char *detail = g_key_file_get_string(result, "VPN Plugin UI", "Description", NULL);
	g_auto(GStrv) groups = g_key_file_get_groups(result, NULL);
	tray_prompt(tray, title ? title : "VPN authentication", detail ? detail : r->id, "Connect", answer_secret);
	for (int i = 0; groups[i]; ++i) {
		const char *key = groups[i];
		if (!allowed_key(key) || !g_key_file_get_boolean(result, key, "IsSecret", NULL))
			continue;
		char *value = g_key_file_get_string(result, key, "Value", NULL);
		if (!g_key_file_get_boolean(result, key, "ShouldAsk", NULL)) {
			if (value && *value)
				g_hash_table_replace(r->values, g_strdup(key), g_steal_pointer(&value));
		} else {
			if (tray->prompt.count == TrayMaxFields) {
				wipe(value);
				finish(tray, NM_AGENT_ERROR "Failed", "The VPN requested too many fields");
				tray_status(tray, TRUE, "The VPN requested too many credential fields");
				return TRUE;
			}
			g_autofree char *label = g_key_file_get_string(result, key, "Label", NULL);
			gboolean echo = g_key_file_get_boolean(result, key, "ForceEcho", NULL);
			tray_field(tray, key, label ? label : pretty_key(key), value, !echo, FALSE, TRUE);
		}
		wipe(value);
	}
	if (tray->prompt.count)
		tray_popup(tray);
	else
		finish(tray, NULL, NULL);
	return TRUE;
}

static void
helper_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	Tray *tray = g_object_get_data(source, "mwm-tray");
	Request *r = data;
	char *output = NULL;
	gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, NULL, NULL)
	              && g_subprocess_get_successful(G_SUBPROCESS(source));
	g_clear_object(&r->child);
	if (agent.request == r) {
		if (!ok || !output || strlen(output) > 65536) {
			finish(tray, NM_AGENT_ERROR "UserCanceled", "VPN authentication was canceled or failed");
			tray_status(tray, TRUE, "VPN authentication was canceled or failed");
		} else if (r->external_ui) {
			if (!external_prompt(tray, r, output))
				prompt_secret(tray, r);
		} else {
			g_auto(GStrv) lines = g_strsplit(output, "\n", -1);
			for (int i = 0; lines[i] && lines[i+1] && *lines[i]; i += 2) {
				if (allowed_key(lines[i]))
					g_hash_table_replace(r->values, g_strdup(lines[i]), g_strdup(lines[i+1]));
				wipe(g_steal_pointer(&lines[i+1]));
				lines[i+1] = g_strdup("");
			}
			finish(tray, NULL, NULL);
		}
	}
	wipe(output);
	request_unref(r);
}

static gboolean
launch_helper(Tray *tray, Request *r)
{
	const char *service = wifi_value(r->setting, "service-type");
	gboolean hints = FALSE;
	g_autofree char *binary = *service ? vpn_helper(service, &r->external_ui, &hints) : NULL;
	if (!binary)
		return FALSE;
	g_autoptr(GPtrArray) args = g_ptr_array_new();
	const char *base[] = {binary, "-u", r->uuid, "-n", r->id, "-s", service, "-i"};
	for (size_t i = 0; i < G_N_ELEMENTS(base); ++i)
		g_ptr_array_add(args, (gpointer)base[i]);
	if (r->external_ui)
		g_ptr_array_add(args, "--external-ui-mode");
	if (r->flags & RequestNew)
		g_ptr_array_add(args, "-r");
	for (int i = 0; hints && r->hints[i]; ++i) {
		g_ptr_array_add(args, "-t");
		g_ptr_array_add(args, r->hints[i]);
	}
	g_ptr_array_add(args, NULL);
	r->child = g_subprocess_newv((const char *const *)args->pdata,
	                            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
	                            G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
	if (!r->child)
		return FALSE;
	GString *input = g_string_new(NULL);
	g_autoptr(GVariant) options = g_variant_lookup_value(r->setting, "data", G_VARIANT_TYPE("a{ss}"));
	if (options) {
		GVariantIter iter;
		const char *key, *value;
		g_variant_iter_init(&iter, options);
		while (g_variant_iter_next(&iter, "{&s&s}", &key, &value)) {
			helper_line(input, "DATA_KEY", key);
			helper_line(input, "DATA_VAL", value);
			g_string_append_c(input, '\n');
		}
	}
	GHashTableIter iter;
	gpointer key, value;
	g_hash_table_iter_init(&iter, r->values);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		helper_line(input, "SECRET_KEY", key);
		helper_line(input, "SECRET_VAL", value);
		g_string_append_c(input, '\n');
	}
	g_string_append(input, "DONE\nQUIT\n");
	g_object_set_data(G_OBJECT(r->child), "mwm-tray", tray);
	tray_hide(tray);
	char *buffer = g_string_free(input, FALSE);
	g_object_set_data_full(G_OBJECT(r->child), "mwm-vpn-input", buffer, wipe);
	g_subprocess_communicate_utf8_async(r->child, buffer, r->cancel, helper_finished, request_ref(r));
	return TRUE;
}

static void
lookup_complete(Tray *tray, Request *r)
{
	gboolean complete = r->keys->len > 0;
	for (guint i = 0; i < r->keys->len; ++i)
		complete &= g_hash_table_contains(r->values, r->keys->pdata[i]);
	if (complete && !(r->flags & RequestNew) && (!r->vpn || !(r->flags & AllowInteraction))) {
		finish(tray, NULL, NULL);
	} else if (!(r->flags & AllowInteraction) || !r->keys->len) {
		finish(tray, NM_AGENT_ERROR "NoSecrets", "No saved credentials for this connection");
	} else if (!r->vpn || !launch_helper(tray, r)) {
		prompt_secret(tray, r);
	}
}

static void
lookup_finished(GObject *source, GAsyncResult *result, gpointer data)
{
	(void)source;
	Request *r = data;
	Tray *tray = g_object_get_data(G_OBJECT(result), "mwm-tray");
	g_autoptr(GHashTable) values = store_result(result, NULL);
	if (agent.request == r) {
		GHashTableIter iter;
		gpointer key, value;
		if (values) {
			g_hash_table_iter_init(&iter, values);
			while (g_hash_table_iter_next(&iter, &key, &value))
				g_hash_table_replace(r->values, g_strdup(key), g_strdup(value));
		}
		lookup_complete(tray, r);
	}
	request_unref(r);
}

static void
begin_lookup(Tray *tray, Request *r)
{
	r->deadline = g_timeout_add_seconds(115, timed_out, tray);
	g_autofree char *tool = g_find_program_in_path("secret-tool");
	if (!tool || !*r->uuid || (r->flags & RequestNew)) {
		lookup_complete(tray, r);
		return;
	}
	Store *s = g_new0(Store, 1);
	*s = (Store){.tool = g_steal_pointer(&tool), .uuid = g_strdup(r->uuid), .name = g_strdup(r->name),
	             .keys = g_ptr_array_new_with_free_func(g_free)};
	for (guint i = 0; i < r->keys->len; ++i) {
		const char *key = r->keys->pdata[i];
		if (!(secret_flags(r->setting, key, r->vpn) & NotSaved) && !g_hash_table_contains(r->values, key))
			g_ptr_array_add(s->keys, g_strdup(key));
	}
	if (!s->keys->len) {
		store_free(s);
		lookup_complete(tray, r);
		return;
	}
	run_store(s, 3, lookup_finished, request_ref(r), tray);
}

static void
agent_request(GDBusConnection *connection, const char *sender, const char *path,
              const char *interface, const char *method, GVariant *parameters,
              GDBusMethodInvocation *invocation, gpointer data)
{
	(void)connection;
	(void)path;
	(void)interface;
	Tray *tray = data;
	if (g_strcmp0(sender, tray->owner)) {
		g_dbus_method_invocation_return_dbus_error(invocation, "org.freedesktop.DBus.Error.AccessDenied",
		                                           "Only NetworkManager can use this agent");
		return;
	}
	if (!strcmp(method, "CancelGetSecrets")) {
		const char *object, *setting;
		g_variant_get(parameters, "(&o&s)", &object, &setting);
		if (agent.request && !strcmp(object, agent.request->path) && !strcmp(setting, agent.request->name))
			finish(tray, NM_AGENT_ERROR "AgentCanceled", "NetworkManager canceled the request");
		g_dbus_method_invocation_return_value(invocation, NULL);
		return;
	}
	g_autoptr(GVariant) settings = g_variant_get_child_value(parameters, 0);
	if (!strcmp(method, "SaveSecrets") || !strcmp(method, "DeleteSecrets")) {
		save_or_delete(settings, invocation, !strcmp(method, "DeleteSecrets"));
		return;
	}
	const char *object, *name;
	guint flags;
	g_variant_get_child(parameters, 1, "&o", &object);
	g_variant_get_child(parameters, 2, "&s", &name);
	g_variant_get_child(parameters, 4, "u", &flags);
	g_autoptr(GVariant) setting = wifi_setting(settings, name);
	g_autoptr(GVariant) profile = wifi_setting(settings, "connection");
	if (!profile || !setting || agent.request) {
		g_dbus_method_invocation_return_dbus_error(invocation, agent.request ? NM_AGENT_ERROR "AgentCanceled" :
		                                           NM_AGENT_ERROR "InvalidConnection", agent.request ?
		                                           "Another authentication request is in progress" : "Missing connection settings");
		return;
	}
	Request *r = g_new0(Request, 1);
	*r = (Request){.invocation = g_object_ref(invocation), .setting = g_steal_pointer(&setting), .path = g_strdup(object),
	               .name = g_strdup(name), .uuid = g_strdup(wifi_value(profile, "uuid")),
	               .id = g_strdup(wifi_value(profile, "id")), .flags = flags, .references = 1, .vpn = !strcmp(name, "vpn"),
	               .cancel = g_cancellable_new(), .keys = g_ptr_array_new_with_free_func(g_free), .values = values_new()};
	g_variant_get_child(parameters, 3, "^as", &r->hints);
	request_keys(r);
	if (!(flags & RequestNew) && r->vpn) {
		g_autoptr(GVariant) values = g_variant_lookup_value(r->setting, "secrets", G_VARIANT_TYPE("a{ss}"));
		if (values) {
			const char *key, *value;
			GVariantIter iter;
			g_variant_iter_init(&iter, values);
			while (g_variant_iter_next(&iter, "{&s&s}", &key, &value))
				if (allowed_key(key) && *value)
					g_hash_table_insert(r->values, g_strdup(key), g_strdup(value));
		}
	} else if (!(flags & RequestNew)) {
		for (guint i = 0; i < r->keys->len; ++i) {
			const char *key = r->keys->pdata[i], *value = wifi_value(r->setting, key);
			if (*value)
				g_hash_table_insert(r->values, g_strdup(key), g_strdup(value));
		}
	}
	agent.request = r;
	begin_lookup(tray, r);
}

static const GDBusInterfaceVTable agent_vtable = {.method_call = agent_request};

static void
registered(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	if (GPOINTER_TO_UINT(data) != agent.generation)
		return;
	agent.pending = FALSE;
	agent.registered = reply != NULL;
	if (error)
		tray_error(tray, error, "Could not register the NetworkManager secret agent");
	else
		wifi_secrets_configure(tray);
}

void
wifi_secrets_configure(Tray *tray)
{
	if (!tray->connected || agent.pending)
		return;
	if (!tray->config.wifi_secret_agent && agent.registered) {
		finish(tray, NM_AGENT_ERROR "AgentCanceled", "The secret agent was disabled");
		tray_call(tray, NM_PATH "/AgentManager", NM_AGENT_MANAGER, "Unregister", NULL, 8000, NULL, NULL);
		agent.registered = FALSE;
		return;
	}
	if (!tray->config.wifi_secret_agent || agent.registered)
		return;
	if (!agent.object) {
		g_autoptr(GError) error = NULL;
		g_autoptr(GDBusNodeInfo) info = g_dbus_node_info_new_for_xml(xml, &error);
		if (info)
			agent.object = g_dbus_connection_register_object(tray_connection(tray), NM_AGENT_PATH, info->interfaces[0],
			                                                 &agent_vtable, tray, NULL, &error);
		if (!agent.object) {
			tray_error(tray, error, "Could not create the NetworkManager secret agent");
			return;
		}
	}
	agent.pending = TRUE;
	tray_call(tray, NM_PATH "/AgentManager", NM_AGENT_MANAGER, "RegisterWithCapabilities",
	          g_variant_new("(su)", "org.mwm.Tray.Wifi", 1u), 8000, registered, GUINT_TO_POINTER(agent.generation));
}

void
wifi_secrets_owner(Tray *tray)
{
	++agent.generation;
	agent.registered = agent.pending = FALSE;
	finish(tray, NM_AGENT_ERROR "AgentCanceled", "NetworkManager disconnected");
	wifi_secrets_configure(tray);
}
