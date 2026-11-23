#!/bin/sh
set -eu

# CI fills these literals in the versioned bootstrap. The installed copy uses
# the running gterm to authenticate the update index.
bootstrap_version='@GTERM_VERSION@'
bootstrap_macos_arm64='@MACOS_ARM64@'
bootstrap_linux_x86_64='@LINUX_X86_64@'
bootstrap_linux_arm64='@LINUX_ARM64@'
repository=https://github.com/hanxi/ghosterm/releases/download

die() {
    printf 'gterm: %s\n' "$*" >&2
    exit 1
}

valid_version() {
    case "$1" in *[!abcdefghijklmnopqrstuvwxyz0-9.-]*) return 1 ;; esac
    printf '%s\n' "$1" | LC_ALL=C grep -Ex 'gterm-v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)' >/dev/null
}

valid_digest() {
    [ "${#1}" -eq 64 ] && case "$1" in *[!0123456789abcdef]*) false;; *) true;; esac
}

download() {
    curl --fail --silent --show-error --location --proto '=https' --proto-redir '=https' \
        --connect-timeout 20 --max-time 300 --max-filesize "$3" --output "$2" "$1"
}

digest_file() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum < "$1"
    else
        shasum -a 256 < "$1"
    fi
}

check_payload() {
    executable=$1/bin/gterm
    [ "$("$executable" _distribution-platform)" = "$platform" ] ||
        die "release executable does not match $platform"
    "$executable" doctor
}

replace_link() {
    if [ "$(uname -s)" = Darwin ]; then
        mv -fh "$1" "$2"
    else
        mv -fT "$1" "$2"
    fi
}

mode=install
if [ "${1:-}" = upgrade ] || [ "${1:-}" = rollback ]; then
    mode=$1
    verifier=$2
    shift 2
    prefix=$(cd "$(dirname "$verifier")/../../../../.." && pwd -P)
    installed_version=${verifier#"$prefix/lib/gterm/versions/"}
    installed_version=${installed_version%/bin/gterm}
    valid_version "$installed_version" || die 'gterm was not installed with the versioned bootstrap'
    [ "$verifier" = "$prefix/lib/gterm/versions/$installed_version/bin/gterm" ] ||
        die 'invalid running installation path'
else
    prefix=${HOME:?}/.local
    if [ "${1:-}" = --prefix ] && [ "$#" -eq 2 ]; then
        prefix=$2
        shift 2
    fi
fi
[ "$#" -eq 0 ] || die 'usage: install-gterm.sh [--prefix DIRECTORY] | gterm upgrade | gterm rollback'

case "$(uname -s):$(uname -m)" in
    Darwin:arm64) platform=macos-arm64; digest=$bootstrap_macos_arm64 ;;
    Linux:x86_64) platform=linux-x86_64; digest=$bootstrap_linux_x86_64 ;;
    Linux:aarch64|Linux:arm64) platform=linux-arm64; digest=$bootstrap_linux_arm64 ;;
    *) die 'unsupported operating system or architecture' ;;
esac
if [ "$mode" = install ]; then
    valid_version "$bootstrap_version" && valid_digest "$digest" ||
        die 'versioned bootstrap is unavailable'
fi

umask 077
mkdir -p -- "$prefix"
prefix=$(cd "$prefix" && pwd -P)
state=$prefix/lib/gterm
entry=$prefix/bin/gterm
entry_target=../lib/gterm/current/bin/gterm
mkdir -p -- "$state/versions" "$state/activations" "$prefix/bin"
lock=$state/write-lock
mkdir -- "$lock" 2>/dev/null || die "another installation holds $lock"
stage=
cleanup() {
    if [ -n "$stage" ]; then rm -rf -- "$stage"; fi
    rmdir -- "$lock"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
stage=$(mktemp -d "$state/.stage-XXXXXX")

if [ -e "$entry" ] || [ -L "$entry" ]; then
    [ "$(readlink "$entry")" = "$entry_target" ] || die "existing entry is unmanaged: $entry"
fi

current=
if [ -L "$state/current" ]; then
    current=$(cat "$state/current/version")
    valid_version "$current" || die 'invalid active version'
fi
case "$mode" in
    install) version=$bootstrap_version ;;
    upgrade)
        [ -n "$current" ] || die 'no active installation'
        download "$repository/gterm-stable/index" "$stage/index" 8192
        download "$repository/gterm-stable/index.sig" "$stage/index.sig" 64
        "$verifier" _verify-update-index "$stage/index" "$stage/index.sig" ||
            die 'update index signature verification failed'
        IFS=' ' read -r channel version extra < "$stage/index"
        [ "$channel" = gterm-stable-v1 ] && valid_version "$version" && [ -z "$extra" ] ||
            die 'invalid update index'
        digest=$(awk -v platform="$platform" '$1 == platform && NF == 2 { print $2 }' "$stage/index")
        valid_digest "$digest" || die 'missing or duplicate platform digest'
        newest=$(printf '%s\n%s\n' "$current" "$version" |
            awk -F. '{ version = $0; sub(/^gterm-v/, "", $1); print $1, $2, $3, version }' |
            LC_ALL=C sort -k1,1n -k2,2n -k3,3n | tail -1)
        [ "${newest##* }" = "$version" ] || die 'update index would downgrade the active version'
        ;;
    rollback)
        [ -f "$state/current/previous" ] || die 'no previous version'
        version=$(cat "$state/current/previous")
        valid_version "$version" && [ "$version" != "$current" ] || die 'no previous version'
        ;;
esac

target=$state/versions/$version
if [ "$mode" != rollback ]; then
    if [ -d "$target" ]; then
        [ "$(cat "$target/asset.sha256")" = "$digest" ] || die "asset changed for $version"
    else
        asset=gterm-$platform.tar.gz
        download "$repository/$version/$asset" "$stage/$asset" 134217728
        actual=$(digest_file "$stage/$asset")
        [ "${actual%% *}" = "$digest" ] || die 'release asset digest mismatch'
        mkdir "$stage/payload"
        tar -xzf "$stage/$asset" -C "$stage/payload"
        check_payload "$stage/payload"
        printf '%s\n' "$digest" > "$stage/payload/asset.sha256"
        mv -- "$stage/payload" "$target"
    fi
fi
check_payload "$target"

if [ "$version" != "$current" ]; then
    activation=$(mktemp -d "$state/activations/run-XXXXXX")
    ln -s "../../versions/$version/bin" "$activation/bin"
    printf '%s\n' "$version" > "$activation/version"
    if [ -n "$current" ]; then printf '%s\n' "$current" > "$activation/previous"; fi
    ln -s "activations/${activation##*/}" "$stage/current"
    replace_link "$stage/current" "$state/current"
fi
if [ ! -L "$entry" ]; then
    ln -s "$entry_target" "$entry"
fi
printf 'Installed %s at %s\n' "$version" "$entry"
case ":${PATH:-}:" in
    *":$prefix/bin:"*) ;;
    *) printf 'Add %s to PATH to run gterm by name.\n' "$prefix/bin" ;;
esac
