#!/usr/bin/env bash
# 闸门：terminal/apple（full libghostty + host IO 后端）。
#
# 前置：scripts/build-ghostty.sh 已产出补丁版 GhosttyKit.xcframework
# （该产物在 .gitignore 内，不入库）。缺产物时**显式报错**，不假装通过——
# 因为「没打补丁的 libghostty」根本编不出 host 后端，静默降级等于放弃架构。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$ROOT/apple/GhostermTerminal"
TMP="$ROOT/tmp"
mkdir -p "$TMP"

XCFW="$ROOT/build/GhosttyKit.xcframework/macos-arm64"

if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP terminal-gate: 未找到 swift 工具链"
  exit 0
fi

if [ ! -d "$XCFW" ]; then
  echo "terminal-gate: 缺 GhosttyKit.xcframework —— 先跑 scripts/build-ghostty.sh" >&2
  exit 2
fi

# 补丁版特有：头里有 host 字段、库里有 process_output 符号。缺任一说明
# 用的是没打补丁的产物，直接失败（不静默降级）。
if ! grep -q 'void (\*host_write)' "$XCFW/Headers/ghostty.h"; then
  echo "terminal-gate: xcframework 头里没有 host 回调 —— 产物未打补丁" >&2
  exit 2
fi

echo "== TerminalSurface 构建 + 测试（真 libghostty 网格） =="
( cd "$PKG" && swift test ) >"$TMP/terminal-test.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
  echo "terminal-gate: swift test 失败" >&2
  grep -E "error:|Test Case .* failed|panic" "$TMP/terminal-test.log" >&2 | head -30
  exit 1
fi
sed -n 's/^.*Executed \(.*\)/\   执行: \1/p' "$TMP/terminal-test.log" | tail -1

echo "TERMINAL GATE PASS"