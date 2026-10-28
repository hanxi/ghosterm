#!/usr/bin/env bash
# 普通默认入口：真实 PTY、启动偏好、分屏目录与关闭确认。变异只改隔离副本。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-default-entry
MARK=GT_DEFAULT_ENTRY_MUTATION_MARK
SOURCE=apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift
mkdir -p "$ROOT/tmp"
if grep -qF "$MARK" "$ROOT/$SOURCE"; then echo '残留变异标记，拒绝起跑' >&2; exit 2; fi
WORK="$(mktemp -d "$ROOT/tmp/default-entry-XXXXXX")"
BASE_SHA="$(shasum -a 256 "$ROOT/$SOURCE" | awk '{print $1}')"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
export TMPDIR="$ROOT/tmp"
python3 - "$ROOT" "$WORK" <<'PY'
import shutil, sys
from pathlib import Path
root, work = map(Path, sys.argv[1:])
(work/'apple').mkdir()
for name in ('GhostermApp', 'GhostermCore', 'GhostermTerminal'):
    shutil.copytree(root/'apple'/name, work/'apple'/name, symlinks=True,
                    ignore=shutil.ignore_patterns('.build', '.swiftpm'))
for name in ('core', 'terminal', 'assets'):
    (work/name).symlink_to(root/name, target_is_directory=True)
(work/'build').mkdir()
(work/'build/GhosttyKit.xcframework').symlink_to(root/'build/GhosttyKit.xcframework', target_is_directory=True)
# 故意不提供 build/runtime，证明普通生产入口和夹具都不依赖 daemon 清单。
PY
cp "$ROOT/$SOURCE" "$WORK/original.swift"
restore() {
  cp "$WORK/original.swift" "$WORK/$SOURCE"
  local candidate
  for candidate in "$ROOT/$SOURCE" "$WORK/$SOURCE"; do
    [ "$(shasum -a 256 "$candidate" | awk '{print $1}')" = "$BASE_SHA" ] || {
      echo "SHA256 回滚失败：${candidate}" >&2; return 1;
    }
  done
}
trap restore EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
probe() {
  local expected=$1 filter=$2 log=$3 rc=0
  gt_timeout 240 xcrun swift test --package-path "$WORK/apple/GhostermApp" --filter "$filter" >"$log" 2>&1 || rc=$?
  if [ "$expected" = green ]; then
    if [ "$rc" = 0 ] && grep -Eq 'Executed [1-9][0-9]* tests?, with 0 failures' "$log"; then return; fi
  else
    if [ "$rc" = 1 ] && grep -qF 'Build complete!' "$log" && grep -Eq 'XCTAssert|: failed - ' "$log" \
       && ! grep -Eq 'unexpected signal|Program crashed|error: emit-module|error: fatalError' "$log"; then return; fi
  fi
  cat "$log" >&2; echo "判据不符：期望 ${expected}，退出码 ${rc}" >&2; return 1
}
probe green DefaultSessionTests "$WORK/baseline.log"
probe green HostListTests.testReadingDoesNotMutateConfig "$WORK/control.log"
for scenario in local startup cwd confirm; do
  restore
  python3 - "$WORK/$SOURCE" "$scenario" "$MARK" <<'PY'
from pathlib import Path
import sys
p, scenario, mark = Path(sys.argv[1]), sys.argv[2], sys.argv[3]
text = p.read_text()
specs = {
    'local': ('startLocal(pane: pane, app: terminalApp, cwd: cwd)', 'pane.markConnecting()'),
    'startup': ('if tabs.tabs.isEmpty && preferences.shellOnLaunch {', 'if false && tabs.tabs.isEmpty && preferences.shellOnLaunch {'),
    'cwd': ('startLocal(pane: pane, app: terminalApp, cwd: startCwd)', 'startLocal(pane: pane, app: terminalApp, cwd: nil)'),
    'confirm': ('return tab.panes.contains { $0.closeNeedsConfirmation }', 'return false'),
}
old, new = specs[scenario]
assert text.count(old) == 1, ('mutation anchor drift', scenario)
p.write_text(text.replace(old, new) + '\n// ' + mark + '\n')
PY
  probe red DefaultSessionTests "$WORK/$scenario-red.log"
  probe green HostListTests.testReadingDoesNotMutateConfig "$WORK/$scenario-control.log"
  echo "PASS default-entry/${scenario}：目标红、同变异对照绿"
done
restore
probe green DefaultSessionTests "$WORK/restored.log"
echo "PASS default-entry：4 组变异与 SHA256 回滚；日志 ${WORK}"
