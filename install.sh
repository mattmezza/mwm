#!/bin/sh
set -eu

repo=https://github.com/mattmezza/mwm

fail() {
    printf 'mwm installer: %s\n' "$*" >&2
    exit 1
}

download() {
    curl --proto '=https' --proto-redir '=https' -fsSL --retry 3 "$@"
}

main() {
    restart=0
    if [ "${1:-}" = --restart ]; then
        restart=1
        shift
    fi
    [ "$#" -le 1 ] || fail "usage: install.sh [--restart] [vMAJOR.MINOR.PATCH]"
    case "$(uname -s):$(uname -m)" in
        Linux:x86_64) platform=linux-x86_64 ;;
        Linux:aarch64|Linux:arm64) platform=linux-aarch64 ;;
        *) fail "no binary for $(uname -s) $(uname -m)" ;;
    esac

    tag=${1:-}
    if [ -z "$tag" ]; then
        latest=$(curl --proto '=https' -fsS --retry 3 -o /dev/null -w '%{redirect_url}' "$repo/releases/latest")
        case "$latest" in
            "$repo/releases/tag/"*) tag=${latest##*/} ;;
            *) fail "could not find the latest published release" ;;
        esac
    fi
    case "$tag" in
        *[!v0-9.]*) fail "expected a release tag such as v0.1.0" ;;
    esac
    printf '%s\n' "$tag" | LC_ALL=C grep -Exq 'v(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){2}' \
        || fail "expected a release tag such as v0.1.0"

    bin_dir=${MWM_BIN_DIR:-"$HOME/.local/bin"}
    mkdir -p -- "$bin_dir"
    for program in mwm mwmc; do
        [ ! -d "$bin_dir/$program" ] || fail "$bin_dir/$program is a directory"
    done
    umask 077
    stage=$(mktemp -d "$bin_dir/.mwm-install.XXXXXXXX")
    trap 'rm -rf -- "$stage"' EXIT
    trap 'exit 1' HUP INT TERM
    base="$repo/releases/download/$tag"

    download "$base/SHA256SUMS" -o "$stage/SHA256SUMS"
    for program in mwm mwmc; do
        asset="$program-$platform"
        digest=$(awk -v asset="$asset" '$2 == asset { print $1 }' "$stage/SHA256SUMS")
        [ "${#digest}" -eq 64 ] || fail "missing or ambiguous checksum for $asset"
        case "$digest" in *[!0-9a-f]*) fail "invalid checksum for $asset" ;; esac
        download "$base/$asset" -o "$stage/$program"
        (cd "$stage" && printf '%s  %s\n' "$digest" "$program" | sha256sum -c -)
        chmod 755 "$stage/$program"
    done

    # Renaming within the destination directory keeps a running binary intact.
    mv -f -- "$stage/mwmc" "$bin_dir/mwmc"
    mv -f -- "$stage/mwm" "$bin_dir/mwm"
    printf 'Installed mwm %s in %s\n' "$tag" "$bin_dir"
    if [ "$restart" -eq 1 ]; then
        "$bin_dir/mwmc" restart
    fi
}

main "$@"
