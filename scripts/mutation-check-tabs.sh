#!/usr/bin/env bash
# 变异验证：证明**标签页 / 分屏 / 新建菜单 / 标签右键菜单**那批断言有牙
# （TabManagerTests + SplitLayoutTests + NewTabMenuTests + TabContextMenuTests，
# 共 20 项）。
#
#   T1 关闭标签页不再停会话（先摘引用再停 → 读循环踩已释放内存）；
#   T2 关闭不再走两步确认（有会话在跑被静默杀掉）；
#   T3 关掉一个窗格后布局不塌陷（留下含单子节点的空壳）；
#   T6 本地 shell 也去重（点「新建」没反应）；
#   T7 本地 shell 标签页不能分屏；
#   T8 同一台主机被去重（开不了第二个标签页）——护「每次点都开新标签页」；
#      T6/T8 都把「去重」**加回 connect** 来造目标失效态（工厂已不含去重）。
#   T5 标签页状态取第一个窗格（失败被别的 ready 盖住，铁律 #6）；
#   T15 窗格退出不折叠（留在原地当「已退出」死窗格——用户实测的那一幕）；
#   T16 关闭窗格后布局沿用旧比例，不按既有关闭规则重新等分；
#   T17 标签右键菜单的分屏作用到「当前标签页」而非被右键的那条（右键 A 动了 B）；
#   T18 标签右键菜单的文件面板开关同样作用错标签页。
#   T19 本地 shell 无可继承目录时不落家目录（回到继承进程 cwd = /）——用户实测
#      「创建的本地终端当前目录是 /」的原始形态。
#   T20 标签右键菜单不再标快捷键（菜单右侧什么都不画，用户只能靠记）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 写脚本注意：`$var` 后面紧跟全角字符时 bash 会把多字节字节吃进变量名，
# 故本脚本一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
TABS="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TabManager.swift"
TAB="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/TerminalTab.swift"
APPMODEL="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift"
ROOTVIEW="$ROOT/apple/GhostermApp/Sources/GhostermApp/RootView.swift"
WORKLAY="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/WorkspaceLayout.swift"
CTXMENU="$ROOT/apple/GhostermApp/Sources/GhostermApp/TabContextMenu.swift"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
declare -a BACKUPS=() FILES=() SHAS=()

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-tabs: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-tabs: 缺 GhosttyKit"; exit 0
fi

FILES=("$TABS" "$TAB" "$APPMODEL" "$ROOTVIEW" "$CTXMENU" "$WORKLAY")

restore_all() {
# 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if ! grep -q "$MARK" "${FILES[@]}" 2>/dev/null; then return 0; fi
  local i
  for i in "${!FILES[@]}"; do
    [ -n "${BACKUPS[$i]:-}" ] && [ -f "${BACKUPS[$i]}" ] \
      && cp "${BACKUPS[$i]}" "${FILES[$i]}" && rm -f "${BACKUPS[$i]}"
  done
}
# EXIT 也复原：脚本被 kill -9 时 INT/TERM 的 trap 不会跑，变异会留在
# 源码里（实测踩到：下一次起跑被「残留标记」拦住，而且测试会红一片）。
# EXIT 覆盖不了 SIGKILL，但覆盖得了正常退出与被忽略的信号路径。
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "${FILES[@]}"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done
for i in "${!FILES[@]}"; do
  SHAS[$i]="$(shasum -a 256 "${FILES[$i]}" | awk '{print $1}')"
  b="$(mktemp "$TMP/gt-muttab-XXXXXX")"; BACKUPS[$i]="$b.swift"
  cp "${FILES[$i]}" "${BACKUPS[$i]}"
done

run_tabs() {  # 标签页结构 / 分屏 / 退出折叠 / 新建菜单
  ( cd "$PKG" && timeout 300 swift test --filter \
      "TabManagerTests|SplitLayoutTests|NewTabMenuTests|SplitExitTests|SplitPaneGeometryTests|TabContextMenuTests|WorkspaceTests" >/dev/null 2>&1 )
}
run_other() {  # 指向性对照：设置生效那批（与标签页结构无关）
  ( cd "$PKG" && timeout 300 swift test --filter TerminalConfigTests >/dev/null 2>&1 )
}

expect() { # ${1}=期望 red|green ${2}=描述 ${3}=函数名
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
      echo "  ✗ 回滚后 ${FILES[$i]} 的 sha256 与起跑前不一致！" >&2
      FAIL=$((FAIL+1))
    }
  done
}

echo "== 基线 =="
expect green "标签页/分屏/新建菜单基线" run_tabs
expect green "对照（设置生效）基线" run_other

echo "== T1：关闭标签页不再先停会话 =="
# 这是「关掉标签页、会话还在后台跑」的泄漏：不报错，只是进程里多出
# 一条没人管的连接与读循环。
python3 - "$TABS" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    public func close(_ tab: TerminalTab) {
        willClose?(tab)   // 取消在途连接（见 willClose 说明），必须在 stop 之前
        tab.stop()
        remove(tab)
    }"""
new = """    public func close(_ tab: TerminalTab) {
        willClose?(tab)
        remove(tab) /* GT_MUTATION_MARK：不停会话 */
    }"""
assert old in s, "T1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "T1 关闭标签页后会话还在跑（泄漏）" run_tabs
expect green "T1 指向性（设置生效不受影响）" run_other
rollback

echo "== T2：关闭一律弹框（不看有没有东西会被打断） =="
# 变异 = 把「有东西才问」的收口退回「无条件弹框」：空闲/已退出/失败的标签页
# 也被拦——正是用户抱怨的「关个空闲 shell 还要确认」。
python3 - "$TABS" <<'PY2'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        if tab.closeNeedsConfirmation {
            closeRequest = tab
        } else {
            close(tab)
        }"""
new = """        closeRequest = tab /* GT_MUTATION_MARK：不看判据，一律弹框 */
        _ = tab"""
assert old in s, "T2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY2
expect red "T2 没什么可丢的标签页也被拦（应直接关）" run_tabs
expect green "T2 指向性（设置生效不受影响）" run_other
rollback

echo "== T3：关窗格后布局不塌陷 =="
python3 - "$WORKLAY" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            if kept.count == 1 { return kept[0] }"
new = "            if false { /* GT_MUTATION_MARK：留空壳 */ }"
assert old in s, "T3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "T3 只剩一个窗格时留下单子节点的空壳" run_tabs
rollback

echo "== T5：标签页状态取第一个窗格（失败被盖住） =="
python3 - "$TAB" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        return Self.worst(panes.map(\\.status) + files)"
new = "        return panes.first?.status ?? .ready /* GT_MUTATION_MARK */"
assert old in s, "T5 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "T5 一个窗格失败被另一个 ready 盖住" run_tabs
rollback

# ---- T6：本地 shell 也去重（点新建没反应） ----
# 「点新建标签页 → 本地 shell」看起来没反应，是因为 kind 相同被当成
# 「已开着」切了回去。这条守的就是它。
echo "== T6：本地 shell 也去重（点新建没反应） =="
# 现在生产路径每次点都新建标签页（见 connect）；这条守护的是「不要把它
# 又改回去重」。故变异就是**把去重加回 connect**（本地 shell 一路）。
python3 - "$APPMODEL" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        // 标签页与窗格先建（状态 .connecting：屏幕显示「连接中…」），"
new = ("        if kind.isLocalShell,\n"
       "           tabs.tabs.contains(where: { $0.kind.isLocalShell }) {\n"
       "            return /* GT_MUTATION_MARK：本地 shell 被误去重 */\n"
       "        }\n"
       "        // 标签页与窗格先建（状态 .connecting：屏幕显示「连接中…」），")
assert old in s, "T6 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "T6 本地 shell 被误判为已开着" run_tabs
rollback

# ---- T7：本地 shell 标签页不能分屏 ----
echo "== T7：本地 shell 标签页不能分屏 =="
python3 - "$APPMODEL" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
# 锚点必须**唯一**：`if tab.kind.isLocalShell {` 在 `startFilesPanel` 和
# `splitActiveTab` 各有一处，只写这一行会被替换成文件里**第一处**（文件面板那条
# ——改它不会让分屏断言变红，T7 曾因此假绿）。带上上一行注释定位到分屏那处。
old = """        if tab.kind.isLocalShell {
            nativeSessions.local(pane: pane,"""
new = """        if false { /* GT_MUTATION_MARK：本地 shell 不分屏 */
            nativeSessions.local(pane: pane,"""
assert old in s, "T7 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "T7 本地 shell 分屏没反应" run_tabs
rollback

# ---- T8：同一台主机被去重（开不了第二个标签页） ----
# 用户实测：「同一个 ssh 无法创建多个 tab，但又可以切割」——`.ssh(...)`
# 少了 instance，两个 kind 相等 → 第二次点被去重成「切回已开着的那个」。
echo "== T8：同一台主机被误判为已开着 =="
# 用户实测：「同一个 ssh 无法创建多个 tab，但又可以切割」——去重把第二次
# 点击吞成了「切回已开着的那条」。故变异是**把「同一 pattern 就去重」
# 加回 connect**（SSH 一路）。
python3 - "$APPMODEL" <<'PYT8'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        // 标签页与窗格先建（状态 .connecting：屏幕显示「连接中…」），"
new = ("        if case .ssh(let dup) = kind,\n"
       "           tabs.tabs.contains(where: { $0.kind.remoteTarget == dup }) {\n"
       "            return /* GT_MUTATION_MARK：同一台主机被误去重 */\n"
       "        }\n"
       "        // 标签页与窗格先建（状态 .connecting：屏幕显示「连接中…」），")
assert old in s, "T8 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYT8
expect red "T8 同一台主机开不了第二个标签页" run_tabs
rollback

echo "== T9：把 idle 当成要确认（空闲也拦） =="
python3 - "$TAB" <<'PY3'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        case .ready: return childState != .idle           // 有作业 / 查不出 → 问"
new = "        case .ready: return true /* GT_MUTATION_MARK：空闲也拦 */"
assert old in s, "T9 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY3
expect red "T9 空闲提示符也被拦（应直接关）" run_tabs
expect green "T9 指向性（设置生效不受影响）" run_other
rollback

echo "== T10：把 unknown 折叠成 idle（查不出当空闲） =="
python3 - "$TAB" <<'PY4'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        case .ready: return childState != .idle           // 有作业 / 查不出 → 问"
new = "        case .ready: return childState == .job /* GT_MUTATION_MARK：unknown 折叠成空闲 */"
assert old in s, "T10 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY4
expect red "T10 查不出的（SSH）被当空闲、不再问" run_tabs
expect green "T10 指向性（设置生效不受影响）" run_other
rollback

echo "== T11：退出不看有没有作业（直接放行） =="
python3 - "$APPMODEL" <<'PY5'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    public func shouldTerminate() -> Bool {\n        !TransferManager.shared.hasActive"
new = "    public func shouldTerminate() -> Bool {\n        if true { return true } /* GT_MUTATION_MARK：退出不拦 */\n        return !TransferManager.shared.hasActive"
assert old in s, "T11 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY5
expect red "T11 有前台作业也直接退出（不拦）" run_tabs
expect green "T11 指向性（设置生效不受影响）" run_other
rollback

echo "== T12：SFTP 又把去重加回来（与 SSH 不一致） =="
python3 - "$APPMODEL" <<'PY6'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        let tab = TerminalTab(kind: kind)\n"
new = ("        if let existing = tabs.tabs.first(where: { $0.kind == kind }) {\n"
       "            tabs.activate(existing); return /* GT_MUTATION_MARK */\n"
       "        }\n"
       + old)
assert old in s, "T12 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY6
expect red "T12 SFTP 又被去重（开不了第二个）" run_tabs
expect green "T12 指向性（设置生效不受影响）" run_other
rollback

echo "== T13：⌘T 写死本地（不继承当前类型） =="
python3 - "$APPMODEL" <<'PY7'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        switch kind {
        case .localShell:
            // 本地：顺带继承当前目录（同一个 shell 常在同一处继续开新的）。
            connect(to: .localShell, cwd: tabs.activeTab?.activePane?.currentDirectory)
        case .ssh(let t): connect(to: .host(t.pattern))
        case .sftp(let t): openSFTP(for: t.pattern)
        case .localFiles: openLocalFiles(newTab: true)
        }"""
new = """        connect(to: .localShell) /* GT_MUTATION_MARK：写死本地，不继承 */
        _ = kind"""
assert old in s, "T13 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY7
expect red "T13 ⌘T 不继承（ssh 标签页上却开本地）" run_tabs
expect green "T13 指向性（设置生效不受影响）" run_other
rollback

echo "== T14：⌘T 不继承本地目录（新 shell 开在别处） =="
python3 - "$APPMODEL" <<'PY8'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            connect(to: .localShell, cwd: tabs.activeTab?.activePane?.currentDirectory)"
new = "            connect(to: .localShell) /* GT_MUTATION_MARK：不继承目录 */"
assert old in s, "T14 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY8
expect red "T14 新本地 shell 没继承目录" run_tabs
expect green "T14 指向性（设置生效不受影响）" run_other
rollback

echo "== T15：窗格退出不再折叠（留下「已退出」死窗格） =="
# 用户实测：分屏里一个 shell `exit` 后，原实现把窗格留在「已退出」占着半屏。
# 变异是**让 handlePaneExit 什么都不做**（回调还在，但结构不动）。
python3 - "$APPMODEL" <<'PYT15'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    func handlePaneExit(_ pane: TerminalPane) {"""
new = """    func handlePaneExit(_ pane: TerminalPane) {
        return /* GT_MUTATION_MARK：退出不折叠 */
"""
assert old in s, "T15 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYT15
expect red "T15 窗格退出后不收掉（留死窗格）" run_tabs
expect green "T15 指向性（设置生效不受影响）" run_other
rollback

echo "== T16：关闭窗格后沿用旧分隔比例 =="
# 原生稳定宿主不再重建子视图；关闭后的等分语义由唯一布局树明确实现。
python3 - "$WORKLAY" <<'PYT16'
import sys
p = sys.argv[1]; s = open(p).read()
old = "weights: equalize && paneIDs.contains(target) ? kept.map { _ in 1 } : keptWeights,"
new = "weights: keptWeights, /* GT_MUTATION_MARK：关闭后沿用旧比例 */"
assert s.count(old) == 1, f"T16 锚点数量不对（{s.count(old)}）"
open(p, 'w').write(s.replace(old, new, 1))
PYT16
expect red "T16 关窗格后剩下两个不等分（旧比例）" run_tabs
expect green "T16 指向性（设置生效不受影响）" run_other
rollback

echo "== T17：右键分屏作用到「当前标签页」而非被右键的那条 =="
# 右键的那条未必是当前标签页：丢掉 activate 就变成「右键 A 却分屏了 B」，
# 不报错、只在屏幕上不对。
python3 - "$APPMODEL" <<'PYT17'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    public func split(_ tab: TerminalTab, axis: PaneLayout.Axis) {
        tabs.activate(tab)
        splitActiveTab(axis: axis)
    }"""
new = """    public func split(_ tab: TerminalTab, axis: PaneLayout.Axis) {
        splitActiveTab(axis: axis) /* GT_MUTATION_MARK：作用到当前标签页 */
    }"""
assert old in s, "T17 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYT17
expect red "T17 右键分屏落在别的标签页上" run_tabs
expect green "T17 指向性（设置生效不受影响）" run_other
rollback

echo "== T18：右键开文件面板作用到「当前标签页」 =="
python3 - "$APPMODEL" <<'PYT18'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    public func toggleFilesPanel(for tab: TerminalTab) {
        guard tab.canHaveFilesPanel else { return }"""
new = """    public func toggleFilesPanel(for tab: TerminalTab) {
        let tab = tabs.activeTab ?? tab /* GT_MUTATION_MARK：作用到当前标签页 */
        guard tab.canHaveFilesPanel else { return }"""
assert old in s, "T18 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYT18
expect red "T18 右键开面板落在别的标签页上" run_tabs
expect green "T18 指向性（设置生效不受影响）" run_other
rollback

echo "== T19：本地 shell 无可继承目录时不落家目录（回到继承进程 cwd = /） =="
# 用户实测「创建的本地终端当前目录是 /」的原始形态：`connect` 的 .localShell
# 分支不显式给 cwd，子进程继承 GUI 进程的 cwd（`/`）。变异就是**把家目录
# 默认值拿掉**，回到 `cwd`（nil）——`testLocalShellDefaultsToHomeWhenNoCwd`
# 期望家目录，实际拿到进程 cwd（≠ 家目录），断言变红。
python3 - "$APPMODEL" <<'PYT19'
import sys
p = sys.argv[1]; s = open(p).read()
old = "nativeSessions.local(pane: pane, app: terminalApp, shell: preferences.resolvedShell, cwd: cwd ?? NSHomeDirectory())"
new = "nativeSessions.local(pane: pane, app: terminalApp, shell: preferences.resolvedShell, cwd: cwd ?? FileManager.default.currentDirectoryPath) /* GT_MUTATION_MARK */"
assert old in s, "T19 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYT19
expect red "T19 无可继承目录的本地 shell 落到进程 cwd（/）" run_tabs
expect green "T19 指向性（设置生效不受影响）" run_other
rollback

echo "== T20：标签右键菜单不再标快捷键 =="
# 「菜单上看不见快捷键」不报错、也不影响功能，只是用户得自己记住 ⌘W/⌘D 这些；
# 变异就是把菜单项上的 `.keyboardShortcut` 拿掉（菜单右侧什么都不画）。
python3 - "$CTXMENU" <<'PYT20'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        Button("关闭标签页") { tabs.requestClose(tab) }
            .keyboardShortcut("w", modifiers: .command)"""
new = """        Button("关闭标签页") { tabs.requestClose(tab) }
            // GT_MUTATION_MARK：不标快捷键"""
assert old in s, "T20 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYT20
expect red "T20 菜单上不再显示 ⌘W" run_tabs
expect green "T20 指向性（设置生效不受影响）" run_other
rollback

echo "== 回滚复绿 =="
expect green "回滚后标签页断言复绿" run_tabs
expect green "回滚后设置生效断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-tabs: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]