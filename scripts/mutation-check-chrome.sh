#!/usr/bin/env bash
# 变异验证：证明「顶栏即标题栏」的几何断言有牙（WindowChromeTests）。
#
# 这类改动**不报错**，只在视觉上不对——红绿灯挂在系统标题栏的高度上
# （约 28/32pt），而我们的顶栏是 38pt，差那几磅肉眼看得出却说不出哪里怪。
#
#   W1 不做 fullSizeContentView（顶栏被压在系统标题栏下）；
#   W2 红绿灯不挪（还是系统标题栏的位置，偏上）；
#   W3 顶栏左侧不留红绿灯的位置（切换按钮压在按钮上）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-chrome

TMP="$ROOT/tmp"
# 判据反了时要当场看到是哪条 swift 断言红（全丢 /dev/null 的话，
# CI 上只剩一行「期望 green，实际 red」＝没有证据）。同一个 expect 只跑一个
# 函数，所以这份日志永远是「刚那一次」的输出。
RUN_LOG="$TMP/gt-mutchrome-run.log"
PKG="$ROOT/apple/GhostermApp"
SRC="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/WindowChrome.swift"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-chrome: 未找到 swift"; exit 0
fi

restore_all() {
  # 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（${MARK}）——拒绝起跑（exit 2）" >&2; exit 2
fi
BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutchrome-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SRC" "$BACKUP"

run_chrome() { ( cd "$PKG" && gt_timeout 300 swift test --filter WindowChromeTests >"$RUN_LOG" 2>&1 ); }
run_other()  { ( cd "$PKG" && gt_timeout 300 swift test --filter TabManagerTests >"$RUN_LOG" 2>&1 ); }
expect() {
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}";
    echo "      —— 最后一次运行的输出（尾 25 行，完整见 ${RUN_LOG}）"
    tail -25 "$RUN_LOG" 2>/dev/null | sed 's/^/      /'
    FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}

echo "== 基线 =="
expect green "窗口外观断言基线" run_chrome
expect green "对照（标签页结构）基线" run_other

echo "== W1：内容不铺满整窗 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        window.styleMask.insert(.fullSizeContentView)"
new = "        /* GT_MUTATION_MARK：不铺满 */"
assert old in s, "W1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "W1 顶栏被压在系统标题栏下" run_chrome
expect green "W1 指向性（标签页结构不受影响）" run_other
rollback

echo "== W2：红绿灯不挪 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "        positionTrafficLights(in: window)"
new = "        /* GT_MUTATION_MARK：红绿灯留在系统位置 */"
assert old in s, "W2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "W2 红绿灯偏上（没贴到顶栏中心）" run_chrome
rollback

echo "== W3：顶栏左侧不留红绿灯位置 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "    public static let trafficLightsWidth: CGFloat = 78"
new = "    public static let trafficLightsWidth: CGFloat = 8 /* GT_MUTATION_MARK */"
assert old in s, "W3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "W3 切换按钮压在红绿灯上" run_chrome
rollback

echo "== W4：双击顶栏忽略系统设置（None 也缩放） =="
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        case "None":'
new = '        case "NoneDISABLED": /* GT_MUTATION_MARK */'
assert old in s, "W4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "W4 「无」设置下双击仍缩放（不听话）" run_chrome
rollback

echo "== 回滚复绿 =="
expect green "回滚后窗口外观断言复绿" run_chrome
expect green "回滚后标签页断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-chrome: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
