#ifndef MWM_TRAY_WIFI_H
#define MWM_TRAY_WIFI_H

#include "tray.h"

#define NM "org.freedesktop.NetworkManager"
#define NM_PATH "/org/freedesktop/NetworkManager"
#define NM_WIFI NM ".Device.Wireless"
#define NM_DEVICE NM ".Device"
#define NM_AP NM ".AccessPoint"
#define NM_ACTIVE NM ".Connection.Active"
#define NM_PROFILE NM ".Settings.Connection"
#define WIFI_SETTING "802-11-wireless"
#define WIFI_SECURITY "802-11-wireless-security"

GVariant *wifi_setting(GVariant *, const char *);
const char *wifi_value(GVariant *, const char *);
const char *wifi_psk_key(const char *key_mgmt, guint wep_index);
void wifi_secrets_configure(Tray *);
void wifi_secrets_owner(Tray *);
void wifi_show(Tray *);

#endif
