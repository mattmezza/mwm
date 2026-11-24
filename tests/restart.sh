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

${CC:-cc} -o "$dir/client" tests/restart-client.c -lX11
cp "$wm" "$dir/mwm"
cp examples/config.ini "$dir/config.ini"
"$dir/mwm" -c "$dir/config.ini" >"$dir/wm.log" 2>&1 &
wm_pid=$!
eventually "$control" version

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
"$control" restart
test "$old" != "$(stat -Lc %i "/proc/$wm_pid/exe")"
for topic in windows tags monitors state; do
    "$control" query "$topic" | diff -u "$dir/$topic" -
done
xwininfo -id "$(cat "$dir/first")" | diff -u "$dir/geometry" -
xwininfo -id "$(cat "$dir/icon")" | grep 'IsUnviewable'
"$control" traybar show
xwininfo -id "$(cat "$dir/icon")" | grep 'IsViewable'
"$control" win toggle full
"$control" query windows | grep -F 'float=1 full=0 sticky=0 focused=1 "second"'

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
