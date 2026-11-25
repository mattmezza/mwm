#!/bin/sh
# Run on a private X server and bus, for example xvfb-run -a dbus-run-session -- sh tests/tray.sh.
set -eu

export DBUS_SYSTEM_BUS_ADDRESS="${DBUS_SESSION_BUS_ADDRESS:?start a private D-Bus session}"
export G_DEBUG=fatal-warnings
wm=${1:-./mwm}
control=${2:-./mwmc}
tray=${3:-./mwm-tray}
dir=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then
        for log in "$dir"/*.log; do
            printf '%s\n' "$log" >&2
            cat "$log" >&2
        done
    fi
    for pid in ${wm_pid:-} ${services_pid:-} ${wifi_pid:-} ${bt_pid:-} ${bell_pid:-}; do
        kill "$pid" 2>/dev/null || true
    done
    rm -rf "$dir"
}
trap cleanup EXIT

eventually() {
    tries=0
    until "$@" >/dev/null 2>&1; do
        tries=$((tries+1))
        [ "$tries" -lt 45 ] || { printf 'Timed out: %s\n' "$*" >&2; return 1; }
        sleep .1
    done
}

seen() { grep -Fx "$1" "$dir/events.log"; }
icon() { xdotool search --onlyvisible --name "^$1$"; }
panel() { xdotool search --onlyvisible --name "^mwm $1$"; }
no_panel() { ! panel "$1"; }
xpos() { xwininfo -id "$1" | awk '/Relative upper-left X:/ { print $NF }'; }
bell_is_last() {
    bell_x=$(xpos "$bell_id")
    [ "$bell_x" -gt "$(xpos "$wifi_id")" ] && [ "$bell_x" -gt "$(xpos "$bt_id")" ]
}
key() { xdotool key --clearmodifiers "$@"; }
enter() { xdotool type --clearmodifiers "$1"; key Return; }

open() {
    id=$(icon "$1")
    xdotool mousemove --window "$id" 9 9 click 1
    eventually panel "$1"
    sleep .2
}

click_panel() {
    id=$(panel "$1")
    xdotool mousemove --window "$id" "$2" "$3" click 1
    sleep .15
}

ask_nm() {
    gdbus call --system --dest org.freedesktop.NetworkManager --object-path /org/freedesktop/NetworkManager \
        --method org.mwm.Tray.Test.Ask wifi >/dev/null
}

cp "$wm" "$dir/mwm"
cp examples/config.ini "$dir/config.ini"
${PYTHON:-python3} tests/tray-services.py "$dir/events.log" >"$dir/services.log" 2>&1 &
services_pid=$!
eventually seen ready
"$dir/mwm" -c "$dir/config.ini" >"$dir/wm.log" 2>&1 &
wm_pid=$!
eventually "$control" version
"$tray" -c "$dir/config.ini" wifi >"$dir/wifi.log" 2>&1 &
wifi_pid=$!
"$tray" -c "$dir/config.ini" bluetooth >"$dir/bluetooth.log" 2>&1 &
bt_pid=$!
eventually seen nm_registered
eventually seen bt_registered
eventually seen bt_default
eventually icon Wi-Fi
eventually icon Bluetooth
# XEmbed children are viewable while the tray's clip is still opening.
sleep .25

open Wi-Fi
key r
eventually seen wifi_scan
key Down Return
eventually seen wifi_saved
sleep .3
key Down Return
sleep .2
enter test-new-password
eventually seen wifi_new
sleep .3
# The newly joined network becomes the first row, followed by the saved network.
key Down Return
sleep .2
key ctrl+a
xdotool type pwd
key Tab
xdotool type tester
key Tab
enter test-enterprise-password
eventually seen wifi_enterprise
sleep .3
click_panel Wi-Fi 253 133
xdotool type 'Secret SSID'
key Return
sleep .2
enter test-hidden-password
eventually seen wifi_hidden
sleep .3
key Tab Down Return
sleep .2
xdotool type test-vpn-password
key Tab
enter 246810
eventually seen vpn_authenticated
sleep .3
key Return
eventually seen vpn_deactivate
key Escape
eventually no_panel Wi-Fi

ask_nm
eventually panel Wi-Fi
sleep .2
enter test-agent-password
eventually seen wifi_agent
key Escape
eventually no_panel Wi-Fi

ask_nm
eventually panel Wi-Fi
sleep .2
key Escape
eventually seen 'wifi_canceled org.freedesktop.NetworkManager.SecretAgent.UserCanceled'
key Escape
eventually no_panel Wi-Fi

open Bluetooth
key v
eventually seen 'Pairable=on'
eventually seen 'Discoverable=on'
key v
eventually seen 'Discoverable=off'
key Down Return
eventually seen bt_connect_paired
sleep .2
key Return
eventually seen bt_disconnect
key r
eventually seen bt_scan
sleep .2
key Down Return
sleep .2
key Escape
eventually seen 'bt_rejected org.bluez.Error.Rejected'
sleep .2
key Return
sleep .2
key Return
eventually seen bt_confirmed
eventually seen bt_connect_new
sleep .3
key Down Return
sleep .2
enter test-pin
eventually seen bt_pin
key Escape
eventually no_panel Bluetooth

wifi_id=$(icon Wi-Fi)
bt_id=$(icon Bluetooth)
"$tray" -c "$dir/config.ini" notifications >"$dir/bell.log" 2>&1 &
bell_pid=$!
eventually icon Notifications
bell_id=$(icon Notifications)
eventually bell_is_last

printf '\n[tray]\nnotificationslast = false\n' >>"$dir/config.ini"
"$control" reload
test "$(xpos "$bell_id")" -eq 0
printf 'notificationslast = true\n' >>"$dir/config.ini"
"$control" reload
eventually bell_is_last

printf '\n[applets]\nwidth = 410\n' >>"$dir/config.ini"
"$control" reload
open Wi-Fi
width=$(xwininfo -id "$(panel Wi-Fi)" | awk '/Width:/ { print $2 }')
test "$width" -eq 410
key Escape
eventually no_panel Wi-Fi
"$control" restart
eventually icon Wi-Fi
eventually icon Bluetooth
test "$wifi_id" = "$(icon Wi-Fi)"
test "$bt_id" = "$(icon Bluetooth)"
test "$bell_id" = "$(icon Notifications)"
eventually bell_is_last
open Bluetooth
key Escape
eventually no_panel Bluetooth
kill -0 "$wifi_pid" "$bt_pid"

printf '\n[motion]\npopup_enter = 500\npopup_exit = 500\npopup_distance = 30\n' >>"$dir/config.ini"
"$control" reload
"$control" palette
palette() { xdotool search --onlyvisible --name '^mwm command palette$'; }
ypos() { xwininfo -id "$1" | awk '/Absolute upper-left Y:/ { print $NF }'; }
eventually palette
palette_id=$(palette)
before=$(ypos "$palette_id")
sleep .55
settled=$(ypos "$palette_id")
test "$before" -lt "$settled"
"$control" palette toggle
sleep .2
leaving=$(ypos "$palette_id")
test "$leaving" -gt "$settled"
"$control" palette toggle
test "$(palette | tail -1)" != "$palette_id"
sleep .55
if xwininfo -id "$palette_id" >/dev/null 2>&1; then
    exit 1
fi
key Escape
printf 'Wi-Fi, VPN, Bluetooth, notification ordering, config reload, popup motion and tray handoff passed.\n'
