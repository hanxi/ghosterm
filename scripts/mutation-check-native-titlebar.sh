#!/usr/bin/env bash
# Prove the AppKit titlebar placement is observed by the test.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-native-titlebar
SOURCE="$ROOT/apple/GhostermApp/Sources/GhostermApp/NativeTitlebarWindow.swift"
PKG="$ROOT/apple/GhostermApp"
mkdir -p "$ROOT/tmp"
if grep -q GT_MUTATION_MARK "$SOURCE"; then echo '残留变异标记' >&2; exit 2; fi
if ! grep -Fq 'NSLayoutConstraint.activate(tabConstraints)' "$SOURCE"; then echo '标题栏变异锚点不存在' >&2; exit 2; fi
if ! grep -Fq 'constant: WindowChrome.trafficLightsWidth + 3 * DS.controlSize + 5 * DS.Padding.s' "$SOURCE"; then
  echo '标题栏控件留白锚点不存在' >&2; exit 2
fi
if ! grep -Fq 'appKitTabItem($0.tab, ivar: "_tabBarItem") === item' "$SOURCE"; then
  echo '关闭按钮归属变异锚点不存在' >&2; exit 2
fi
WORK="$(mktemp -d "$ROOT/tmp/native-titlebar-mutation.XXXXXX")"
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
TARGET='NativeTabWindowsTests.testNativeWindowsFollowModelAndCloseTheRequestedTab'
CONTROL='NativeTabWindowsTests.testNativeAndSidebarOrderStayInSync'
CLOSE='NativeTabWindowsTests.testNativeCloseRequestsConfirmationForTheTarget'
expect_green "$TARGET"
expect_green "$CONTROL"
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '        NSLayoutConstraint.activate(tabConstraints)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '        NSLayoutConstraint.deactivate(tabConstraints) // GT_MUTATION_MARK', 1))
PY
if run_test "$TARGET"; then cat "$WORK/test.log"; echo '标题栏位置变异未被检测' >&2; exit 1; fi
if ! grep -Fq '原生标签栏应占据标题栏可用宽度' "$WORK/test.log"; then
  cat "$WORK/test.log"; echo '失败不在标题栏位置断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = 'constant: WindowChrome.trafficLightsWidth + 3 * DS.controlSize + 5 * DS.Padding.s'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, 'constant: 160 /* GT_MUTATION_MARK */', 1))
PY
if run_test "$TARGET"; then cat "$WORK/test.log"; echo '控件遮挡变异未被检测' >&2; exit 1; fi
if ! grep -Fq '标签栏不得覆盖配置错误提示' "$WORK/test.log"; then
  cat "$WORK/test.log"; echo '失败不在控件遮挡断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
expect_green "$CLOSE"
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '                  appKitTabItem($0.tab, ivar: "_tabBarItem") === item'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '                  _ = $0; _ = item; return true // GT_MUTATION_MARK', 1))
PY
if run_test "$CLOSE"; then cat "$WORK/test.log"; echo '关闭归属变异未被检测' >&2; exit 1; fi
if ! grep -Fq '原生关闭必须询问指定的标签' "$WORK/test.log"; then
  cat "$WORK/test.log"; echo '失败不在关闭目标断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
expect_green "$CLOSE"
expect_green "$TARGET"
echo 'native-titlebar mutation PASS: 本机适用的目标红、对照绿、回滚绿'
