#!/usr/bin/env bash
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-native-context
MENU="$ROOT/apple/GhostermApp/Sources/GhostermApp/NativeTabContextMenu.swift"
WINDOW="$ROOT/apple/GhostermApp/Sources/GhostermApp/NativeTitlebarWindow.swift"
PKG="$ROOT/apple/GhostermApp"
mkdir -p "$ROOT/tmp"
if grep -q GT_MUTATION_MARK "$MENU" "$WINDOW"; then echo '残留变异标记' >&2; exit 2; fi
if ! grep -Fq 'let id = owner.tabID(for: view)' "$MENU" || ! grep -Fq 'guard let tab = findTab(bar), let menu = tab.menu else { return nil }' "$WINDOW"; then
  echo '原生右键菜单变异锚点不存在' >&2; exit 2
fi
if ! grep -Fq 'add("恢复默认布局") { tab.restoreDefaultWorkspace() }' "$MENU" || ! grep -Fq 'key: "e", modifiers: [.command, .shift]' "$MENU"; then
  echo '菜单项目或快捷键变异锚点不存在' >&2; exit 2
fi
if ! grep -Fq 'view.bounds.contains(view.convert(event.locationInWindow, from: nil))' "$WINDOW"; then
  echo '右键命中范围变异锚点不存在' >&2; exit 2
fi
if ! grep -Fq 'event.window === bar.window' "$WINDOW"; then echo '右键窗口归属变异锚点不存在' >&2; exit 2; fi
WORK="$(mktemp -d "$ROOT/tmp/native-context-mutation.XXXXXX")"
cp "$MENU" "$WORK/menu.swift"
cp "$WINDOW" "$WORK/window.swift"
MENU_SHA="$(shasum -a 256 "$MENU" | awk '{print $1}')"
WINDOW_SHA="$(shasum -a 256 "$WINDOW" | awk '{print $1}')"
restore() {
  cp "$WORK/menu.swift" "$MENU"
  cp "$WORK/window.swift" "$WINDOW"
  local m w
  m="$(shasum -a 256 "$MENU" | awk '{print $1}')"
  w="$(shasum -a 256 "$WINDOW" | awk '{print $1}')"
  if [ "$m" != "$MENU_SHA" ] || [ "$w" != "$WINDOW_SHA" ]; then echo '回滚 SHA256 不一致' >&2; return 1; fi
}
trap 'rc=$?; restore || rc=1; rm -rf "$WORK"; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
run_test() { (cd "$PKG" && TMPDIR="$ROOT/tmp" gt_timeout 300 swift test --filter "$1") >"$WORK/test.log" 2>&1; }
expect_green() { if ! run_test "$1"; then cat "$WORK/test.log"; echo "期望通过：${1}" >&2; exit 1; fi; }
expect_red() {
  if run_test "$1"; then cat "$WORK/test.log"; echo "期望断言失败：${1}" >&2; exit 1; fi
  if ! grep -Fq "$2" "$WORK/test.log"; then cat "$WORK/test.log"; echo "失败不在目标断言：${2}" >&2; exit 1; fi
}
TARGET='NativeTabContextMenuTests.testNativeMenuTargetsRightClickedTab'
CONTROL='NativeTabWindowsTests.testNativeAndSidebarOrderStayInSync'
expect_green "$TARGET"
expect_green "$CONTROL"
python3 - "$MENU" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '              let id = owner.tabID(for: view),'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '              let id = model.tabs.activeTabID, // GT_MUTATION_MARK', 1))
PY
expect_red "$TARGET" '文件面板应作用于被右键的标签'
expect_green "$CONTROL"
restore
python3 - "$WINDOW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '''        guard let tab = findTab(bar), let menu = tab.menu else { return nil }
        return (menu, tab, tab.convert(event.locationInWindow, from: nil))'''
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '        return nil // GT_MUTATION_MARK', 1))
PY
expect_red "$TARGET" '原生标签右键必须提供菜单'
expect_green "$CONTROL"
restore
python3 - "$WINDOW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = 'view.bounds.contains(view.convert(event.locationInWindow, from: nil))'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, 'true /* GT_MUTATION_MARK */', 1))
PY
expect_red "$TARGET" '标签栏外的右键不得被截获'
expect_green "$CONTROL"
restore
python3 - "$WINDOW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = 'event.window === bar.window'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, 'true /* GT_MUTATION_MARK */', 1))
PY
expect_red "$TARGET" '其他窗口的右键不得被截获'
expect_green "$CONTROL"
restore
python3 - "$MENU" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        add("恢复默认布局") { tab.restoreDefaultWorkspace() }'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '        // GT_MUTATION_MARK: 缺少菜单项', 1))
PY
expect_red "$TARGET" '原生菜单必须与侧边栏菜单一致'
expect_green "$CONTROL"
restore
python3 - "$MENU" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = 'key: "e", modifiers: [.command, .shift]'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, 'key: "x", modifiers: [.command, .shift] /* GT_MUTATION_MARK */', 1))
PY
expect_red "$TARGET" '原生菜单快捷键必须与侧边栏一致'
expect_green "$CONTROL"
restore
expect_green "$TARGET"
echo 'native-context mutation PASS: 入口、目标、菜单项目及快捷键红，对照绿、回滚绿'
