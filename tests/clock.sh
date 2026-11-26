#!/bin/sh
# Run on a private X server, for example xvfb-run -a sh tests/clock.sh.
set -eu

wm=${1:-./mwm}
control=${2:-./mwmc}
dir=$(mktemp -d)
cleanup() {
    status=$?
    if [ "$status" -ne 0 ]; then cat "$dir/wm.log" >&2; fi
    if [ -n "${pid:-}" ]; then kill "$pid" 2>/dev/null || true; fi
    rm -rf "$dir"
}
trap cleanup EXIT

eventually() {
    i=0
    until "$@" >/dev/null 2>&1; do
        i=$((i + 1))
        [ "$i" -lt 60 ] || { printf 'Timed out: %s\n' "$*" >&2; return 1; }
        sleep .1
    done
}

panel() { xdotool search --onlyvisible --name '^mwm clock$'; }
height() { xwininfo -id "$id" -shape | awk '/Window shape extents:/ { split($4, size, /[x+]/); print size[2] }'; }
same_window() {
    [ "$(panel)" = "$id" ] &&
        [ "$(xwininfo -id "$id" | awk '/Absolute upper-left|Width:|Height:/')" = "$geometry" ]
}

cp "$wm" "$dir/mwm"
cat > "$dir/hook" <<EOF
#!/bin/sh
printf '%s\n' "\$@" > "$dir/alert"
EOF
chmod +x "$dir/hook"
cat > "$dir/config.ini" <<EOF
[bar]
segments = | | datetime
[motion]
reduced_motion = true
[clock]
timer_hook = $dir/hook
EOF

"$dir/mwm" -c "$dir/config.ini" >"$dir/wm.log" 2>&1 &
pid=$!
eventually "$control" version
screen_width=$(xwininfo -root | awk '/Width:/ { print $2 }')
xdotool mousemove "$((screen_width-32))" 21 click 1
eventually panel
id=$(panel)
geometry=$(xwininfo -id "$id" | awk '/Absolute upper-left|Width:|Height:/')
collapsed=$(height)
xdotool key Return
sleep .15
same_window
[ "$(height)" -gt "$collapsed" ]
xdotool key 0 colon 0 0 colon 0 4 Tab
xdotool type 'Take a break'
xdotool key Return
sleep .15
same_window
[ "$(height)" -eq "$collapsed" ]
"$control" restart
eventually grep -Fx 'Take a break' "$dir/alert"
[ "$(cat "$dir/alert")" = "$(printf 'Timer complete\nTake a break')" ]
