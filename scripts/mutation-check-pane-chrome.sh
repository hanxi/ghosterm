#!/usr/bin/env bash
# 变异验证：证明「窗格外观（间距 / 标题栏）、缩放、只有一个加号」那批断言
# 有牙（PaneChromeTests / PaneZoomTests / TabStripVisibilityTests）。
#
# 这几条全是「不报错、只在屏幕上不对」的改动：
#   P1 间距偏好读了但没留白（padding 加在错的层）→ 两格还是贴着；
#   P2 窗格标题栏开关没接上（关了照画）；
#   P3/P4 导航显隐与唯一加号迁到 tab-chrome 的 navigation 组。
#   P5 缩放把被缩放那格的位置**塌掉** → 兄弟窗格跟着重排（终端那边是一轮
#      SIGWINCH，回卷内容被揉烂）；
#   P6 关掉被缩放那格后不清缩放状态 → 覆盖层盖出一片空白；
#   P7 缩放还原时重置布局比例 → 用户拖出来的比例丢了；缩放期间兄弟尺寸仍正确。
#   P8 单个窗格宿主不放弃安全区 → 侧栏展开时终端顶部出现额外空白。
#   P9 工作区重复避让标题栏 → 几何顶上多出一条留白。
#      旧根宿主安全区变异在直接子视图架构下已等价，改验实际几何偏移。
#   P10 聚焦那格的标题栏不点强调色 → 分屏时「哪格是当前」只剩一圈描边，
#       深色外观下读不出来（用户实测报回来的那条）；
#   P11 点色时不看分不分屏 → 只有一格时标题栏也染上强调色（纯噪音）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-pane-chrome

TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
CHROME="$ROOT/apple/GhostermApp/Sources/GhostermApp/PaneChrome.swift"
ROOTVIEW="$ROOT/apple/GhostermApp/Sources/GhostermApp/RootView.swift"
STRIP="$ROOT/apple/GhostermApp/Sources/GhostermApp/TabStrip.swift"
APPEAR="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/PaneAppearance.swift"
TAB="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TerminalTab.swift"
WORKSPACE="$ROOT/apple/GhostermApp/Sources/GhostermApp/WorkspaceView.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
declare -a FILES=("$CHROME" "$ROOTVIEW" "$STRIP" "$APPEAR" "$TAB" "$WORKSPACE")
declare -a BACKUPS=() SHAS=()

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-pane-chrome: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-pane-chrome: 缺 GhosttyKit"; exit 0
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
  b="$(mktemp "$TMP/gt-mutpane-XXXXXX")"; BACKUPS[$i]="$b.swift"
  cp "${FILES[$i]}" "${BACKUPS[$i]}"
done

run_pixels() { ( cd "$PKG" && gt_timeout 420 swift test --filter PaneChromeTests >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
run_zoom()   { ( cd "$PKG" && gt_timeout 420 swift test --filter PaneZoomTests >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
# P7 要证明「比例丢了」与「几何塌了」是两条不同的红，故各自单跑一条。
run_zoom_ratio() { ( cd "$PKG" && gt_timeout 420 swift test \
  --filter 'PaneZoomTests/testZoomRoundTripKeepsDraggedDividerRatio' >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
run_zoom_sibling() { ( cd "$PKG" && gt_timeout 420 swift test \
  --filter 'PaneZoomTests/testSiblingGeometryIsUntouchedByZoom' >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
run_strip()  { ( cd "$PKG" && gt_timeout 420 swift test --filter TabStripVisibilityTests >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
# P8/P9 量的是「窗格离窗口顶多远」，与量「两格之间空多少」的间距那条互为指向性。
run_top_inset() { ( cd "$PKG" && gt_timeout 420 swift test \
  --filter 'PaneChromeTests/testSplitPanesReachWindowTopWhenSidebarIsOpen' >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
run_pane_gap() { ( cd "$PKG" && gt_timeout 420 swift test \
  --filter 'PaneChromeTests/testGapIsRealWhitespaceBetweenPanes' >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
# P10/P11 互为指向性：一个管「分屏时点上」，一个管「单格时别点」。
run_focus_tint() { ( cd "$PKG" && gt_timeout 420 swift test \
  --filter 'PaneChromeTests/testFocusedPaneTitleBarIsTinted' >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
run_lone_tint() { ( cd "$PKG" && gt_timeout 420 swift test \
  --filter 'PaneChromeTests/testLonePaneTitleBarIsNotTinted' >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
run_other()  { ( cd "$PKG" && gt_timeout 420 swift test --filter TabManagerTests >"$TMP/pane-chrome-${PASS}-${FAIL}-test.log" 2>&1 ); }
expect() {
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; cat "$TMP/pane-chrome-${PASS}-${FAIL}-test.log"; FAIL=$((FAIL+1)); fi
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
expect green "窗格外观像素基线" run_pixels
expect green "缩放基线" run_zoom
expect green "只有一个加号 / 顶栏显隐基线" run_strip
expect green "对照（标签页结构）基线" run_other

echo "== P1：间距偏好读了但不留白 =="
python3 - "$CHROME" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            .padding(CGFloat(gap) / 2)"
new = "            .padding(0) /* GT_MUTATION_MARK：间距不落实 */"
assert old in s, "P1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P1 设了间距两格仍贴着" run_pixels
expect green "P1 指向性（标签页结构不受影响）" run_other
rollback

echo "== P2：窗格标题栏开关没接上（关了照画） =="
python3 - "$ROOTVIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "                if opts.showsTitleBar {"
new = "                if true { /* GT_MUTATION_MARK：无视偏好 */"
assert old in s, "P2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P2 关掉标题栏还是画出来了" run_pixels
rollback

# P3/P4 顶部空带与重复导航已迁到 mutation-check-tab-chrome.py navigation 组。

echo "== P5：缩放时把被缩放那格的位置塌掉（兄弟跟着重排） =="
python3 - "$WORKSPACE" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "                host.frame = frame"
new = "                host.frame = tab.zoomedPaneID != nil && tab.zoomedPaneID != id ? CGRect(x: 0, y: 0, width: 1, height: 1) : frame /* GT_MUTATION_MARK */"
assert s.count(old) == 1, "P5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P5 缩放后兄弟窗格被重排" run_zoom
expect green "P5 指向性（标签页结构不受影响）" run_other
rollback

echo "== P7：缩放还原重置布局（用户比例被丢） =="
# 保持旧行为断言：缩放期间兄弟几何正确，但还原后比例不能被默认布局替换。
python3 - "$TAB" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        if zoomedPaneID == pane.id {\n            zoomedPaneID = nil"
new = "        if zoomedPaneID == pane.id {\n            zoomedPaneID = nil\n            restoreDefaultWorkspace() /* GT_MUTATION_MARK：还原时重置比例 */"
assert s.count(old) == 1, "P7 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P7 缩放往返后用户拖出来的比例丢了" run_zoom_ratio
expect green "P7 指向性（缩放期间兄弟几何仍纹丝不动）" run_zoom_sibling
rollback

echo "== P6：关掉被缩放那格后不清缩放状态 =="
python3 - "$TAB" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        if zoomedPaneID == pane.id || workspacePanes.count < 2 { zoomedPaneID = nil }"
new = "        /* GT_MUTATION_MARK：不清缩放状态 */"
assert old in s, "P6 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P6 缩放状态指向一个已经没了的窗格" run_zoom
rollback

echo "== P8：分屏子项不放弃顶部安全区（侧栏展开时顶出一条空白） =="
python3 - "$WORKSPACE" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
# 窗格不再嵌套 NSSplitView；保护稳定 NSHostingView 自身的安全区设置。
old = "                if #available(macOS 13.3, *) { host.safeAreaRegions = [] }\n"
new = "                _ = host /* GT_MUTATION_MARK：窗格宿主仍报安全区 */\n"
assert s.count(old) == 1, "P8 窗格宿主锚点异常：%d" % s.count(old)
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P8 侧栏展开 + 分屏时窗格顶上多出一条空白" run_top_inset
expect green "P8 指向性（两格之间的间距没动）" run_pane_gap
rollback

echo "== P9：工作区重复避让标题栏（顶上多一条留白） =="
python3 - "$WORKSPACE" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        let rect = bounds.insetBy(dx: inset, dy: inset)"
new = old + ".offsetBy(dx: 0, dy: PaneTitleBar.height) /* GT_MUTATION_MARK：重复避让标题栏 */"
assert s.count(old) == 1, "P9 工作区几何锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P9 工作区顶部重复留白" run_top_inset
expect green "P9 指向性（两格之间的间距没动）" run_pane_gap
rollback

echo "== P10：聚焦那格的标题栏不点强调色 =="
python3 - "$APPEAR" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        isFocused && isSplit ? 0.22 : 0"
new = "        _ = (isFocused, isSplit); return 0 /* GT_MUTATION_MARK：不点色 */"
assert old in s, "P10 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P10 分屏时聚焦那格的标题栏没点上强调色" run_focus_tint
expect green "P10 指向性（单格仍不点色）" run_lone_tint
rollback

echo "== P11：点色时不看分不分屏（单格也染色） =="
python3 - "$APPEAR" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        isFocused && isSplit ? 0.22 : 0"
new = "        isFocused ? 0.22 : 0 /* GT_MUTATION_MARK：无视分屏 */"
assert old in s, "P11 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P11 只有一格时标题栏也染上强调色" run_lone_tint
expect green "P11 指向性（分屏时的点色仍对）" run_focus_tint
rollback

echo "== 回滚复绿 =="
expect green "回滚后像素断言复绿" run_pixels
expect green "回滚后缩放断言复绿" run_zoom
expect green "回滚后加号/顶栏断言复绿" run_strip
expect green "回滚后标签页结构断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-pane-chrome: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
