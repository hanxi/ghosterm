#!/usr/bin/env bash
# 变异验证：证明 gate-ghostty-patch 的符号断言**有牙**。
#
# 变异手法不是"改坏源码"（重新构建一版补丁库要几十分钟），而是**把
# xcframework 里的静态库换成无补丁符号的替身**，断言必须变红。
# 同一变异下另有一条断言（头文件 host 字段）必须**仍绿**——双断言证明
# 「红有指向性」，不是整个闸门崩了。
#
# 纪律（AGENTS.md）：起跑前扫残留标记；trap 复原；回滚后比对起跑前 sha256。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
VENDOR="$ROOT/vendor/ghostty"
LIB="$ROOT/build/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a"
MARK="GT_GHOSTTY_MUTATION"
PASS=0; FAIL=0

if [ ! -f "$LIB" ]; then
  echo "SKIP mutation-check-ghostty-patch: 无 ${LIB}（先跑 scripts/build-ghostty.sh）"
  exit 0
fi

if [ -e "$LIB.$MARK" ]; then
  echo "检测到残留变异产物（$LIB.${MARK}）——工作区不干净，拒绝起跑（exit 2）" >&2
  exit 2
fi

BASE_SHA="$(shasum -a 256 "$LIB" | awk '{print $1}')"

restore() { [ -e "$LIB.$MARK" ] && mv -f "$LIB.$MARK" "$LIB"; }
trap 'restore' EXIT
trap 'restore; echo "已复原（trap）"' INT TERM

gate_out() { bash "$ROOT/scripts/gate-ghostty-patch.sh" 2>&1; }
expect_gate() {
  local actual
  if gate_out >/dev/null 2>&1; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → 闸门 $actual"; PASS=$((PASS+1))
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}

echo "== 基线 =="
expect_gate green "基线"

echo "== M1：替换为无补丁符号的替身静态库 =="
mv "$LIB" "$LIB.$MARK"
printf 'int gt_unrelated_symbol(void){return 0;}\n' > "$TMP/_ghostty_mut_probe.c"
clang -c "$TMP/_ghostty_mut_probe.c" -o "$TMP/_ghostty_mut_probe.o"
rm -f "$LIB" && ar rcs "$LIB" "$TMP/_ghostty_mut_probe.o"
expect_gate red "M1 库中无补丁符号"

# 双断言：同一变异下，符号断言红（有指向性），头文件断言仍绿（没被连带打崩）。
out="$(gate_out || true)"
if echo "$out" | grep -q "缺导出 ghostty_surface_process_output"; then
  echo "  ✓ 红落在符号断言（缺导出 ghostty_surface_process_output）"; PASS=$((PASS+1))
else
  echo "  ✗ 未看到符号断言的定向失败输出" >&2; FAIL=$((FAIL+1))
fi
if echo "$out" | grep -q "头文件声明 host_write"; then
  echo "  ✓ 同一变异下头文件断言仍绿（红有指向性）"; PASS=$((PASS+1))
else
  echo "  ✗ 头文件断言被连带打崩——红无指向性" >&2; FAIL=$((FAIL+1))
fi

echo "== 回滚复绿 =="
restore
now="$(shasum -a 256 "$LIB" | awk '{print $1}')"
if [ "$now" = "$BASE_SHA" ]; then echo "  ✓ 回滚后 sha256 一致"; PASS=$((PASS+1))
else echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); fi
expect_gate green "回滚后"

trap - INT TERM
echo
echo "mutation-check-ghostty-patch: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]