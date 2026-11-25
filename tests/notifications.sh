#!/bin/sh
# Test against a history-capable nod binary on a private X server and session bus.
set -eu
if [ "${1:-}" != --isolated ]; then
    nod=$(realpath "${1:?usage: tests/notifications.sh /path/to/nod}")
    exec xvfb-run -a -s '-screen 0 1024x768x24' dbus-run-session -- sh "$0" --isolated "$nod"
fi
cd "$(dirname "$0")/.."
nod=$2
dir=$(mktemp -d)
wm=
daemon=
applet=
cleanup() {
    result=$?
    trap - EXIT
    for pid in "$applet" "$daemon" "$wm"; do
        [ -z "$pid" ] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }
    done
    if [ "$result" -ne 0 ]; then cat "$dir"/*.log >&2; fi
    rm -rf "$dir"
    exit "$result"
}
trap cleanup EXIT

eventually() {
    tries=0
    until "$@"; do
        tries=$((tries+1))
        [ "$tries" -lt 50 ] || { printf 'Timed out: %s\n' "$*" >&2; return 1; }
        sleep .1
    done
}

call() {
    gdbus call --session --dest org.freedesktop.Notifications \
        --object-path /org/freedesktop/Notifications --method "$@"
}
history() { call org.nod.History.List; }
has() { history | grep -qF "$1"; }
absent() { entries=$(history) && ! printf '%s\n' "$entries" | grep -qF "$1"; }
dnd_state() { [ "$(call org.nod.Dnd.Status)" = "($1,)" ]; }
notify() { call org.freedesktop.Notifications.Notify Tests "$1" '' "$2" "$3" '[]' "$4" 250; }
native() { xdotool search --onlyvisible --name '^nod: click to toggle do not disturb$' >/dev/null; }
no_native() { ! native; }
bell() { xdotool search --onlyvisible --classname '^mwm-notifications-icon$'; }
panel() { xdotool search --onlyvisible --name '^mwm Notifications$'; }
no_panel() { ! panel >/dev/null; }
click() { xdotool mousemove --window "$1" "$2" "$3" click "$4"; }
height() { xwininfo -id "$(panel)" | awk '/Height:/ { print $2 }'; }
detail_open() { [ "$(height)" -ne "$list_height" ]; }
detail_closed() { [ "$(height)" -eq "$list_height" ]; }

cp ./mwm "$dir/mwm"
cp examples/config.ini "$dir/config.ini"
"$dir/mwm" -c "$dir/config.ini" >"$dir/wm.log" 2>&1 &
wm=$!
eventually ./mwmc version
"$nod" >"$dir/nod.log" 2>&1 &
daemon=$!
eventually native
call org.nod.Dnd.On >/dev/null
./mwm-tray -c "$dir/config.ini" notifications >"$dir/applet.log" 2>&1 &
applet=$!
eventually no_native
id=$(eventually bell)
sleep .25

notify 0 'Calendar reminder' 'Planning starts in five minutes' '{}' >/dev/null
receipt=$(notify 0 'Build started' 'Waiting for results' '{}')
notification_id=$(printf '%s' "$receipt" | sed -nE 's/^\(uint32 ([0-9]+),\)$/\1/p')
[ -n "$notification_id" ]
[ "$(notify "$notification_id" 'Build complete' 'All checks passed' "{'urgency': <byte 2>}")" = "$receipt" ]
notify 0 'Transient message' 'Do not retain' "{'transient': <true>}" >/dev/null
has 'Build complete'
absent 'Build started'
absent 'Transient message'

click "$id" 9 9 1
eventually panel >/dev/null
sleep .2
list_height=$(height)
xdotool key --clearmodifiers Down Return
eventually detail_open
xdotool key --clearmodifiers Left
eventually detail_closed
xdotool key --clearmodifiers Delete
eventually absent 'Build complete'
has 'Calendar reminder'
click "$(panel)" 320 87 1
eventually absent 'Calendar reminder'
xdotool key --clearmodifiers Escape
eventually no_panel

click "$id" 9 9 3
eventually dnd_state false
click "$id" 9 9 3
eventually dnd_state true
./mwmc restart
[ "$(eventually bell)" = "$id" ]
kill "$daemon"
wait "$daemon"
"$nod" >"$dir/nod.log" 2>&1 &
daemon=$!
eventually call org.nod.Dnd.Status >/dev/null 2>&1
call org.nod.Dnd.Off >/dev/null
sleep .3
click "$id" 9 9 3
eventually dnd_state true
kill "$applet"
wait "$applet"
applet=
eventually native

if grep -E 'CRITICAL|CUI frame failed|rendering failed' "$dir"/*.log; then exit 1; fi
printf 'PASS: nod history, DND, XEmbed fallback, applet input, and mwm restart\n'
