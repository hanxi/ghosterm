#!/usr/bin/env bash
# 变异验证：证明**非聚焦窗格变暗**断言（SplitDimTests）有牙。
#
# 这条是「不报错、只在屏幕上不对」的典型：遮罩颜色不对、该关没关、压根没画，
# 都不会抛错。每个变异造出其中一种假绿：
#
#   D1 视图不再画遮罩（RootView 的 overlay 条件写死 false）→ 像素断言必须红；
#   D2 规则不再回落背景色（SplitDim 只用 fill）→ 纯规则 + 接线断言必须红；
#   D3 规则忽略「opacity ≥ 1 = 关掉」→ 对照断言（关掉后不该变暗）必须红。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 后面紧跟全角字符时 bash 会把多字节字节吃进变量名，故一律
# 写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
DIM="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/SplitDim.swift"
VIEW="$ROOT/apple/GhostermApp/Sources/GhostermApp/RootView.swift"
TEST="$ROOT/apple/GhostermApp/Tests/GhostermAppTests/SplitDimTests.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP_DIM=""; BACKUP_VIEW=""; BACKUP_TEST=""

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-split-dim: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-split-dim: 缺 GhosttyKit"; exit 0
fi

restore_all() {
  # 只有现场还在变异态才复原（见 mutation-check-pixel.sh 的同名说明）。
  if ! grep -q "$MARK" "$DIM" "$VIEW" "$TEST" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP_DIM" ] && [ -f "$BACKUP_DIM" ] && cp "$BACKUP_DIM" "$DIM" && rm -f "$BACKUP_DIM"
  [ -n "$BACKUP_VIEW" ] && [ -f "$BACKUP_VIEW" ] && cp "$BACKUP_VIEW" "$VIEW" && rm -f "$BACKUP_VIEW"
  [ -n "$BACKUP_TEST" ] && [ -f "$BACKUP_TEST" ] && cp "$BACKUP_TEST" "$TEST" && rm -f "$BACKUP_TEST"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "$DIM" "$VIEW" "$TEST"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done
SHA_DIM="$(shasum -a 256 "$DIM" | awk '{print $1}')"
SHA_VIEW="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
SHA_TEST="$(shasum -a 256 "$TEST" | awk '{print $1}')"
BACKUP_DIM="$(mktemp "$TMP/gt-mutdim-dim-XXXXXX")"; BACKUP_DIM="$BACKUP_DIM.swift"
BACKUP_VIEW="$(mktemp "$TMP/gt-mutdim-view-XXXXXX")"; BACKUP_VIEW="$BACKUP_VIEW.swift"
BACKUP_TEST="$(mktemp "$TMP/gt-mutdim-test-XXXXXX")"; BACKUP_TEST="$BACKUP_TEST.swift"
cp "$DIM" "$BACKUP_DIM"; cp "$VIEW" "$BACKUP_VIEW"; cp "$TEST" "$BACKUP_TEST"

run_dim() { ( cd "$PKG" && timeout 420 swift test --filter SplitDimTests >/dev/null 2>&1 ); }
run_other() { ( cd "$PKG" && timeout 420 swift test --filter ProgramStatusStoreTests >/dev/null 2>&1 ); }

expect() { # $1=期望 red|green $2=描述 $3=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP_DIM" "$DIM"; cp "$BACKUP_VIEW" "$VIEW"; cp "$BACKUP_TEST" "$TEST"
  local a b c
  a="$(shasum -a 256 "$DIM" | awk '{print $1}')"
  b="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
  c="$(shasum -a 256 "$TEST" | awk '{print $1}')"
  if [ "$a" != "$SHA_DIM" ] || [ "$b" != "$SHA_VIEW" ] || [ "$c" != "$SHA_TEST" ]; then
    echo "  ✗ 回滚后 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1))
  fi
}

echo "== 基线 =="
expect green "变暗断言基线" run_dim
expect green "对照（程序状态结构）基线" run_other

echo "== D1：视图不再画遮罩 =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if tab.isSplit, !isFocused, let dim {"
new = "            if false, let dim { /* GT_MUTATION_MARK：不画遮罩 */"
assert old in s, "D1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "D1 遮罩没画出来（像素断言红）" run_dim
expect green "D1 指向性（程序状态结构不受影响）" run_other
rollback

echo "== D2：规则不再回落背景色（只用 fill） =="
python3 - "$DIM" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        guard let c = fill ?? background else { return nil }"
new = "        guard let c = fill else { return nil } /* GT_MUTATION_MARK：不回落 background */"
assert old in s, "D2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "D2 没设 fill 时不再用背景色（规则/接线断言红）" run_dim
expect green "D2 指向性" run_other
rollback

echo "== D3：忽略「opacity ≥ 1 = 关掉」 =="
python3 - "$DIM" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        guard kept < 1 else { return nil }"
new = "        /* GT_MUTATION_MARK：opacity=1 也照样变暗 */"
assert old in s, "D3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "D3 opacity=1 仍在变暗（对照断言红）" run_dim
expect green "D3 指向性" run_other
rollback

echo "== 回滚复绿 =="
expect green "回滚后变暗断言复绿" run_dim
expect green "回滚后对照仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-split-dim: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
