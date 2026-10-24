#!/usr/bin/env bash
# 只构建 C 核心（快速路径，不跑对拍）。完整闸门见 scripts/gate.sh。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cmake -S "$ROOT/core" -B "$ROOT/core/build" -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build "$ROOT/core/build"
