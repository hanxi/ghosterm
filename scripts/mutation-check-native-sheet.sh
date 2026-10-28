#!/usr/bin/env bash
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-native-sheet
SOURCE="$ROOT/apple/GhostermApp/Sources/GhostermApp/RootView.swift"
PKG="$ROOT/apple/GhostermApp"
mkdir -p "$ROOT/tmp"
if grep -q GT_MUTATION_MARK "$SOURCE"; then echo '残留变异标记' >&2; exit 2; fi
if ! grep -Fq 'guard !nativeTabs || windowTabID == model.tmuxChooserOwnerID else { return nil }' "$SOURCE"; then
  echo '对话框归属变异锚点不存在' >&2; exit 2
fi
if ! grep -Fq 'Color.clear.frame(height: WindowChrome.barHeight)' "$SOURCE"; then
  echo '原生标签栏留白变异锚点不存在' >&2; exit 2
fi
WORK="$(mktemp -d "$ROOT/tmp/native-sheet-mutation.XXXXXX")"
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
TARGET='NativeTabWindowsTests.testTmuxChooserIsPresentedOnlyOnCurrentNativeTab'
CONTROL='NativeTabWindowsTests.testSwitchingNativeTabsKeepsLiveTerminalHostsAndProcesses'
LAYOUT='NativeTabWindowsTests.testNativeWindowsFollowModelAndCloseTheRequestedTab'
expect_green "$TARGET"
expect_green "$CONTROL"
expect_green "$LAYOUT"
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '            guard !nativeTabs || windowTabID == model.tmuxChooserOwnerID else { return nil }'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '            // GT_MUTATION_MARK: 每个窗口都呈现对话框', 1))
PY
if run_test "$TARGET"; then cat "$WORK/test.log"; echo '对话框归属变异未被检测' >&2; exit 1; fi
if ! grep -Eq '切换标签后不得重复显示对话框|后台标签窗口不得重复弹出同一个对话框' "$WORK/test.log"; then
  cat "$WORK/test.log"; echo '失败不在对话框归属断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
python3 - "$SOURCE" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
old = '                    Color.clear.frame(height: WindowChrome.barHeight)'
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, '                    Color.clear.frame(height: 0) // GT_MUTATION_MARK', 1))
PY
if run_test "$LAYOUT"; then cat "$WORK/test.log"; echo '标签栏遮挡变异未被检测' >&2; exit 1; fi
if ! grep -Fq '原生标签栏不得遮住工作区' "$WORK/test.log"; then
  cat "$WORK/test.log"; echo '失败不在标签栏遮挡断言' >&2; exit 1
fi
expect_green "$CONTROL"
restore
expect_green "$TARGET"
expect_green "$LAYOUT"
echo 'native-sheet mutation PASS: 对话框归属与标签栏遮挡目标红、对照绿、回滚绿'
