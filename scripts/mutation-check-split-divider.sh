#!/usr/bin/env bash
# 变异验证：证明「分屏分隔线不画、拖拽手柄保留」这组断言有牙
# （SplitDividerTests）。
#
# 工作区采用原生透明拖拽手柄；卡片已有边框，手柄不能再画一条背景线。
#   D1 分隔位置命令被短路 → 拖动不改变布局比例；
#   D2 给手柄设置灰色背景 → 中缝再次出现额外线；
#   D3 藏掉手柄本体 → 拖拽命中和调比例失效。
# 横竖两个方向均验，几何套件作为独立对照。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap 复原（EXIT 也
# 复原：被 kill -9 时 INT/TERM 不跑，变异会留在源码里）；回滚后比对起跑前
# sha256。注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-split-divider

TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
ROOTVIEW="$ROOT/apple/GhostermApp/Sources/GhostermApp/WorkspaceView.swift"
SPLITDIV="$ROOTVIEW"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
declare -a FILES=("$ROOTVIEW")
declare -a BACKUPS=() SHAS=()

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-split-divider: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-split-divider: 缺 GhosttyKit"; exit 0
fi

restore_all() {
  if ! grep -q "$MARK" "${FILES[@]}" 2>/dev/null; then return 0; fi
  local i
  for i in "${!FILES[@]}"; do
    [ -n "${BACKUPS[$i]:-}" ] && [ -f "${BACKUPS[$i]}" ] \
      && cp "${BACKUPS[$i]}" "${FILES[$i]}" && rm -f "${BACKUPS[$i]}"
  done
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "${FILES[@]}"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done

for i in "${!FILES[@]}"; do
  SHAS[$i]="$(shasum -a 256 "${FILES[$i]}" | awk '{print $1}')"
  b="$(mktemp "$TMP/gt-mutdivider-XXXXXX")"; BACKUPS[$i]="$b.swift"
  cp "${FILES[$i]}" "${BACKUPS[$i]}"
done

run_divider()  { ( cd "$PKG" && gt_timeout 420 swift test --filter SplitDividerTests >/dev/null 2>&1 ); }
run_geometry() { ( cd "$PKG" && gt_timeout 420 swift test --filter SplitPaneGeometryTests >/dev/null 2>&1 ); }
expect() {
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  local i now
  for i in "${!FILES[@]}"; do cp "${BACKUPS[$i]}" "${FILES[$i]}"; done
  for i in "${!FILES[@]}"; do
    now="$(shasum -a 256 "${FILES[$i]}" | awk '{print $1}')"
    [ "$now" = "${SHAS[$i]}" ] || {
      echo "  ✗ 回滚后 ${FILES[$i]} 的 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1)); }
  done
}

echo "== 基线 =="
expect green "分隔线断言基线" run_divider
expect green "对照（分屏几何）基线" run_geometry

# ---- D1：分隔位置命令被短路 ----
echo "== D1：分隔手柄不改变比例 =="
python3 - "$ROOTVIEW" <<'PYD1'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        resizeDivider(index, at: point)'
n = s.count(old)
assert n == 1, f"D1 锚点数不对（{n} != 1）"
s = s.replace(old, '        /* GT_MUTATION_MARK：分隔位置命令被短路 */')
open(p, 'w').write(s)
PYD1
expect red "D1 拖拽调比例失效" run_divider
expect green "D1 指向性（分屏几何不受影响）" run_geometry
rollback

# ---- D2：手柄设置可见背景 ----
echo "== D2：透明手柄又画出背景线 =="
python3 - "$SPLITDIV" <<'PYD2'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        layer?.backgroundColor = nil'
new = '        layer?.backgroundColor = NSColor.gray.cgColor /* GT_MUTATION_MARK：手柄绘制灰线 */'
assert old in s, "D2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYD2
expect red "D2 手柄背景线被画出" run_divider
rollback

# ---- D3：藏掉整个 divider view（拖拽一起死） ----
echo "== D3：拿 isHidden 当修法 =="
python3 - "$SPLITDIV" <<'PYD3'
import sys
p = sys.argv[1]; s = open(p).read()
old = '            view.isHidden = i >= dividerGeometry.count'
new = '            view.isHidden = true /* GT_MUTATION_MARK：整个藏掉 */'
assert old in s, "D3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYD3
expect red "D3 拖拽手柄被藏（断言不许）" run_divider
rollback

echo "== 回滚复绿 =="
expect green "回滚后分隔线断言复绿" run_divider
expect green "回滚后分屏几何断言仍绿" run_geometry

restore_all
echo
echo "mutation-check-split-divider: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
