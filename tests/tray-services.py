"""Minimal NetworkManager and BlueZ services for the private X11 integration test."""

import os
import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib

NM = "org.freedesktop.NetworkManager"
NM_PATH = "/org/freedesktop/NetworkManager"
BLUEZ = "org.bluez"
ADAPTER = "org.bluez.Adapter1"
DEVICE = "org.bluez.Device1"
BT_AGENT = "org.bluez.AgentManager1"
BT_PATH = "/org/bluez/hci0"
HEADSET = BT_PATH + "/dev_00_00_00_00_00_01"
KEYBOARD = BT_PATH + "/dev_00_00_00_00_00_02"
PIN_DEVICE = BT_PATH + "/dev_00_00_00_00_00_03"
WIFI = NM + ".Device.Wireless"
WIFI_PATH = NM_PATH + "/Devices/1"
AP = NM + ".AccessPoint"
HOME, SECURED, SCHOOL = [NM_PATH + f"/AccessPoint/{n}" for n in range(1, 4)]
PROFILE = NM + ".Settings.Connection"
SAVED, VPN = [NM_PATH + f"/Settings/{n}" for n in range(1, 3)]
ACTIVE = NM + ".Connection.Active"
NM_AGENT = NM + ".AgentManager"
PROPERTIES = "org.freedesktop.DBus.Properties"
OBJECTS = "org.freedesktop.DBus.ObjectManager"
TEST = "org.mwm.Tray.Test"

DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
nm_owner = dbus.service.BusName(NM, bus)
bluez_owner = dbus.service.BusName(BLUEZ, bus)
state = {NM: {}, BLUEZ: {}}
agents = {}
serial = 0


def event(text):
    with open(sys.argv[1], "a", encoding="utf-8") as log:
        print(text, file=log, flush=True)


def paths(*items):
    return dbus.Array([dbus.ObjectPath(p) for p in items], signature="o")


class Object(dbus.service.Object):
    def __init__(self, owner, path, entries=None):
        super().__init__(owner, path)
        self.path = path
        self.owner = owner.get_name()
        self.entries = entries or {}
        if entries:
            state[self.owner][path] = self.entries

    @dbus.service.method(PROPERTIES, in_signature="ssv", out_signature="")
    def Set(self, interface, key, value):
        self.entries[interface][key] = value
        self.PropertiesChanged(interface, {key: value}, [])
        event(f"{key}={'on' if value else 'off'}")

    @dbus.service.signal(PROPERTIES, signature="sa{sv}as")
    def PropertiesChanged(self, interface, changes, invalid):
        pass

    @dbus.service.method(OBJECTS, out_signature="a{oa{sa{sv}}}")
    def GetManagedObjects(self):
        return state[self.owner]

    @dbus.service.signal(OBJECTS, signature="oa{sa{sv}}")
    def InterfacesAdded(self, path, entries):
        pass


class AccessDevice(Object):
    @dbus.service.method(WIFI, in_signature="a{sv}")
    def RequestScan(self, _options):
        self.entries[WIFI]["LastScan"] = dbus.Int64(self.entries[WIFI]["LastScan"] + 1)
        self.PropertiesChanged(WIFI, {"LastScan": self.entries[WIFI]["LastScan"]}, [])
        event("wifi_scan")

    @dbus.service.method(NM + ".Device")
    def Disconnect(self):
        self.entries[WIFI]["ActiveAccessPoint"] = dbus.ObjectPath("/")
        self.PropertiesChanged(WIFI, {"ActiveAccessPoint": dbus.ObjectPath("/")}, [])
        event("wifi_disconnect")


class Settings(Object):
    def __init__(self, path, settings):
        super().__init__(nm_owner, path, {PROFILE: {"VersionId": dbus.UInt64(1)}})
        self.settings = settings

    @dbus.service.method(PROFILE, out_signature="a{sa{sv}}")
    def GetSettings(self):
        return self.settings


def agent_call(kind, method, *args, reply_handler, error_handler):
    owner, path, interface = agents[kind]
    proxy = bus.get_object(owner, path, introspect=False)
    proxy.get_dbus_method(method, interface)(*args, reply_handler=reply_handler,
                                            error_handler=error_handler, timeout=15)


def credentials(kind, flags, on_success, on_error):
    if kind == "vpn":
        profile, setting = VPN, "vpn"
        settings = {
            "connection": {"id": "Test VPN", "uuid": "00000000-0000-0000-0000-000000000002"},
            "vpn": {"service-type": "org.mwm.test.vpn"},
        }
        keys = ["password", "otp"]
    else:
        profile, setting = SAVED, "802-11-wireless-security"
        settings = {
            "connection": {"id": "Saved wireless", "uuid": "00000000-0000-0000-0000-000000000001"},
            setting: {"key-mgmt": "wpa-psk"},
        }
        keys = ["psk"]
    agent_call(NM, "GetSecrets", dbus.Dictionary(settings, signature="sa{sv}"), dbus.ObjectPath(profile), setting,
               dbus.Array(keys, signature="s"), dbus.UInt32(flags), reply_handler=on_success, error_handler=on_error)


class Network(Object):
    @dbus.service.method(NM, in_signature="ooo", out_signature="o")
    def ActivateConnection(self, profile, device, ap):
        del device
        active = self.activate(profile, ap, profile == VPN)
        if profile == VPN:
            def accepted(secrets):
                valid = secrets["vpn"]["secrets"] == {"password": "test-vpn-password", "otp": "246810"}
                event("vpn_authenticated" if valid else "wrong_vpn_credentials")
                active.entries[ACTIVE]["State"] = dbus.UInt32(2)
                active.PropertiesChanged(ACTIVE, {"State": dbus.UInt32(2)}, [])
            credentials("vpn", 3, accepted, lambda err: event(f"vpn_error {err.get_dbus_name()}"))
        else:
            event("wifi_saved")
        return active.path

    @dbus.service.method(NM, in_signature="a{sa{sv}}ooa{sv}", out_signature="ooa{sv}")
    def AddAndActivateConnection2(self, profile, device, ap, _options):
        assert device == WIFI_PATH
        security = profile.get("802-11-wireless-security", {})
        if ap == SECURED:
            assert security == {"key-mgmt": "wpa-psk", "psk": "test-new-password"}
            assert bytes(profile["802-11-wireless"]["ssid"]) == b"Secured wireless"
            event("wifi_new")
        elif ap == SCHOOL:
            assert security["key-mgmt"] == "wpa-eap"
            eap = profile["802-1x"]
            assert eap["eap"] == ["pwd"] and eap["identity"] == "tester"
            assert eap["password"] == "test-enterprise-password"
            event("wifi_enterprise")
        elif ap == "/":
            assert profile["802-11-wireless"]["hidden"]
            assert bytes(profile["802-11-wireless"]["ssid"]) == b"Secret SSID"
            assert security["psk"] == "test-hidden-password"
            event("wifi_hidden")
        else:
            raise AssertionError("Unexpected Wi-Fi activation")
        active = self.activate(SAVED, ap, False)
        return SAVED, active.path, {}

    def activate(self, profile, ap, needs_password):
        global serial
        serial += 1
        path = NM_PATH + f"/ActiveConnection/{serial}"
        active = Object(nm_owner, path, {ACTIVE: {"Connection": dbus.ObjectPath(profile),
                                                "State": dbus.UInt32(1 if needs_password else 2)}})
        children.append(active)
        nm_root.InterfacesAdded(path, active.entries)
        current = self.entries[NM]["ActiveConnections"]
        self.entries[NM]["ActiveConnections"] = paths(*current, path)
        self.PropertiesChanged(NM, {"ActiveConnections": self.entries[NM]["ActiveConnections"]}, [])
        if ap != "/":
            radio.entries[WIFI]["ActiveAccessPoint"] = dbus.ObjectPath(ap)
            radio.PropertiesChanged(WIFI, {"ActiveAccessPoint": dbus.ObjectPath(ap)}, [])
        return active

    @dbus.service.method(NM, in_signature="o")
    def DeactivateConnection(self, active):
        event("vpn_deactivate" if active.startswith(NM_PATH + "/ActiveConnection/") else "wrong_vpn")
        current = self.entries[NM]["ActiveConnections"]
        self.entries[NM]["ActiveConnections"] = paths(*(p for p in current if p != active))
        self.PropertiesChanged(NM, {"ActiveConnections": self.entries[NM]["ActiveConnections"]}, [])

    @dbus.service.method(TEST, in_signature="s")
    def Ask(self, kind):
        def accepted(value):
            valid = value["802-11-wireless-security"]["psk"] == "test-agent-password"
            event("wifi_agent" if valid else "wrong_network_credential")
        credentials(kind, 3, accepted, lambda err: event(f"wifi_canceled {err.get_dbus_name()}"))


class NMManager(Object):
    @dbus.service.method(NM_AGENT, in_signature="su", sender_keyword="sender")
    def RegisterWithCapabilities(self, identifier, capabilities, sender=None):
        assert identifier == "org.mwm.Tray.Wifi" and capabilities & 1
        agents[NM] = (sender, NM_PATH + "/SecretAgent", NM + ".SecretAgent")
        event("nm_registered")

    @dbus.service.method(NM_AGENT)
    def Unregister(self):
        agents.pop(NM, None)


class BlueZManager(Object):
    @dbus.service.method(BT_AGENT, in_signature="os", sender_keyword="sender")
    def RegisterAgent(self, path, capability, sender=None):
        assert capability == "KeyboardDisplay"
        agents[BLUEZ] = (sender, path, "org.bluez.Agent1")
        event("bt_registered")

    @dbus.service.method(BT_AGENT, in_signature="o")
    def RequestDefaultAgent(self, _path):
        event("bt_default")

    @dbus.service.method(BT_AGENT, in_signature="o")
    def UnregisterAgent(self, _path):
        agents.pop(BLUEZ, None)


class BlueAdapter(Object):
    @dbus.service.method(ADAPTER)
    def StartDiscovery(self):
        event("bt_scan")

    @dbus.service.method(ADAPTER)
    def StopDiscovery(self):
        event("bt_scan_stop")

    @dbus.service.method(ADAPTER, in_signature="o")
    def RemoveDevice(self, device):
        assert device == HEADSET
        event("bt_forget")


class BlueDevice(Object):
    def change(self, key, value):
        self.entries[DEVICE][key] = value
        self.PropertiesChanged(DEVICE, {key: value}, [])

    @dbus.service.method(DEVICE)
    def Connect(self):
        self.change("Connected", True)
        event("bt_connect_paired" if self.path == HEADSET else "bt_connect_new")

    @dbus.service.method(DEVICE)
    def Disconnect(self):
        self.change("Connected", False)
        event("bt_disconnect")

    @dbus.service.method(DEVICE, async_callbacks=("success", "failure"))
    def Pair(self, success, failure):
        def paired(*code):
            if self.path == PIN_DEVICE:
                assert code == ("test-pin",)
                event("bt_pin")
            else:
                assert not code
                event("bt_confirmed")
            self.change("Paired", True)
            success()
        def declined(error):
            event(f"bt_rejected {error.get_dbus_name()}")
            failure(dbus.exceptions.DBusException("The user declined pairing", name="org.bluez.Error.Rejected"))
        path = dbus.ObjectPath(self.path)
        if self.path == PIN_DEVICE:
            agent_call(BLUEZ, "RequestPinCode", path, reply_handler=paired, error_handler=declined)
        else:
            agent_call(BLUEZ, "RequestConfirmation", path, dbus.UInt32(827194),
                       reply_handler=paired, error_handler=declined)

    @dbus.service.method(DEVICE)
    def CancelPairing(self):
        event("bt_pair_cancel")


nm_root = Object(nm_owner, "/org/freedesktop")
nm = Network(nm_owner, NM_PATH, {NM: {"WirelessEnabled": True, "WirelessHardwareEnabled": True,
                                     "PrimaryConnection": dbus.ObjectPath("/"), "ActiveConnections": paths(),
                                     "Connectivity": dbus.UInt32(4)}})
nm_manager = NMManager(nm_owner, NM_PATH + "/AgentManager")
radio = AccessDevice(nm_owner, WIFI_PATH, {
    NM + ".Device": {"Managed": True, "DeviceType": dbus.UInt32(2), "Interface": "wlan-mwm",
                     "ActiveConnection": dbus.ObjectPath("/"), "AvailableConnections": paths(SAVED)},
    WIFI: {"ActiveAccessPoint": dbus.ObjectPath("/"), "AccessPoints": paths(HOME, SECURED, SCHOOL),
           "LastScan": dbus.Int64(0)},
})
children = [Object(nm_owner, path, {AP: {"Ssid": dbus.ByteArray(name), "Flags": dbus.UInt32(1),
                                      "Strength": dbus.Byte(strength), "RsnFlags": dbus.UInt32(flags)}})
            for path, name, strength, flags in [(HOME, b"Saved wireless", 79, 0x100),
                                                (SECURED, b"Secured wireless", 65, 0x100),
                                                (SCHOOL, b"School wireless", 61, 0x200)]]
saved = Settings(SAVED, {
    "connection": {"id": "Saved wireless", "type": "802-11-wireless"},
    "802-11-wireless": {"ssid": dbus.ByteArray(b"Saved wireless")},
    "802-11-wireless-security": {"key-mgmt": "wpa-psk"},
})
vpn = Settings(VPN, {"connection": {"id": "Test VPN", "type": "vpn"}})

bt_root = Object(bluez_owner, "/")
bt_manager = BlueZManager(bluez_owner, "/org/bluez")
bt_adapter = BlueAdapter(bluez_owner, BT_PATH, {ADAPTER: {"Powered": True, "Discoverable": False, "Pairable": False}})
for path, name, paired, icon in [(HEADSET, "Headphones", True, "audio-headphones"),
                                  (KEYBOARD, "Keyboard", False, "input-keyboard"),
                                  (PIN_DEVICE, "Printer", False, "printer")]:
    children.append(BlueDevice(bluez_owner, path, {DEVICE: {"Adapter": dbus.ObjectPath(BT_PATH), "Alias": name,
                                                            "Connected": False, "Paired": paired, "Icon": icon}}))
event("ready")
GLib.MainLoop().run()
