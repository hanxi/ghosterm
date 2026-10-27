#!/usr/bin/env bash
# Build the pinned session engine in tmp/; publish build/GhosttyKit.xcframework.
# Vendor source is read-only. --restore remains accepted: no source needs restoring.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 原生探针里的 cc 链接也必须走 Xcode SDK；CLT 27.0 的 TBD 经典 ld 读不了。
if [ "$(uname -s)" = Darwin ]; then
  SYSROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
  [ -n "$SYSROOT" ] && export SDKROOT="$SYSROOT"
fi
if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --restore ]; }; then
  echo "Usage: bash scripts/build-ghostty.sh [--restore]" >&2; exit 2
fi
python3 "$ROOT/scripts/build-session-engine.py"
. "$ROOT/scripts/lib/ghostty-themes.sh"
THEMES_SRC="$(ghostty_themes_find "$ROOT/tmp/worker-surface/engine/zig-pkg")"
N="$(ghostty_themes_stage "$THEMES_SRC" "$ROOT/tmp/ghostty-resources/ghostty/themes")"
echo "BUILD-GHOSTTY PASS: isolated source, build/ SDK, ${N} themes"
