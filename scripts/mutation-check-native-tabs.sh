#!/usr/bin/env bash
# Native tab selection and title tests must fail for their respective regressions.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-native-tabs
SOURCE="$ROOT/apple/GhostermApp/Sources/GhostermApp/NativeTabWindows.swift"
PKG="$ROOT/apple/GhostermApp"
mkdir -p "$ROOT/tmp"
if grep -q GT_MUTATION_MARK "$SOURCE"; then
  echo "残留变异标记，拒绝起跑" >&2; exit 2
fi
for needle in 'self.activateWindow(window)' 'window.title = title' 'model.tabs.requestClose(tab)' 'self.acceptWindowOrder(group.windows)' 'if group.windows[index] !== window { group.insertWindow(window, at: index) }'; do
  if ! grep -Fq "$needle" "$SOURCE"; then
    echo "缺少变异锚点：${needle}" >&2; exit 2
  fi
done
WORK="$(mktemp -d "$ROOT/tmp/native-tabs-mutation.XXXXXX")"
BACKUP="$WORK/source.swift"
LOG="$WORK/test.log"
cp "$SOURCE" "$BACKUP"
SHA="$(shasum -a 256 "$SOURCE" | awk '{print $1}')"
restore() {
  cp "$BACKUP" "$SOURCE"
  local actual
  actual="$(shasum -a 256 "$SOURCE" | awk '{print $1}')"
  if [ "$actual" != "$SHA" ]; then echo '回滚 SHA256 不一致' >&2; return 1; fi
}
trap 'rc=$?; restore || rc=1; rm -rf "$WORK"; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
run_test() {
  (cd "$PKG" && TMPDIR="$ROOT/tmp" gt_timeout 300 swift test --filter "$1") >"$LOG" 2>&1
}
expect_green() {
  if ! run_test "$1"; then cat "$LOG"; echo "期望通过：${1}" >&2; exit 1; fi
}
expect_red() {
  if run_test "$1"; then cat "$LOG"; echo "期望断言失败：${1}" >&2; exit 1; fi
  if ! grep -Fq "$2" "$LOG"; then cat "$LOG"; echo "失败不在目标断言：${2}" >&2; exit 1; fi
}
TARGET='NativeTabWindowsTests.testNativeWindowsFollowModelAndCloseTheRequestedTab'
CONTROL='NativeTabWindowsTests.testSwitchingNativeTabsKeepsLiveTerminalHostsAndProcesses'
CLOSE='NativeTabWindowsTests.testNativeCloseRequestsConfirmationForTheTarget'
ORDER='NativeTabWindowsTests.testNativeAndSidebarOrderStayInSync'
expect_green "$TARGET"
expect_green "$CONTROL"
expect_green "$CLOSE"
expect_green "$ORDER"
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '                self.activateWindow(window)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '                _ = window // GT_MUTATION_MARK', 1))
PY
expect_red "$TARGET" 'XCTAssertEqual failed'
expect_green "$CONTROL"
restore
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '        window.title = title\n    }\n}'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '        _ = title // GT_MUTATION_MARK\n    }\n}', 1))
PY
expect_red "$TARGET" '调整尺寸不得清空原生标签标题'
expect_green "$CONTROL"
restore
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '        model.tabs.requestClose(tab)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '        model.tabs.close(tab) // GT_MUTATION_MARK', 1))
PY
expect_red "$CLOSE" '原生关闭必须询问指定的标签'
expect_green "$CONTROL"
restore
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '                self.acceptWindowOrder(group.windows)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '                _ = group.windows // GT_MUTATION_MARK', 1))
PY
expect_red "$ORDER" '拖动原生标签必须更新侧边栏顺序'
expect_green "$CONTROL"
restore
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '                if group.windows[index] !== window { group.insertWindow(window, at: index) }'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '                _ = window // GT_MUTATION_MARK', 1))
PY
expect_red "$ORDER" 'XCTAssertEqual failed'
expect_green "$CONTROL"
restore
expect_green "$TARGET"
expect_green "$CLOSE"
expect_green "$ORDER"
echo 'native-tabs mutation PASS: 选择、标题、关闭确认与双向排序目标红、对照绿、回滚绿'
