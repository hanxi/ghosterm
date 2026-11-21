#!/usr/bin/env bash
# Verify the exact HTTPS endpoint configured in a plugin-enabled release.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CHANNEL="${1:?expected channel name}"
ROOT_KEY="${2:?expected channel public key}"
DIRECTORY="${3:?expected HTTPS channel directory}"
STATE_ARGS=()
if [ "$#" -eq 4 ]; then STATE_ARGS=(--record-state "$4"); fi

GHOSTERM_PLUGIN_CHANNEL="$CHANNEL" GHOSTERM_PLUGIN_CHANNEL_ROOT="$ROOT_KEY" \
  GHOSTERM_PLUGIN_CHANNEL_URL="$DIRECTORY" \
  python3 -B "$ROOT/scripts/configure-macos-plugin-channel.py" --check-required

mkdir -p "$ROOT/tmp"
WORK="$(mktemp -d "$ROOT/tmp/channel-url-XXXXXX")"
trap 'rm -rf -- "$WORK"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for entry in channel-policy.json:65536 channel-policy.sig:64 policy.json:131072; do
  name="${entry%:*}"
  limit="${entry##*:}"
  status="$(curl --disable --silent --show-error --proto '=https' --globoff \
    --connect-timeout 10 --max-time 30 --max-filesize "$limit" \
    --output "$WORK/$name" --write-out '%{http_code}' "${DIRECTORY%/}/$name")"
  if [ "$status" != 200 ]; then
    echo "Plugin channel returned HTTP $status for $name" >&2
    exit 1
  fi
done

python3 -B "$ROOT/scripts/gterm-plugin" release verify-policy \
  --channel "$CHANNEL" --channel-root "$ROOT_KEY" \
  --statement "$WORK/channel-policy.json" --signature "$WORK/channel-policy.sig" \
  --policy "$WORK/policy.json" ${STATE_ARGS[@]+"${STATE_ARGS[@]}"}
