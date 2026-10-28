#!/usr/bin/env bash
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-native-focus
SOURCE="$ROOT/apple/GhostermApp/Sources/GhostermApp/RootView.swift"
PKG="$ROOT/apple/GhostermApp"
mkdir -p "$ROOT/tmp"
if grep -q GT_MUTATION_MARK "$SOURCE"; then echo '残留变异标记' >&2; exit 2; fi
if ! grep -Fq 'isActive: tab.id == tabs.activeTabID)' "$SOURCE"; then echo '标签焦点变异锚点不存在' >&2; exit 2; fi
WORK="$(mktemp -d "$ROOT/tmp/native-focus-mutation.XXXXXX")"
cp "$SOURCE" "$WORK/source.swift"
SHA="$(shasum -a 256 "$SOURCE" | awk '{print $1}')"
restore() {
  cp "$WORK/source.swift" "$SOURCE"
  local actual
  actual="$(shasum -a 256 "$SOURCE" | awk '{print $1}')"
  if [ "$actual" != "$SHA" ]; then echo '回滚 SHA256 不一致' >&2; return 1; fi
}
trap 'rc=$?; restore || rc=1; rm -rf "$WORK"; exit "$rc"' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
run_test() { (cd "$PKG" && TMPDIR="$ROOT/tmp" gt_timeout 300 swift test --filter "$1") >"$WORK/test.log" 2>&1; }
expect_green() {
  if ! run_test "$1"; then cat "$WORK/test.log"; echo "期望通过：${1}" >&2; exit 1; fi
}
TARGET='NativeTabWindowsTests.testSwitchingNativeTabsKeepsLiveTerminalHostsAndProcesses'
CONTROL='NativeTabWindowsTests.testNativeAndSidebarOrderStayInSync'
expect_green "$TARGET"
expect_green "$CONTROL"
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '                               isActive: tab.id == tabs.activeTabID)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '                               isActive: false) // GT_MUTATION_MARK', 1))
PY
if run_test "$TARGET"; then cat "$WORK/test.log"; echo '焦点变异未被检测' >&2; exit 1; fi
if ! grep -Fq '切到第一页时键盘应落在该页终端' "$WORK/test.log"; then
  cat "$WORK/test.log"; echo '失败不在键盘焦点断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
expect_green "$TARGET"
echo 'native-focus mutation PASS: 目标红、对照绿、回滚绿'
