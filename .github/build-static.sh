#!/bin/sh
set -eu

version=$1
arch=$2
case "$version" in
    *[!0-9.]*) echo 'Expected a build version such as 0.1.0.' >&2; exit 1 ;;
esac
printf '%s\n' "$version" | LC_ALL=C grep -Exq '(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}' \
    || { echo 'Expected a build version such as 0.1.0.' >&2; exit 1; }
test "$(uname -m)" = "$arch"

libs=$(pkg-config --static --libs x11 xext xrender xinerama fontconfig freetype2 harfbuzz)
x11_libs=$(pkg-config --static --libs x11)
tray_libs=$(pkg-config --static --libs gio-2.0 libcjson)
make -j4 VERSION="$version" LDFLAGS=-static \
    LIBS="-Wl,--start-group $libs -Wl,--end-group" \
    X11_LIBS="$x11_libs" TRAY_LIBS="-Wl,--start-group $tray_libs -leconf -Wl,--end-group"

mkdir -p dist
for program in mwm mwmc mwm-tray; do
    headers=$(readelf -lW -d "$program")
    if printf '%s\n' "$headers" | grep -E 'INTERP|\(NEEDED\)'; then
        echo "$program links to a shared library or interpreter" >&2
        exit 1
    fi
    install -m 755 "$program" "dist/$program-linux-$arch"
    strip "dist/$program-linux-$arch"
done

xvfb-run -a -s '-screen 0 1280x720x24' \
    sh tests/restart.sh "dist/mwm-linux-$arch" "dist/mwmc-linux-$arch"
xvfb-run -a -s '-screen 0 1280x720x24' dbus-run-session -- \
    sh tests/tray.sh "dist/mwm-linux-$arch" "dist/mwmc-linux-$arch" "dist/mwm-tray-linux-$arch"
xvfb-run -a -s '-screen 0 1280x720x24' \
    python3 tests/displays.py "dist/mwm-linux-$arch" "dist/mwmc-linux-$arch" "dist/mwm-tray-linux-$arch"
