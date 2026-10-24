#!/usr/bin/env bash
# 变异验证：证明**焦点下发**断言（WindowFocusTests）有牙。
#
# 这条最容易变成「验了个假东西」——屏幕上不会报任何错，焦点没下发时每个窗格
# 都停在 libghostty 的「默认已聚焦」上一起闪。每个变异都造出一种「看起来对、
# 其实焦点没到引擎」的状态：
#
#   F1 掐断下发（TerminalSurface.setFocused 变空操作）→ 焦点断言必须红；
#   F2 改焦点不下发（paneFocused.didSet 不调 applyFocus）→ 必须红；
#   F3 漏掉「窗口是否 key」（effectiveFocus 只看 paneFocused）→ 必须红。
#
# 后两条盯的是**另一件事**：键盘（AppKit firstResponder）有没有跟着当前窗格走。
# 只下发引擎焦点位时屏幕同样不报错——新窗格光标在闪，敲字却进了上一个终端。
#
#   F4 切窗格不要键盘（paneFocused.didSet 不调 claimFirstResponder）→ 必须红；
#   F5 进树不补要键盘（viewDidMoveToWindow 不调 claimFirstResponder）→ 必须红。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 后面紧跟全角字符时 bash 会把多字节字节吃进变量名，故一律
# 写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-focus

TMP="$ROOT/tmp"
# 判据反了时要当场看到是哪条 swift 断言红（全丢 /dev/null 的话，
# CI 上只剩一行「期望 green，实际 red」＝没有证据）。同一个 expect 只跑一个
# 函数，所以这份日志永远是「刚那一次」的输出。
RUN_LOG="$TMP/gt-mutfocus-run.log"
PKG="$ROOT/apple/GhostermApp"
SURF="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurface.swift"
VIEW="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift"
TEST="$ROOT/apple/GhostermApp/Tests/GhostermAppTests/WindowFocusTests.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP_SURF=""; BACKUP_VIEW=""; BACKUP_TEST=""

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-focus: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-focus: 缺 GhosttyKit"; exit 0
fi

restore_all() {
  # 只有现场还在变异态才复原（见 mutation-check-pixel.sh 的同名说明）。
  if ! grep -q "$MARK" "$SURF" "$VIEW" "$TEST" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP_SURF" ] && [ -f "$BACKUP_SURF" ] && cp "$BACKUP_SURF" "$SURF" && rm -f "$BACKUP_SURF"
  [ -n "$BACKUP_VIEW" ] && [ -f "$BACKUP_VIEW" ] && cp "$BACKUP_VIEW" "$VIEW" && rm -f "$BACKUP_VIEW"
  [ -n "$BACKUP_TEST" ] && [ -f "$BACKUP_TEST" ] && cp "$BACKUP_TEST" "$TEST" && rm -f "$BACKUP_TEST"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "$SURF" "$VIEW" "$TEST"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done
SHA_SURF="$(shasum -a 256 "$SURF" | awk '{print $1}')"
SHA_VIEW="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
SHA_TEST="$(shasum -a 256 "$TEST" | awk '{print $1}')"
BACKUP_SURF="$(mktemp "$TMP/gt-mutfocus-surf-XXXXXX")"; BACKUP_SURF="$BACKUP_SURF.swift"
BACKUP_VIEW="$(mktemp "$TMP/gt-mutfocus-view-XXXXXX")"; BACKUP_VIEW="$BACKUP_VIEW.swift"
BACKUP_TEST="$(mktemp "$TMP/gt-mutfocus-test-XXXXXX")"; BACKUP_TEST="$BACKUP_TEST.swift"
cp "$SURF" "$BACKUP_SURF"; cp "$VIEW" "$BACKUP_VIEW"; cp "$TEST" "$BACKUP_TEST"

run_focus() { ( cd "$PKG" && gt_timeout 420 swift test --filter WindowFocusTests >"$RUN_LOG" 2>&1 ); }
run_other() { ( cd "$PKG" && gt_timeout 420 swift test --filter ProgramStatusStoreTests >"$RUN_LOG" 2>&1 ); }

expect() { # $1=期望 red|green $2=描述 $3=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}";
    echo "      —— 最后一次运行的输出（尾 25 行，完整见 ${RUN_LOG}）"
    tail -25 "$RUN_LOG" 2>/dev/null | sed 's/^/      /'
    FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP_SURF" "$SURF"; cp "$BACKUP_VIEW" "$VIEW"; cp "$BACKUP_TEST" "$TEST"
  local a b c
  a="$(shasum -a 256 "$SURF" | awk '{print $1}')"
  b="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
  c="$(shasum -a 256 "$TEST" | awk '{print $1}')"
  if [ "$a" != "$SHA_SURF" ] || [ "$b" != "$SHA_VIEW" ] || [ "$c" != "$SHA_TEST" ]; then
    echo "  ✗ 回滚后 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1))
  fi
}

echo "== 基线 =="
expect green "焦点断言基线" run_focus
expect green "对照（程序状态结构）基线" run_other

echo "== F1：掐断下发（setFocused 变空操作） =="
python3 - "$SURF" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("    public func setFocused(_ focused: Bool) {\n"
       "        gt_ghostty_surface_set_focus(raw, focused)\n    }")
new = ("    public func setFocused(_ focused: Bool) {\n"
       "        _ = focused /* GT_MUTATION_MARK：焦点不往引擎下发 */\n    }")
assert old in s, "F1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "F1 下发被掐断（焦点断言红）" run_focus
expect green "F1 指向性（程序状态结构不受影响）" run_other
rollback

echo "== F2：改焦点不下发（paneFocused.didSet 不调 applyFocus） =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("        didSet {\n            guard paneFocused != oldValue else { return }\n"
       "            applyFocus()\n            claimFirstResponder()\n        }")
new = ("        didSet {\n            guard paneFocused != oldValue else { return }\n"
       "            /* GT_MUTATION_MARK：切窗格不下发焦点 */\n"
       "            claimFirstResponder()\n        }")
assert old in s, "F2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "F2 切窗格不下发（焦点断言红）" run_focus
expect green "F2 指向性" run_other
rollback

echo "== F3：漏掉「窗口是否 key」 =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        paneFocused && (window?.isKeyWindow ?? false)"
new = "        paneFocused /* GT_MUTATION_MARK：漏看窗口 key */"
assert old in s, "F3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "F3 失 key 仍报聚焦（焦点断言红）" run_focus
expect green "F3 指向性" run_other
rollback

echo "== F4：切窗格不要键盘（didSet 不调 claimFirstResponder） =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = ("            applyFocus()\n            claimFirstResponder()\n        }")
new = ("            applyFocus()\n"
       "            /* GT_MUTATION_MARK：切窗格不把键盘要过来 */\n        }")
assert old in s, "F4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "F4 键盘留在旧窗格（焦点断言红）" run_focus
expect green "F4 指向性" run_other
rollback

echo "== F5：进树不补要键盘（viewDidMoveToWindow 不调 claimFirstResponder） =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
i = s.index("    public override func viewDidMoveToWindow() {")
j = s.index("    /// 生效焦点", i)
body = s[i:j]
assert "claimFirstResponder()" in body, "F5 锚点缺失"
mutated = body.replace("        claimFirstResponder()\n",
                       "        /* GT_MUTATION_MARK：进树不补要键盘 */\n", 1)
open(p, 'w').write(s[:i] + mutated + s[j:])
PY
expect red "F5 新建窗格要不到键盘（焦点断言红）" run_focus
expect green "F5 指向性" run_other
rollback

echo "== 回滚复绿 =="
expect green "回滚后焦点断言复绿" run_focus
expect green "回滚后对照仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-focus: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
