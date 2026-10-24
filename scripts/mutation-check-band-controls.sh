#!/usr/bin/env bash
# 变异验证：证明「顶带按钮组的顺序 / ⚠ 报错出口 / 菜单栏是唯一退路」那批
# 断言有牙（TabStripVisibilityTests / AppMenuCoverageTests）。
#
# 背景：侧栏底部的 ⚙︎「设置」和标签条右端的 ⋯ 溢出菜单都删了（macOS 顶上
# 有全局菜单条，窗口里再挂一份是重复）。删完之后有三件事会**悄悄**坏掉，
# 编译照过、界面照画，所以逐条变异：
#   B1 开关与 `+` 顺序对调 → 两个按钮每次改动都可能互换，位置就不是肌肉记忆；
#   B2 ⚠ 的条件去掉 → 终端配置坏了一声不吭（铁律 #6：不许悄悄降级）；
#   B3 ⚠ 插到按钮组最左 → 它一出现就把开关和加号全推走；
#   B4 菜单栏里拿掉「设置…」→ 窗口里的 ⚙︎ 已经没了，设置彻底打不开；
#   B5 菜单栏里拿掉「传输」→ 同上。它**没有快捷键**，不在 ShortcutCatalog
#      里，所以旧的 `testShortcutCatalogMatchesTheRealMenu` 看不见它——
#      B5 的指向性断言就是用来证明这个缺口确实存在、确实由新 gate 堵上；
#   B6 「向下分屏」漏掉 target → 菜单项在、点了什么都不发生（真出过这个 bug）；
#   B7 「关于」点了不开窗 → 菜单项在、target 在、点了没反应，`AppMenuCoverageTests`
#      那两条（action / target 不为 nil）全看不见——`AboutWindowTests` 才认；
#   B8 菜单栏里拿掉「关于 Ghosterm」→ 关于窗口再没有入口（它不在设置里了）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
BAND="$ROOT/apple/GhostermApp/Sources/GhostermApp/BandControls.swift"
APPMENU="$ROOT/apple/GhostermApp/Sources/GhostermApp/AppMenu.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
declare -a FILES=("$BAND" "$APPMENU")
declare -a BACKUPS=() SHAS=()

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-band-controls: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-band-controls: 缺 GhosttyKit"; exit 0
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
  b="$(mktemp "$TMP/gt-mutband-XXXXXX")"; BACKUPS[$i]="$b.swift"
  cp "${FILES[$i]}" "${BACKUPS[$i]}"
done

run_band()    { ( cd "$PKG" && timeout 420 swift test --filter TabStripVisibilityTests >/dev/null 2>&1 ); }
run_menu()    { ( cd "$PKG" && timeout 420 swift test --filter AppMenuCoverageTests >/dev/null 2>&1 ); }
run_catalog() { ( cd "$PKG" && timeout 420 swift test --filter SettingsUITests >/dev/null 2>&1 ); }
run_pixels()  { ( cd "$PKG" && timeout 420 swift test --filter SidebarChromeTests >/dev/null 2>&1 ); }
run_about()   { ( cd "$PKG" && timeout 420 swift test --filter AboutWindowTests >/dev/null 2>&1 ); }
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
expect green "顶带几何基线" run_band
expect green "菜单栏覆盖基线" run_menu
expect green "快捷键对拍基线" run_catalog
expect green "侧栏/顶栏像素基线" run_pixels
expect green "关于窗口基线" run_about

echo "== B1：开关与加号顺序对调 =="
python3 - "$BAND" <<'PYB1'
import sys
p = sys.argv[1]; s = open(p).read()
old = r"""            GlassIconButton(systemName: "sidebar.leading",
                            help: showsSidebar ? "隐藏侧边栏" : "显示侧边栏") {
                // 两个方向都由这一个按钮管。踩过的坑：先前侧栏开合各画一个
                // 按钮，收起时那个抄来的还写着 `showsSidebar = false`，按下去
                // 什么都不发生，侧栏再也打不开（用户实测）。
                showsSidebar.toggle()
            }

            // 「新建标签页」全窗口只有这一个入口（两处都画就会出现两个加号）。
            NewTabMenu(model: model)"""
new = r"""            NewTabMenu(model: model) /* GT_MUTATION_MARK：加号抢到左边 */

            GlassIconButton(systemName: "sidebar.leading",
                            help: showsSidebar ? "隐藏侧边栏" : "显示侧边栏") {
                showsSidebar.toggle()
            }"""
assert old in s, "B1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB1
expect red "B1 加号跑到开关左边" run_band
# 指向性：像素断言只量「按钮组在不在红绿灯右边那一带」，看不出组内顺序
# ——这正是几何断言（TabStripVisibilityTests）必须单独存在的理由。
expect green "B1 指向性（像素断言看不出顺序，仍绿）" run_pixels
rollback

echo "== B2：配置坏了不吭声（删掉 ⚠ 的条件） =="
python3 - "$BAND" <<'PYB2'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if let err = model.terminalConfigError {"
new = "            if false, let err = model.terminalConfigError { /* GT_MUTATION_MARK：不报错 */"
assert old in s, "B2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB2
expect red "B2 终端配置无效时顶带什么都不显示" run_band
rollback

echo "== B3：⚠ 插到最左（一出现就把开关和加号推走） =="
python3 - "$BAND" <<'PYB3'
import sys
p = sys.argv[1]; s = open(p).read()
head = r"""            if let err = model.terminalConfigError {"""
assert head in s, "B3 锚点缺失"
# 把整段 ⚠（从 if 到它那一层的收尾花括号）搬到按钮组最前面。
i = s.index(head)
j = s.index('\n            }\n', i) + len('\n            }\n')
warn = s[i:j]
s = s[:i] + s[j:]
anchor = r"""            GlassIconButton(systemName: "sidebar.leading","""
k = s.index(anchor)
s = s[:k] + warn.rstrip('\n') + ' /* GT_MUTATION_MARK：⚠ 抢到最左 */\n\n' + s[k:]
open(p, 'w').write(s)
PYB3
expect red "B3 ⚠ 一出现就把开关和加号推走" run_band
rollback

echo "== B4：菜单栏里拿掉「设置…」 =="
python3 - "$APPMENU" <<'PYB4'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        menu.addItem(settings)"
new = "        _ = settings /* GT_MUTATION_MARK：设置项不进菜单 */"
assert old in s, "B4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB4
expect red "B4 窗口里没了 ⚙︎、菜单栏也没了设置…" run_menu
rollback

echo "== B5：菜单栏里拿掉「传输」 =="
python3 - "$APPMENU" <<'PYB5'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        menu.addItem(transfers)"
new = "        _ = transfers /* GT_MUTATION_MARK：传输项不进菜单 */"
assert old in s, "B5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB5
expect red "B5 传输这项整个消失" run_menu
# 指向性：「传输」没有快捷键，不在 ShortcutCatalog 里，所以旧的快捷键对拍
# 看不见它——这条绿正好说明新 gate 堵的是一个真缺口。
expect green "B5 指向性（旧的快捷键对拍抓不到，仍绿）" run_catalog
rollback

echo "== B6：「向下分屏」漏掉 target（点了什么都不发生） =="
python3 - "$APPMENU" <<'PYB6'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        down.target = commands"
new = "        /* GT_MUTATION_MARK：漏掉 target */"
assert old in s, "B6 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB6
expect red "B6 菜单项在、但没人接" run_menu
expect green "B6 指向性（快捷键对拍只看键位，仍绿）" run_catalog
rollback

echo "== B7：「关于」点了不开窗 =="
python3 - "$APPMENU" <<'PYB7'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    @objc func showAbout(_ sender: Any?) { AboutWindow.open() }"
new = "    @objc func showAbout(_ sender: Any?) { /* GT_MUTATION_MARK：不开窗 */ }"
assert old in s, "B7 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB7
expect red "B7 关于菜单项点了没有窗口" run_about
# 指向性：菜单覆盖那两条只看 action / target 在不在，这种「点了没反应」的
# 坏法（也就是退回系统标准面板的那种坏法）它们一概看不见。
expect green "B7 指向性（菜单覆盖断言看不出点了有没有用，仍绿）" run_menu
rollback

echo "== B8：菜单栏里拿掉「关于 Ghosterm」 =="
python3 - "$APPMENU" <<'PYB8'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        menu.addItem(about)"
new = "        _ = about /* GT_MUTATION_MARK：关于项不进菜单 */"
assert old in s, "B8 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYB8
expect red "B8 关于这项整个消失（它已经不在设置里了）" run_menu
rollback

echo "== 回滚复绿 =="
expect green "回滚后顶带几何断言复绿" run_band
expect green "回滚后菜单栏覆盖断言复绿" run_menu
expect green "回滚后快捷键对拍仍绿" run_catalog
expect green "回滚后像素断言仍绿" run_pixels
expect green "回滚后关于窗口断言复绿" run_about

restore_all
trap - INT TERM
echo
echo "mutation-check-band-controls: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
