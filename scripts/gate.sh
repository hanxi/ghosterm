#!/usr/bin/env bash
# 离线验收闸门。无网、无平台工具链时可跑。缺工具链的端由各自脚本诚实 SKIP。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
mkdir -p "$ROOT/tmp"

fail() { echo "GATE FAIL: $*" >&2; exit 1; }

echo "== 1/4 构建 C 核心（-Werror） =="
# 钉 SDK：见 scripts/package-mac.sh 与 build-core.sh 的等价注释。CommandLineTools 的
# 27.0 SDK 与经典 ld/tapi 不兼容，经典 ld 解析不了它的 libSystem.B.tbd。
SYSROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
SYSROOT_FLAG=()
[ -n "$SYSROOT" ] && SYSROOT_FLAG=(-DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0)
cmake -S core -B core/build -DCMAKE_BUILD_TYPE=Debug "${SYSROOT_FLAG[@]}" >/dev/null
cmake --build core/build >/dev/null
echo "   ok"

echo "== 2/4 单元/对拍测试 =="
ctest --test-dir core/build --output-on-failure >"$ROOT/tmp/gt_ctest.log" 2>&1 || {
  cat "$ROOT/tmp/gt_ctest.log" >&2
  fail "ctest 未全绿"
}
grep -E "tests passed|tests failed" "$ROOT/tmp/gt_ctest.log" | sed 's/^/   /' 

echo "== 3/4 C ABI 符号对拍 =="
bash scripts/check-abi.sh

echo "== 4/4 未实现入口不得静默成功 =="
core/build/test_api | grep -q "0 failed" || fail "api_smoke 失败"

echo "GATE PASS"
