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
make -j4 VERSION="$version" LDFLAGS=-static \
    LIBS="-Wl,--start-group $libs -Wl,--end-group" \
    X11_LIBS="$x11_libs"

mkdir -p dist
for program in mwm mwmc; do
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
