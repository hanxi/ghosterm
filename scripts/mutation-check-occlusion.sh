#!/usr/bin/env bash
# 变异验证：证明**可见性（occlusion）**断言（WindowFocusTests 的可见性两条 +
# TerminalSurfaceTests 的引擎侧一条）有牙。
#
# 这条同样是「不报错、只是白烧 CPU/GPU」：可见性没下发时，后台标签页的 surface
# 会在没人看的时候继续跑渲染线程。每个变异造出其中一种假绿：
#
#   O1 可见性不下发（shim 变空操作）→ 可见性断言必须红；
#   O2 改可见性不下发（paneVisible.didSet 不调 applyVisible）→ 必须红；
#   O3 漏看窗口（effectiveVisibility 只看 paneVisible）→ 必须红。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 后面紧跟全角字符时 bash 会把多字节字节吃进变量名，故一律
# 写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermApp"
SHIM="$ROOT/apple/GhostermTerminal/Sources/CGhostty/shim.c"
VIEW="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurfaceView.swift"
TEST="$ROOT/apple/GhostermApp/Tests/GhostermAppTests/WindowFocusTests.swift"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP_SHIM=""; BACKUP_VIEW=""; BACKUP_TEST=""

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-occlusion: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-occlusion: 缺 GhosttyKit"; exit 0
fi

restore_all() {
  # 只有现场还在变异态才复原（见 mutation-check-pixel.sh 的同名说明）。
  if ! grep -q "$MARK" "$SHIM" "$VIEW" "$TEST" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP_SHIM" ] && [ -f "$BACKUP_SHIM" ] && cp "$BACKUP_SHIM" "$SHIM" && rm -f "$BACKUP_SHIM"
  [ -n "$BACKUP_VIEW" ] && [ -f "$BACKUP_VIEW" ] && cp "$BACKUP_VIEW" "$VIEW" && rm -f "$BACKUP_VIEW"
  [ -n "$BACKUP_TEST" ] && [ -f "$BACKUP_TEST" ] && cp "$BACKUP_TEST" "$TEST" && rm -f "$BACKUP_TEST"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "$SHIM" "$VIEW" "$TEST"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done
SHA_SHIM="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
SHA_VIEW="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
SHA_TEST="$(shasum -a 256 "$TEST" | awk '{print $1}')"
BACKUP_SHIM="$(mktemp "$TMP/gt-mutocc-shim-XXXXXX")"; BACKUP_SHIM="$BACKUP_SHIM.c"
BACKUP_VIEW="$(mktemp "$TMP/gt-mutocc-view-XXXXXX")"; BACKUP_VIEW="$BACKUP_VIEW.swift"
BACKUP_TEST="$(mktemp "$TMP/gt-mutocc-test-XXXXXX")"; BACKUP_TEST="$BACKUP_TEST.swift"
cp "$SHIM" "$BACKUP_SHIM"; cp "$VIEW" "$BACKUP_VIEW"; cp "$TEST" "$BACKUP_TEST"

run_occ() { ( cd "$PKG" && timeout 420 swift test --filter WindowFocusTests >/dev/null 2>&1 ); }
run_other() { ( cd "$PKG" && timeout 420 swift test --filter ProgramStatusStoreTests >/dev/null 2>&1 ); }

expect() { # $1=期望 red|green $2=描述 $3=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP_SHIM" "$SHIM"; cp "$BACKUP_VIEW" "$VIEW"; cp "$BACKUP_TEST" "$TEST"
  local a b c
  a="$(shasum -a 256 "$SHIM" | awk '{print $1}')"
  b="$(shasum -a 256 "$VIEW" | awk '{print $1}')"
  c="$(shasum -a 256 "$TEST" | awk '{print $1}')"
  if [ "$a" != "$SHA_SHIM" ] || [ "$b" != "$SHA_VIEW" ] || [ "$c" != "$SHA_TEST" ]; then
    echo "  ✗ 回滚后 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1))
  fi
}

echo "== 基线 =="
expect green "可见性断言基线" run_occ
expect green "对照（程序状态结构）基线" run_other

echo "== O1：可见性不下发（shim 变空操作） =="
python3 - "$SHIM" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """void gt_ghostty_surface_set_occlusion(gt_ghostty_surface_t *s, bool visible) {
  if (!s || !s->surface) return;"""
new = """void gt_ghostty_surface_set_occlusion(gt_ghostty_surface_t *s, bool visible) {
  (void)s; (void)visible; return; /* GT_MUTATION_MARK：可见性不下发 */
  if (!s || !s->surface) return;"""
assert old in s, "O1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "O1 可见性没下发（可见性断言红）" run_occ
expect green "O1 指向性（程序状态结构不受影响）" run_other
rollback

echo "== O2：改可见性不下发（paneVisible.didSet 不调 applyVisible） =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        didSet {
            guard paneVisible != oldValue else { return }
            applyVisible()
        }"""
new = """        didSet {
            guard paneVisible != oldValue else { return }
            /* GT_MUTATION_MARK：切标签页不下发可见性 */
        }"""
assert old in s, "O2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "O2 切标签页不下发（可见性断言红）" run_occ
expect green "O2 指向性" run_other
rollback

echo "== O3：漏看窗口（effectiveVisibility 只看 paneVisible） =="
python3 - "$VIEW" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        guard paneVisible, let window else { return false }
        return window.isVisible && !window.isMiniaturized
            && window.occlusionState.contains(.visible)"""
new = """        return paneVisible /* GT_MUTATION_MARK：漏看窗口是否在屏幕上 */"""
assert old in s, "O3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "O3 被遮挡/最小化时仍报可见（断言红）" run_occ
expect green "O3 指向性" run_other
rollback

echo "== 回滚复绿 =="
expect green "回滚后可见性断言复绿" run_occ
expect green "回滚后对照仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-occlusion: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
