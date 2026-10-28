#!/usr/bin/env bash
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-sidebar-hover
SOURCE="$ROOT/apple/GhostermApp/Sources/GhostermApp/Sidebar.swift"
BUTTON="$ROOT/apple/GhostermApp/Sources/GhostermApp/TabStrip.swift"
PKG="$ROOT/apple/GhostermApp"
mkdir -p "$ROOT/tmp"
if grep -q GT_MUTATION_MARK "$SOURCE" "$BUTTON"; then echo '残留变异标记' >&2; exit 2; fi
if ! grep -Fq 'override func mouseEntered(with event: NSEvent) { changed?(true) }' "$SOURCE"; then
  echo '悬停变异锚点不存在' >&2; exit 2
fi
if ! grep -Fq 'NotificationCenter.default.post(name: .ghostermRequestCloseTab, object: tab.id)' "$BUTTON"; then
  echo '关闭目标变异锚点不存在' >&2; exit 2
fi
WORK="$(mktemp -d "$ROOT/tmp/sidebar-hover-mutation.XXXXXX")"
BACKUP="$WORK/source.swift"
LOG="$WORK/test.log"
cp "$SOURCE" "$BACKUP"
cp "$BUTTON" "$WORK/button.swift"
SHA="$(shasum -a 256 "$SOURCE" | awk '{print $1}')"
BUTTON_SHA="$(shasum -a 256 "$BUTTON" | awk '{print $1}')"
restore() {
  cp "$BACKUP" "$SOURCE"
  cp "$WORK/button.swift" "$BUTTON"
  local actual
  actual="$(shasum -a 256 "$SOURCE" | awk '{print $1}')"
  if [ "$actual" != "$SHA" ]; then echo '回滚 SHA256 不一致' >&2; return 1; fi
  actual="$(shasum -a 256 "$BUTTON" | awk '{print $1}')"
  if [ "$actual" != "$BUTTON_SHA" ]; then echo '按钮回滚 SHA256 不一致' >&2; return 1; fi
}
trap 'rc=$?; restore || rc=1; rm -rf "$WORK"; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
run_test() { (cd "$PKG" && TMPDIR="$ROOT/tmp" gt_timeout 300 swift test --filter "$1") >"$LOG" 2>&1; }
expect_green() {
  if ! run_test "$1"; then cat "$LOG"; echo "期望通过：${1}" >&2; exit 1; fi
}
TARGET='SidebarHoverTests.testHoverChangesRowRendering'
CONTROL='NativeTabWindowsTests.testSwitchingNativeTabsKeepsLiveTerminalHostsAndProcesses'
expect_green "$TARGET"
expect_green "$CONTROL"
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'override func mouseEntered(with event: NSEvent) { changed?(true) }'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, 'override func mouseEntered(with event: NSEvent) { changed?(false) } // GT_MUTATION_MARK', 1))
PY
if run_test "$TARGET"; then cat "$LOG"; echo '悬停变异未被检测' >&2; exit 1; fi
if ! grep -Fq '悬停时侧栏关闭按钮应画出来' "$LOG"; then
  cat "$LOG"; echo '失败不在悬停断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
python3 - "$BUTTON" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'NotificationCenter.default.post(name: .ghostermRequestCloseTab, object: tab.id)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, 'NotificationCenter.default.post(name: .ghostermRequestCloseTab, object: UUID()) // GT_MUTATION_MARK', 1))
PY
if run_test "$TARGET"; then cat "$LOG"; echo '关闭目标变异未被检测' >&2; exit 1; fi
if ! grep -Fq '关闭按钮必须作用于对应标签' "$LOG"; then
  cat "$LOG"; echo '失败不在关闭目标断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
expect_green "$TARGET"
echo 'sidebar-hover mutation PASS: 悬停与关闭目标两组目标红、对照绿、回滚绿'
