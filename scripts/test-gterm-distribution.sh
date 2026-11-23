#!/usr/bin/env bash
# Exercise the actual packaged executable with a controlled HTTPS transport.
set -euo pipefail
root=$(git rev-parse --show-toplevel)
work=${1:?pass an absolute test directory}
archive=${2:?pass a platform release archive}
mkdir -p "$work/mock" "$work/web" "$work/home"
export HOME="$work/home" TMPDIR="$work" TMP="$work" TEMP="$work"
export GTERM_TEST_WEB="$work/web"
export PATH="$work/mock:$PATH"
tag1=gterm-v0.0.1
tag2=gterm-v0.0.2
platform=${archive##*/gterm-}
platform=${platform%.tar.gz}
stable="$work/web/gterm-stable"
mkdir "$work/web/$tag1" "$work/web/$tag2" "$stable"
cp "$archive" "$work/web/$tag1/"
cp "$archive" "$work/web/$tag2/"
python3 "$root/scripts/prepare-gterm-release.py" "$tag1" "$work/web/$tag1" --platform "$platform"
python3 "$root/scripts/prepare-gterm-release.py" "$tag2" "$work/web/$tag2" --platform "$platform"
cat > "$work/mock/curl" <<'CURL'
#!/bin/sh
set -eu
out=
url=
while [ "$#" -gt 0 ]; do
    case "$1" in
        --output) out=$2; shift ;;
        https://github.com/hanxi/ghosterm/releases/download/*)
            url=${1#https://github.com/hanxi/ghosterm/releases/download/} ;;
    esac
    shift
done
case "${GTERM_TEST_DOWNLOAD:-}" in
    pause)
        : > "$GTERM_TEST_WEB/downloading"
        while [ ! -e "$GTERM_TEST_WEB/resume" ]; do sleep 0.1; done ;;
    interrupted) printf 'incomplete' > "$out"; exit 18 ;;
    corrupt-asset)
        case "$url" in *.tar.gz) printf 'corrupt' > "$out"; exit 0 ;; esac ;;
esac
cp "$GTERM_TEST_WEB/$url" "$out"
CURL
chmod +x "$work/mock/curl"

bootstrap="$work/web/$tag1/install-gterm.sh"
sh "$bootstrap"
entry="$HOME/.local/bin/gterm"
test -x "$entry"
"$entry" doctor
custom="$work/prefix with spaces"
sh "$bootstrap" --prefix "$custom"
"$custom/bin/gterm" doctor
current() { cat "$HOME/.local/lib/gterm/current/version"; }
first=$(readlink "$HOME/.local/lib/gterm/current")
sh "$bootstrap"
test "$(readlink "$HOME/.local/lib/gterm/current")" = "$first"
if "$entry" rollback; then exit 1; fi
if GTERM_TEST_DOWNLOAD=interrupted sh "$bootstrap" --prefix "$work/interrupted"; then exit 1; fi
test ! -e "$work/interrupted/bin/gterm"
if GTERM_TEST_DOWNLOAD=corrupt-asset sh "$bootstrap" --prefix "$work/corrupt"; then exit 1; fi
test ! -e "$work/corrupt/bin/gterm"
mkdir -p "$work/wrong/bin" "$work/web/gterm-v0.0.3"
cat > "$work/wrong/bin/gterm" <<'WRONG'
#!/bin/sh
case "$1" in
    _distribution-platform) printf 'unsupported-test-architecture\n' ;;
    *) exit 99 ;;
esac
WRONG
chmod +x "$work/wrong/bin/gterm"
tar -C "$work/wrong" -czf "$work/web/gterm-v0.0.3/gterm-$platform.tar.gz" bin
python3 "$root/scripts/prepare-gterm-release.py" gterm-v0.0.3 "$work/web/gterm-v0.0.3" --platform "$platform"
if sh "$work/web/gterm-v0.0.3/install-gterm.sh" --prefix "$work/wrong-arch"; then exit 1; fi
test ! -e "$work/wrong-arch/bin/gterm"
export XDG_DATA_HOME="$work/xdg"
"$entry" plugin list
"$entry" plugin versions
test -d "$XDG_DATA_HOME/gterm/catalog" && test -d "$XDG_DATA_HOME/gterm/packages"
if [[ ${3:-} == --bootstrap-only ]]; then
    printf 'PASS production bootstrap on %s\n' "$platform"
    exit 0
fi
cp "$work/web/$tag2/index" "$stable/index"
printf 'invalid' > "$stable/index.sig"
if "$entry" upgrade; then exit 1; fi
test "$(current)" = "$tag1"

# Signature uses a disposable CI test key passed via standard input.
openssl pkeyutl -sign -rawin -inkey /dev/stdin -in "$work/web/$tag2/index-message" \
    -out "$stable/index.sig"
if GTERM_TEST_DOWNLOAD=interrupted "$entry" upgrade; then exit 1; fi
test "$(current)" = "$tag1"
if GTERM_TEST_DOWNLOAD=corrupt-asset "$entry" upgrade; then exit 1; fi
test "$(current)" = "$tag1"
GTERM_TEST_DOWNLOAD=pause "$entry" upgrade &
running=$!
trap 'touch "$GTERM_TEST_WEB/resume"; wait "$running" || true' EXIT
for attempt in {1..100}; do
    if [[ -e "$work/web/downloading" ]]; then break; fi
    sleep 0.1
done
test -e "$work/web/downloading"
if "$entry" upgrade; then exit 1; fi
test "$(current)" = "$tag1"
touch "$work/web/resume"
wait "$running"
trap - EXIT
test "$(current)" = "$tag2"
"$entry" upgrade
"$entry" rollback
test "$(current)" = "$tag1"
"$custom/bin/gterm" upgrade
"$custom/bin/gterm" rollback
test "$(cat "$custom/lib/gterm/current/version")" = "$tag1"
test ! -e "$HOME/.local/lib/gterm/write-lock"
printf 'PASS distribution on %s\n' "$platform"
