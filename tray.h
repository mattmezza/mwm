#ifndef MWM_TRAY_H
#define MWM_TRAY_H

#include <gio/gio.h>
#include "shell.h"

#define PROPERTIES "org.freedesktop.DBus.Properties"

enum TrayKind { TrayWifi, TrayBluetooth };
enum TrayAction { TrayToggle, TrayScan, TrayRow, TrayForget, TrayHidden };
enum TrayIcon { TraySignal, TrayDevice, TrayVpn, TrayIconCount };

typedef struct {
	char *id, *label, *detail;
	enum TrayIcon icon;
	int active, saved, strength;
} TrayEntry;

enum { TrayMaxFields = 8, TrayFieldSize = 512 };
typedef struct {
	char *label, *key;
	char value[TrayFieldSize];
	gboolean secret, digits, required;
} TrayField;
typedef struct {
	char *title, *detail, *accept;
	TrayField fields[TrayMaxFields];
	int count;
} TrayPrompt;

typedef struct Tray Tray;
typedef void (*TrayAnswer)(Tray *, gboolean);
typedef void (*TrayReply)(Tray *, GVariant *, GError *, gpointer);

struct Tray {
	enum TrayKind kind;
	Config config;
	GDBusObjectManagerClient *manager;
	const char *service, *manager_path;
	char *owner;
	guint refresh_id;
	gboolean connected, available, enabled, scanning, can_scan, busy;
	int strength;
	char *headline, *subtitle, *message;
	gboolean failed;
	GPtrArray *rows;
	TrayPrompt prompt;
	unsigned prompt_serial;
	TrayAnswer answer;
	gpointer backend;
	gpointer window;
};

void tray_changed(Tray *);
gboolean tray_popup(Tray *);
void tray_hide(Tray *);
void tray_model(Tray *, const char *headline, const char *subtitle);
TrayEntry *tray_entry(Tray *, const char *id, const char *label, const char *detail,
                      enum TrayIcon, gboolean active, gboolean saved, int strength);
void tray_status(Tray *, gboolean failed, const char *message);
void tray_error(Tray *, GError *, const char *fallback);
void tray_prompt(Tray *, const char *title, const char *detail, const char *accept, TrayAnswer);
void tray_field(Tray *, const char *key, const char *label, const char *value,
                gboolean secret, gboolean digits, gboolean required);
void tray_clear_prompt(Tray *);
void tray_action(Tray *, enum TrayAction, const char *id);

void tray_bus_start(Tray *);
void tray_refresh(Tray *);
GDBusConnection *tray_connection(Tray *);
GDBusProxy *tray_proxy(Tray *, const char *path, const char *interface);
GVariant *tray_property(GDBusProxy *, const char *);
gboolean tray_bool(GDBusProxy *, const char *);
guint tray_uint(GDBusProxy *, const char *);
char *tray_string(GDBusProxy *, const char *);
char *tray_label(const char *);
void tray_call(Tray *, const char *, const char *, const char *, GVariant *,
               int timeout, TrayReply, gpointer);
void tray_set(Tray *, const char *, const char *, const char *, GVariant *, TrayReply, gpointer);

void wifi_owner(Tray *);
void wifi_model(Tray *);
void wifi_action(Tray *, enum TrayAction, const char *);
void wifi_configure(Tray *);
void bluetooth_owner(Tray *);
void bluetooth_model(Tray *);
void bluetooth_action(Tray *, enum TrayAction, const char *);
void bluetooth_configure(Tray *);

#endif
