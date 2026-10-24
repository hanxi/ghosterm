#!/usr/bin/env bash
# 变异验证：证明**像素级**断言（PixelRenderTests）有牙。
#
# 这组断言最容易变成「验了个假东西」——抓到的帧可能是全黑、可能是系统
# 窗口底色、可能根本不来自终端。每个变异都造出其中一种假绿状态。
#
#   P1 feed 变空操作（字节永不到达网格）→ 背景色断言必须红；
#   P2 抓帧返回固定色（模拟「帧没内容/抓错东西」）→ 必须红；
#   P3 前景文本不再绘制（只留背景）→ 前景断言必须红。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 后面紧跟全角字符时 bash 会把多字节字节吃进变量名，故一律
# 写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-pixel

TMP="$ROOT/tmp"
# 判据反了时要当场看到是哪条 swift 断言红（全丢 /dev/null 的话，
# CI 上只剩一行「期望 green，实际 red」＝没有证据）。同一个 expect 只跑一个
# 函数，所以这份日志永远是「刚那一次」的输出。
RUN_LOG="$TMP/gt-mutpixel-run.log"
PKG="$ROOT/apple/GhostermApp"
SURF="$ROOT/apple/GhostermTerminal/Sources/GhostermTerminal/TerminalSurface.swift"
TEST="$ROOT/apple/GhostermApp/Tests/GhostermAppTests/PixelRenderTests.swift"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP_SURF=""; BACKUP_TEST=""

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-pixel: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-pixel: 缺 GhosttyKit"; exit 0
fi

restore_all() {
# 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP_SURF" ] && [ -f "$BACKUP_SURF" ] && cp "$BACKUP_SURF" "$SURF" && rm -f "$BACKUP_SURF"
  [ -n "$BACKUP_TEST" ] && [ -f "$BACKUP_TEST" ] && cp "$BACKUP_TEST" "$TEST" && rm -f "$BACKUP_TEST"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

for f in "$SURF" "$TEST"; do
  if grep -q "$MARK" "$f" 2>/dev/null; then
    echo "检测到残留变异标记（${MARK}）于 ${f}——拒绝起跑（exit 2）" >&2; exit 2
  fi
done
SHA_SURF="$(shasum -a 256 "$SURF" | awk '{print $1}')"
SHA_TEST="$(shasum -a 256 "$TEST" | awk '{print $1}')"
BACKUP_SURF="$(mktemp "$TMP/gt-mutpix-surf-XXXXXX")"; BACKUP_SURF="$BACKUP_SURF.swift"
BACKUP_TEST="$(mktemp "$TMP/gt-mutpix-test-XXXXXX")"; BACKUP_TEST="$BACKUP_TEST.swift"
cp "$SURF" "$BACKUP_SURF"; cp "$TEST" "$BACKUP_TEST"

run_pixel() { ( cd "$PKG" && gt_timeout 300 swift test --filter PixelRenderTests >"$RUN_LOG" 2>&1 ); }
run_other() { ( cd "$PKG" && gt_timeout 300 swift test --filter TabManagerTests >"$RUN_LOG" 2>&1 ); }

expect() { # $1=期望 red|green $2=描述 $3=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}";
    echo "      —— 最后一次运行的输出（尾 25 行，完整见 ${RUN_LOG}）"
    tail -25 "$RUN_LOG" 2>/dev/null | sed 's/^/      /'
    FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP_SURF" "$SURF"; cp "$BACKUP_TEST" "$TEST"
  local a b
  a="$(shasum -a 256 "$SURF" | awk '{print $1}')"
  b="$(shasum -a 256 "$TEST" | awk '{print $1}')"
  if [ "$a" != "$SHA_SURF" ] || [ "$b" != "$SHA_TEST" ]; then
    echo "  ✗ 回滚后 sha256 与起跑前不一致！" >&2; FAIL=$((FAIL+1))
  fi
}

echo "== 基线 =="
expect green "像素断言基线" run_pixel
expect green "对照（标签页结构）基线" run_other

echo "== P1：feed 变空操作（字节到不了网格） =="
python3 - "$SURF" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    public func feed(_ data: Data) {\n        guard !data.isEmpty else { return }"
new = "    public func feed(_ data: Data) {\n        if true { return } /* GT_MUTATION_MARK */\n        guard !data.isEmpty else { return }"
assert old in s, "P1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P1 终端不再拿到字节（背景色断言红）" run_pixel
expect green "P1 指向性（标签页结构不受影响）" run_other
rollback

echo "== P2：抓帧拿到的是空帧（模拟抓错东西） =="
python3 - "$TEST" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        surface.view.cacheDisplay(in: surface.view.bounds, to: rep)"
new = ("        rep.size = NSSize(width: 1, height: 1) /* GT_MUTATION_MARK："
       "把帧弄成空的 */")
assert old in s, "P2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P2 空帧必须被断言拦下（不静默通过）" run_pixel
rollback

echo "== P3：前景文本不再绘制 =="
python3 - "$TEST" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        surface.feed(Data("\\u{1b}[2J\\u{1b}[H\\(backgroundSGR)\\(text)\\u{1b}[0m".utf8))'
new = ('        surface.feed(Data("\\u{1b}[2J\\u{1b}[H\\(backgroundSGR)\\u{1b}[0m"'
       '.utf8)) /* GT_MUTATION_MARK：不画字 */')
assert old in s, "P3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "P3 只画背景不画字（前景断言红）" run_pixel
rollback

echo "== 回滚复绿 =="
expect green "回滚后像素断言复绿" run_pixel
expect green "回滚后标签页断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-pixel: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
