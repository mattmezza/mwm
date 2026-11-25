#!/bin/sh
set -eu

case "${BUILD_UID:-}:${BUILD_GID:-}" in
    *[!0-9:]*|:*|*:) echo 'Set numeric BUILD_UID and BUILD_GID.' >&2; exit 1 ;;
esac

# The private D-Bus test session resolves the caller in the container's user database.
if ! getent group "$BUILD_GID" >/dev/null; then
    printf 'mwm-builder:x:%s:\n' "$BUILD_GID" >> /etc/group
fi
if ! getent passwd "$BUILD_UID" >/dev/null; then
    printf 'mwm-builder:x:%s:%s:Build user:/tmp:/bin/sh\n' "$BUILD_UID" "$BUILD_GID" >> /etc/passwd
fi
exec su-exec "$BUILD_UID:$BUILD_GID" sh .github/build-static.sh "$@"
