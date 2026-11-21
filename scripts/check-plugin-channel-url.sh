#!/usr/bin/env bash
# Verify the exact HTTPS endpoint configured in a plugin-enabled release.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -lt 3 ] || [ "$#" -gt 5 ] ||
  { [ "$#" -eq 4 ] && [ "$4" = --compare-directory ]; } ||
  { [ "$#" -eq 5 ] && [ "$4" != --compare-directory ]; }; then
  echo "Usage: $0 CHANNEL ROOT_KEY HTTPS_DIRECTORY [STATE_FILE | --compare-directory DIRECTORY]" >&2
  exit 2
fi
CHANNEL="${1:?expected channel name}"
ROOT_KEY="${2:?expected channel public key}"
DIRECTORY="${3:?expected HTTPS channel directory}"
STATE_ARGS=()
if [ "$#" -eq 4 ]; then STATE_ARGS=(--record-state "$4"); fi
COMPARE_DIRECTORY=""
if [ "$#" -eq 5 ]; then COMPARE_DIRECTORY="${5:?expected comparison directory}"; fi

GHOSTERM_PLUGIN_CHANNEL="$CHANNEL" GHOSTERM_PLUGIN_CHANNEL_ROOT="$ROOT_KEY" \
  GHOSTERM_PLUGIN_CHANNEL_URL="$DIRECTORY" \
  python3 -B "$ROOT/scripts/configure-macos-plugin-channel.py" --check-required

mkdir -p "$ROOT/tmp"
TMP_ROOT="$(cd "$ROOT/tmp" && pwd -P)"
SCRATCH="$TMP_ROOT"
if [ -n "${TMPDIR:-}" ] && [ -d "$TMPDIR" ]; then
  REQUESTED_TMP="$(cd "$TMPDIR" && pwd -P)"
  case "$REQUESTED_TMP" in
    "$TMP_ROOT"/*) SCRATCH="$REQUESTED_TMP" ;;
  esac
fi
WORK="$(mktemp -d "$SCRATCH/channel-url-XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP

for entry in channel-policy.json:65536 channel-policy.sig:64 policy.json:131072; do
  name="${entry%:*}"
  limit="${entry##*:}"
  status="$(
    # Older curl versions need a separate bound for responses without Content-Length.
    set +o posix
    ulimit -Sf "$(( (limit + 1023) / 1024 ))" || exit 1
    exec curl --disable --silent --show-error --proto '=https' --globoff \
      --connect-timeout 10 --max-time 30 --max-filesize "$limit" \
      --output "$WORK/$name" --write-out '%{http_code}' "${DIRECTORY%/}/$name"
  )" || {
    echo "Plugin channel download failed for $name (maximum $limit bytes)" >&2
    exit 1
  }
  if [ "$status" != 200 ]; then
    echo "Plugin channel returned HTTP $status for $name" >&2
    exit 1
  fi
  size="$(wc -c < "$WORK/$name")"
  if [ "$size" -gt "$limit" ]; then
    echo "Plugin channel response exceeds $limit bytes for $name" >&2
    exit 1
  fi
  if [ -n "$COMPARE_DIRECTORY" ]; then
    cmp "$COMPARE_DIRECTORY/$name" "$WORK/$name"
  fi
done

python3 -B "$ROOT/scripts/gterm-plugin" release verify-policy \
  --channel "$CHANNEL" --channel-root "$ROOT_KEY" \
  --statement "$WORK/channel-policy.json" --signature "$WORK/channel-policy.sig" \
  --policy "$WORK/policy.json" ${STATE_ARGS[@]+"${STATE_ARGS[@]}"}
