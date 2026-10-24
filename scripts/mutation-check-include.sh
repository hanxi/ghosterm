#!/usr/bin/env bash
# 变异验证：证明 Include 展开断言有牙（非空跑）。
#
# 变异目标（都是「目标失效状态」）：
#   I1 相对 include 基准目录指向别处 → e0 相对路径断言红；
#   I2 递归深度上限失效（depth<0 恒假）→ 自环 include 无限递归，
#      进程崩溃（SIGSEGV/SIGTRAP）＝非零退出＝红；
#   I3 无损写回不再跳过 included 节点（铁律 #4 破坏）→ 崩溃/红；
#   I4 include_error 不传播 → 「具名缺失必失败」「自环必拦下」红。
# 纪律（AGENTS.md）：起跑前扫残留标记；trap 复原；回滚比对 sha256。
# 每次 I 变异前重建（防 timeout 300 ctest/旧二进制假绿——SFTP 变异教训）。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
SRC="$ROOT/core/src/config.c"
FIXTURES="$ROOT/core/tests/fixtures"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

restore_all() {
  # 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap
  # 会拿**旧备份**把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  # 文件里没有标记 = 现场已不是变异态 → 什么都别动。
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
restore_all() { [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"; }
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（${MARK}）——拒绝起跑（exit 2）" >&2
  exit 2
fi

BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutinc-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

# 变异下预期会出现崩溃（SIGSEGV/SIGTRAP）——崩溃本身就是「红」，但 shell
# 会往 stderr 打一行 job 终止提示；连同其一起吞掉，保持输出干净。
#
# 构建与跑测试**分开报**：构建失败（并发占着 build 目录等）与断言变红是
# 两件事，混在一起会让一次偶发红变成无从查起的谜（本脚本踩过：链式跑三个
# 闸门时出现过一次「回滚复绿」假红，没留输出，事后无法归因）。
run_suite() {
  if ! cmake --build "$ROOT/core/build" >"$TMP/gt-mutinc-build.log" 2>&1; then
    LAST_REASON="构建失败"
    return 1
  fi
  if ! timeout 60 "$ROOT/core/build/test_config" "$FIXTURES" \
        >"$TMP/gt-mutinc-run.log" 2>&1; then
    LAST_REASON="断言红（$(grep -c '^FAIL' "$TMP/gt-mutinc-run.log" 2>/dev/null || echo 0) 条 FAIL）"
    return 1
  fi
  LAST_REASON=""
  return 0
} 2>/dev/null
LAST_REASON=""
cleanup_fixture_dirs() { rm -rf "$FIXTURES"/inc-e2e-* 2>/dev/null || true; }
expect() { # expect <want:red|green> <label>
  cleanup_fixture_dirs
  if run_suite; then actual=green; else actual=red; fi
  cleanup_fixture_dirs
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → $actual"; PASS=$((PASS+1));
  else
    echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}（${LAST_REASON}）"; FAIL=$((FAIL+1))
    # 意外的红：把证据留在原地（这两份日志是唯一能归因的东西）。
    [ "$1" = green ] && {
      echo "    —— 构建尾部 ——"; tail -5 "$TMP/gt-mutinc-build.log" 2>/dev/null
      echo "    —— 运行尾部 ——"; tail -10 "$TMP/gt-mutinc-run.log" 2>/dev/null
    }
  fi
}
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}

mutate() { python3 "$1" "$SRC"; }

# 每个变异的锚点与替换内容都钉在脚本里（不依赖外部临时 py 文件）。
MUTDIR="$TMP"; mkdir -p "$MUTDIR"

echo "== 基线 =="
expect green "基线"

echo "== I1：相对 include 基准目录失效 =="
cat > "$MUTDIR/gtmut_i1.py" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '  const char *base_dir = sshdir_buf;'
new = '  const char *base_dir = "/nonexistent-mutation"; /* GT_MUTATION_MARK */'
assert old in s, "I1 anchor missing"
open(p, 'w').write(s.replace(old, new, 1))
PY
mutate "$MUTDIR/gtmut_i1.py"
expect red "I1 相对 include 找不到基准目录"
rollback

echo "== I2：递归深度上限失效 =="
cat > "$MUTDIR/gtmut_i2.py" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '  if (depth >= GT_CONFIG_MAX_INCLUDE_DEPTH) {'
new = '  if (depth < 0) { /* GT_MUTATION_MARK：上限失效 */'
assert old in s, "I2 anchor missing"
open(p, 'w').write(s.replace(old, new, 1))
PY
mutate "$MUTDIR/gtmut_i2.py"
expect red "I2 自环 include 无上限（崩溃＝红）"
rollback

echo "== I3：写回泄漏被含内容 =="
cat > "$MUTDIR/gtmut_i3.py" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '    if (n->included) continue;'
new = '    if (0) { /* GT_MUTATION_MARK */ }'
assert old in s, "I3 anchor missing"
open(p, 'w').write(s.replace(old, new, 1))
PY
mutate "$MUTDIR/gtmut_i3.py"
expect red "I3 被含内容泄漏进写回"
rollback

echo "== I4：include_error 不传播 =="
cat > "$MUTDIR/gtmut_i4.py" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '    if (cfg->include_error) {'
new = '    if (0 && cfg->include_error) { /* GT_MUTATION_MARK */'
assert old in s, "I4 anchor missing"
open(p, 'w').write(s.replace(old, new, 1))
PY
mutate "$MUTDIR/gtmut_i4.py"
expect red "I4 具名缺失/自环不再 parse 失败"
rollback

echo "== 回滚复绿 =="
expect green "回滚后"

restore_all
trap - INT TERM
rm -f "$MUTDIR"/gtmut_i*.py
echo
echo "mutation-check-include: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
