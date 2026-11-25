#include "tray.h"

#include <stdlib.h>
#include <string.h>

#define NOTIFICATIONS "org.freedesktop.Notifications"
#define NOD_PATH "/org/freedesktop/Notifications"
#define NOD_DND "org.nod.Dnd"
#define NOD_HISTORY "org.nod.History"

typedef struct {
	guint subscription;
	gboolean dnd_unsupported, status_pending, status_again, history_known, history_pending, history_again;
} NodState;

static void status(Tray *);
static void history(Tray *);

static gboolean
unsupported(GError *error)
{
	return g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE) ||
	       g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD);
}

static void
update_status(Tray *tray, GVariant *reply, GError *error)
{
	NodState *state = tray->backend;
	if (reply && g_variant_is_of_type(reply, G_VARIANT_TYPE("(b)"))) {
		gboolean muted;
		g_variant_get(reply, "(b)", &muted);
		if (!tray->available) {
			tray->available = TRUE;
			tray_nod_claim(tray, TRUE);
			history(tray);
		} else if (tray->enabled == muted) {
			return;
		}
		tray->enabled = muted;
		tray_model(tray, "Do not disturb", muted ? "Banners are paused" : "Banners are on");
	} else if (unsupported(error)) {
		state->dnd_unsupported = TRUE;
		tray_model(tray, "nod unavailable", "The notification service does not support nod controls");
	} else if (tray->busy) {
		tray_error(tray, error, "Cannot change Do Not Disturb");
	}
	tray_changed(tray);
}

static void
status_done(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))
		return;
	NodState *state = tray->backend;
	if (!data)
		state->status_pending = FALSE;
	/* While a toggle is in flight its reply owns the state. */
	if (data || !tray->busy)
		update_status(tray, reply, error);
	if (data) {
		tray->busy = FALSE;
		tray_changed(tray);
	}
	if (state->status_again) {
		state->status_again = FALSE;
		status(tray);
	}
}

static void
status(Tray *tray)
{
	NodState *state = tray->backend;
	if (!tray->connected || state->dnd_unsupported)
		return;
	if (state->status_pending || tray->busy) {
		state->status_again = TRUE;
		return;
	}
	state->status_pending = TRUE;
	tray_call(tray, NOD_PATH, NOD_DND, "Status", NULL, 2000, status_done, NULL);
}

static gboolean
poll_status(gpointer data)
{
	Tray *tray = data;
	NodState *state = tray->backend;
	if (!tray->history_available)
		status(tray);
	if (tray->available && !state->history_known && !state->history_pending)
		history(tray);
	return G_SOURCE_CONTINUE;
}

static char *
notification_detail(const char *app, guint64 received)
{
	g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(received / G_USEC_PER_SEC);
	g_autoptr(GDateTime) now = g_date_time_new_now_local();
	if (!when)
		return g_strdup(app);
	gboolean today = g_date_time_get_year(when) == g_date_time_get_year(now) &&
	                 g_date_time_get_day_of_year(when) == g_date_time_get_day_of_year(now);
	g_autofree char *time = g_date_time_format(when, today ? "%H:%M" : "%b %e · %H:%M");
	return *app ? g_strdup_printf("%s · %s", app, time) : g_strdup(time);
}

static gboolean
unseen_head(Tray *tray, GVariant *list)
{
	if (!g_variant_n_children(list))
		return FALSE;
	g_autoptr(GVariant) first = g_variant_get_child_value(list, 0);
	guint32 id;
	guint64 received;
	g_variant_get_child(first, 0, "u", &id);
	g_variant_get_child(first, 1, "t", &received);
	char key[16];
	g_snprintf(key, sizeof key, "%u", id);
	for (guint i = 0; i < tray->rows->len; ++i) {
		TrayEntry *row = tray->rows->pdata[i];
		if (row->received == received && !strcmp(row->id, key))
			return FALSE;
	}
	return TRUE;
}

static void
history_done(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)data;
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))
		return;
	NodState *state = tray->backend;
	state->history_pending = FALSE;
	if (reply && g_variant_is_of_type(reply, G_VARIANT_TYPE("(a(utysss))"))) {
		state->history_known = tray->history_available = TRUE;
		g_autoptr(GVariant) list = g_variant_get_child_value(reply, 0);
		gboolean latest_is_new = unseen_head(tray, list);
		g_ptr_array_set_size(tray->rows, 0);
		GVariantIter items;
		g_variant_iter_init(&items, list);
		guint32 id;
		guint64 received;
		guchar urgency;
		const char *app, *summary, *body;
		while (g_variant_iter_loop(&items, "(uty&s&s&s)", &id, &received, &urgency, &app, &summary, &body)) {
			char key[16];
			g_snprintf(key, sizeof key, "%u", id);
			g_autofree char *detail = notification_detail(app, received);
			TrayEntry *row = tray_entry(tray, key, *summary ? summary : *app ? app : "Notification", detail,
			                           TrayDevice, urgency == 2, FALSE, 0);
			row->body = g_strdup(body);
			row->received = received;
		}
		if (latest_is_new && !tray_panel_open(tray))
			tray->unread = TRUE;
		if (!tray->rows->len)
			tray->unread = FALSE;
	} else if (unsupported(error)) {
		state->history_known = TRUE;
		tray->history_available = FALSE;
	} else if (error) {
		tray_error(tray, error, "Cannot read notification history");
	}
	tray_changed(tray);
	if (state->history_again) {
		state->history_again = FALSE;
		history(tray);
	}
}

static void
history(Tray *tray)
{
	NodState *state = tray->backend;
	if (!tray->available || (state->history_known && !tray->history_available))
		return;
	if (state->history_pending) {
		state->history_again = TRUE;
		return;
	}
	state->history_pending = TRUE;
	tray_call(tray, NOD_PATH, NOD_HISTORY, "List", NULL, 2000, history_done, NULL);
}

static void
state_changed(GDBusConnection *bus, const char *sender, const char *path, const char *interface,
              const char *signal, GVariant *args, gpointer data)
{
	(void)bus; (void)path; (void)signal; (void)args;
	Tray *tray = data;
	if (g_strcmp0(tray->owner, sender))
		return;
	if (!strcmp(interface, NOD_DND))
		status(tray);
	else if (!strcmp(interface, NOD_HISTORY))
		history(tray);
}

static void
appeared(GDBusConnection *bus, const char *name, const char *owner, gpointer data)
{
	(void)name;
	Tray *tray = data;
	NodState *state = tray->backend;
	tray->session = g_object_ref(bus);
	tray->owner = g_strdup(owner);
	tray->connected = TRUE;
	state->subscription = g_dbus_connection_signal_subscribe(bus, owner, NULL, "Changed", NOD_PATH,
	                                                         NULL, G_DBUS_SIGNAL_FLAGS_NONE,
	                                                         state_changed, tray, NULL);
	status(tray);
}

static void
vanished(GDBusConnection *bus, const char *name, gpointer data)
{
	(void)bus; (void)name;
	Tray *tray = data;
	NodState *state = tray->backend;
	if (state->subscription)
		g_dbus_connection_signal_unsubscribe(tray->session, state->subscription);
	*state = (NodState){0};
	g_clear_object(&tray->session);
	g_clear_pointer(&tray->owner, g_free);
	tray_nod_claim(tray, FALSE);
	tray->connected = tray->available = tray->enabled = tray->busy = tray->unread = tray->history_available = FALSE;
	g_ptr_array_set_size(tray->rows, 0);
	tray_model(tray, "nod unavailable", "Waiting for the notification daemon");
	tray_status(tray, FALSE, "");
}

void
nod_start(Tray *tray)
{
	tray->backend = g_new0(NodState, 1);
	g_bus_watch_name(G_BUS_TYPE_SESSION, NOTIFICATIONS, G_BUS_NAME_WATCHER_FLAGS_NONE, appeared, vanished, tray, NULL);
	/* nod builds before Changed was added still support Status. */
	g_timeout_add_seconds(2, poll_status, tray);
}

static void
history_changed(Tray *tray, GVariant *reply, GError *error, gpointer data)
{
	(void)data;
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CLOSED))
		return;
	if (!reply)
		tray_error(tray, error, "Cannot update notification history");
}

void
nod_action(Tray *tray, enum TrayAction action, const char *id)
{
	if (!tray->available)
		return;
	if (action == TrayToggle) {
		tray->busy = TRUE;
		tray_call(tray, NOD_PATH, NOD_DND, "Toggle", NULL, 2000, status_done, GINT_TO_POINTER(1));
	} else if (action == TrayClear && tray->history_available) {
		tray_call(tray, NOD_PATH, NOD_HISTORY, "Clear", NULL, 2000, history_changed, NULL);
	} else if (action == TrayForget && tray->history_available && id) {
		tray_call(tray, NOD_PATH, NOD_HISTORY, "Remove", g_variant_new("(u)", (guint32)strtoul(id, NULL, 10)),
		          2000, history_changed, NULL);
	}
}
