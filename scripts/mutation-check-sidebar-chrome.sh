#!/usr/bin/env bash
# 变异验证：证明「侧栏切换按钮位置 / 当前标签页高亮」那批像素断言有牙
# （SidebarChromeTests）。
#
# 这两条都是「不报错、只在屏幕上不对」的改动（用户实测报回来的）：
#   S1 顶带不给红绿灯留位 → 按钮压在红绿灯上（用户截图里那团蓝色高亮）；
#   S2 侧栏顶带改回「按钮右对齐」的旧规则——**反向**变异：它必须把
#      「按钮组贴红绿灯左对齐、开合两态同一个 x」那条打红，否则旧布局
#      能悄悄长回来；
#   S3 当前标签页那行不高亮（看不出窗口在哪一页）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-sidebar-chrome

TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
STRIP="$ROOT/apple/GhostermApp/Sources/GhostermApp/TabStrip.swift"
SIDEBAR="$ROOT/apple/GhostermApp/Sources/GhostermApp/Sidebar.swift"
BAND="$ROOT/apple/GhostermApp/Sources/GhostermApp/BandControls.swift"
CHROME="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/WindowChrome.swift"
HASSIST="$ROOT/apple/GhostermApp/Sources/GhostermApp/HorizontalScrollAssist.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
declare -a FILES=("$STRIP" "$SIDEBAR" "$CHROME" "$HASSIST" "$BAND")
declare -a BACKUPS=() SHAS=()

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-sidebar-chrome: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-sidebar-chrome: 缺 GhosttyKit"; exit 0
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
  b="$(mktemp "$TMP/gt-mutchrome-XXXXXX")"; BACKUPS[$i]="$b.swift"
  cp "${FILES[$i]}" "${BACKUPS[$i]}"
done

run_chrome() { ( cd "$PKG" && gt_timeout 300 swift test --filter "SidebarChromeTests|WindowChromeTests" >/dev/null 2>&1 ); }
run_other()  { ( cd "$PKG" && gt_timeout 300 swift test --filter TabManagerTests >/dev/null 2>&1 ); }
run_strip()  { ( cd "$PKG" && gt_timeout 300 swift test --filter TabStripScrollTests >/dev/null 2>&1 ); }
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
expect green "侧栏/顶栏像素基线" run_chrome
expect green "对照（标签页结构）基线" run_other
expect green "标签条滚动（普通鼠标滚轮）基线" run_strip

echo "== S1：顶带不给红绿灯留位（按钮压上去） =="
python3 - "$BAND" <<'PYS1'
import sys
p = sys.argv[1]; s = open(p).read()
old = '''            Color.clear
                .frame(width: WindowChrome.trafficLightsWidth, height: 1)
                .titleBarDoubleClick()   // 双击顶栏空白处 = 系统的标题栏双击'''
new = '''            EmptyView() /* GT_MUTATION_MARK：不给红绿灯留位 */'''
assert old in s, "S1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYS1
expect red "S1 按钮压在红绿灯上" run_chrome
expect green "S1 指向性（标签页结构不受影响）" run_other
rollback

echo "== S2（反向）：侧栏顶带改回「按钮右对齐」的旧规则 =="
python3 - "$SIDEBAR" <<'PYS2'
import sys
p = sys.argv[1]; s = open(p).read()
old = '''            BandControls(model: model, showsSidebar: $showsSidebar,
                         onShowSettings: onShowSettings)
            Spacer(minLength: 0)'''
new = '''            Spacer(minLength: 0)
            BandControls(model: model, showsSidebar: $showsSidebar,
                         onShowSettings: onShowSettings)
            /* GT_MUTATION_MARK：旧规则，按钮贴侧栏右边 */'''
assert old in s, "S2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYS2
expect red "S2 按钮又跑回侧栏右端（开合两态位置不一致）" run_chrome
rollback

echo "== S3：当前标签页那行不高亮 =="
python3 - "$SIDEBAR" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ".fill(isActive ? Color.primary.opacity(0.12) : Color.clear)"
new = ".fill(Color.clear) /* GT_MUTATION_MARK：不高亮 */"
assert old in s, "S3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "S3 看不出当前标签页是哪行" run_chrome
expect green "S3 指向性（标签页结构不受影响）" run_other
rollback

echo "== S4：标题不清（分屏时 AppKit 会把它画出来） =="
python3 - "$CHROME" <<'PYS4'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        window.title = ""'
new = '        window.title = "Ghosterm" /* GT_MUTATION_MARK */'
assert old in s, "S4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYS4
expect red "S4 标题没清（分屏会冒出 Ghosterm）" run_chrome
rollback

echo "== S5：标签条不接滚轮（普通鼠标滚不动，右边标签够不着） =="
python3 - "$STRIP" <<'PYS5'
import sys
p = sys.argv[1]; s = open(p).read()
old = "                .background(WheelScrollsHorizontally())"
new = "                /* GT_MUTATION_MARK：不接滚轮 */"
assert old in s, "S5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYS5
expect red "S5 普通鼠标滚不动标签条" run_strip
expect green "S5 指向性（标签页结构不受影响）" run_other
rollback

echo "== S6：滚轮坐标不翻 y（真机点跑到条外，滚不动） =="
python3 - "$HASSIST" <<'PYS6'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            return NSPoint(x: p.x, y: primaryHeight - p.y)"
new = "            return NSPoint(x: p.x, y: p.y) /* GT_MUTATION_MARK：不翻 y */"
assert old in s, "S6 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYS6
expect red "S6 坐标不翻 y（Quartz/AppKit 混用）" run_strip
expect green "S6 指向性（标签页结构不受影响）" run_other
rollback

echo "== 回滚复绿 =="
expect green "回滚后像素断言复绿" run_chrome
expect green "回滚后标签页结构断言仍绿" run_other
expect green "回滚后标签条滚动断言复绿" run_strip

restore_all
trap - INT TERM
echo
echo "mutation-check-sidebar-chrome: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
