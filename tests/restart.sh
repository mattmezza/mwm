#!/bin/sh
# Run on a private X server, for example xvfb-run -a sh tests/restart.sh.
set -eu

wm=${1:-./mwm}
control=${2:-./mwmc}
dir=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -ne 0 ] && [ -f "$dir/wm.log" ]; then
        cat "$dir/wm.log" >&2
    fi
    for pid in ${wm_pid:-} ${first_pid:-} ${second_pid:-} ${icon_pid:-}; do
        kill "$pid" 2>/dev/null || true
    done
    rm -rf "$dir"
}
trap cleanup EXIT

eventually() {
    i=0
    until "$@" >/dev/null 2>&1; do
        i=$((i + 1))
        [ "$i" -lt 40 ] || { printf 'Timed out: %s\n' "$*" >&2; return 1; }
        sleep .1
    done
}

focused() {
    "$control" query state | grep -F "sel=$1"
}

segment_state() {
    "$control" segment list | grep -Fx "$1 $2"
}

${CC:-cc} -o "$dir/client" tests/restart-client.c -lX11
cp "$wm" "$dir/mwm"
cp examples/config.ini "$dir/config.ini"
printf '\n[bar]\nhidden_segments = system, datetime\n' >>"$dir/config.ini"
"$dir/mwm" -c "$dir/config.ini" >"$dir/wm.log" 2>&1 &
wm_pid=$!
eventually "$control" version
segment_state system hidden
segment_state datetime hidden
segment_state power shown
"$control" segment toggle system
segment_state system shown
"$control" segment hide power

cp "$dir/config.ini" "$dir/valid.ini"
printf '\nhidden_segments = unknown\n' >>"$dir/config.ini"
if "$control" reload >"$dir/reply" 2>&1; then exit 1; fi
grep -F 'unknown or repeated name in hidden_segments' "$dir/reply"
segment_state system shown
mv "$dir/valid.ini" "$dir/config.ini"
printf '\nsegments = tags, title | | status, power, datetime, system, tray\n' >>"$dir/config.ini"

"$dir/client" first >"$dir/first" &
first_pid=$!
eventually focused first
"$control" win move 67 28
"$control" win toggle sticky
"$control" focus tag 2
"$dir/client" second >"$dir/second" &
second_pid=$!
eventually focused second
"$control" win move 70 45
"$control" win toggle full
"$dir/client" tray >"$dir/icon" &
icon_pid=$!
tray_docked() {
    [ -s "$dir/icon" ] || return 1
    parent=$(xwininfo -id "$(cat "$dir/icon")" -tree | awk '/Parent window id:/ { print $4 }')
    [ -n "$parent" ] || return 1
    xprop -id "$parent" WM_CLASS | grep -F 'mwm-tray'
}
eventually tray_docked
tray_colors() {
    xprop -id "$1" -f _NET_SYSTEM_TRAY_COLORS 32x _NET_SYSTEM_TRAY_COLORS |
        grep -Fx "_NET_SYSTEM_TRAY_COLORS(CARDINAL) = $2"
}
tray_docked >/dev/null
tray_before=$parent
tray_colors "$tray_before" '0x7a7a, 0xa2a2, 0xf7f7, 0xffff, 0x5252, 0x6666, 0xe0e0, 0xafaf, 0x6868, 0x9e9e, 0xcece, 0x6a6a'
xprop -root -f RESOURCE_MANAGER 8s -set RESOURCE_MANAGER 'mwm.selbg: #123456
mwm.systemdanger: #de3041
mwm.systemwarn: #c98720
mwm.traysuccess: #2aaf79'
"$control" reload
segment_state system shown
segment_state datetime hidden
segment_state power hidden
tray_docked >/dev/null
test "$parent" = "$tray_before"
reloaded_colors='0x1212, 0x3434, 0x5656, 0xdede, 0x3030, 0x4141, 0xc9c9, 0x8787, 0x2020, 0x2a2a, 0xafaf, 0x7979'
tray_colors "$tray_before" "$reloaded_colors"

"$control" gap 31
"$control" layout stack
"$control" master ratio .61
"$control" traybar hide
for topic in windows tags monitors state; do
    "$control" query "$topic" >"$dir/$topic"
done
xwininfo -id "$(cat "$dir/first")" >"$dir/geometry"
old=$(stat -Lc %i "/proc/$wm_pid/exe")
cp "$wm" "$dir/new"
mv -f "$dir/new" "$dir/mwm"
printf '\nhidden_segments = system\n' >>"$dir/config.ini"
"$control" restart
test "$old" != "$(stat -Lc %i "/proc/$wm_pid/exe")"
segment_state system shown
segment_state datetime shown
segment_state power hidden
for topic in windows tags monitors state; do
    "$control" query "$topic" | diff -u "$dir/$topic" -
done
xwininfo -id "$(cat "$dir/first")" | diff -u "$dir/geometry" -
xwininfo -id "$(cat "$dir/icon")" | grep 'IsUnviewable'
tray_docked >/dev/null
tray_colors "$parent" "$reloaded_colors"
"$control" traybar show
tray_viewable() {
    xwininfo -id "$(cat "$dir/icon")" | grep -q 'IsViewable'
}
eventually tray_viewable
"$control" win toggle full
"$control" query windows | grep -F 'float=1 full=0 sticky=0 focused=1 "second"'
printf '\nhidden_segments = datetime\n' >>"$dir/config.ini"
"$control" reload
segment_state system shown
segment_state datetime hidden
segment_state power hidden

printf 'int main(void) { return 0; }\n' | ${CC:-cc} -x c -o "$dir/bad" -
mv -f "$dir/bad" "$dir/mwm"
if "$control" restart; then exit 1; fi
focused second
cp "$wm" "$dir/mwm"
printf '\n[invalid-section]\n' >>"$dir/config.ini"
if "$control" restart; then exit 1; fi
focused second

"$control" quit
wait "$wm_pid"
xwininfo -id "$(cat "$dir/first")" | grep 'IsViewable'
xwininfo -id "$(cat "$dir/second")" | grep 'IsViewable'
