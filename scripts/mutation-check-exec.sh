#!/usr/bin/env bash
# 变异验证：证明 **SSH exec 通道** 的 e2e 断言有牙（目标是 core/src/ssh.c）。
#
#   E1 exec 的 stderr 读成 stdout（stdout/stderr 分离断言失效）；
#   E2 exec 退出码恒 0（退出码断言失效）。
# 注：命令尾部换行的剥离是**防御性**代码——本机 libssh2 容得下尾换行，
# 造不出「不剥就红」的稳定失效态，故不配变异（诚实标注：这条无闸门）。
#
# 纪律照 AGENTS.md：起跑扫残留标记；trap 复原；回滚比对 sha256；无 sshd SKIP。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-exec

TMP="$ROOT/tmp"
mkdir -p "$TMP"
SRC="$ROOT/core/src/ssh.c"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

if ! command -v sshd >/dev/null 2>&1 && [ ! -x /usr/sbin/sshd ]; then
  echo "SKIP mutation-check-exec: 本机无 sshd，闸门会 SKIP，变异无法验证"
  exit 0
fi

restore_all() {
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
}
trap 'restore_all' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（$MARK）——工作区不干净，拒绝起跑（exit 2）" >&2
  exit 2
fi

BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutexec-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

run_e2e() {
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    gt_timeout 300 ctest --test-dir "$ROOT/core/build" -R ssh_e2e --output-on-failure >/dev/null 2>&1 )
}
expect_e2e() {
  if run_e2e; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → e2e $actual"; PASS=$((PASS+1));
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}
mutate() { python3 "$1" "$SRC"; }
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}
mk() { cat > "$TMP/$1.py"; }

echo "== 基线 =="
expect_e2e green "基线"

echo "== E1：exec 的 stderr 读成 stdout =="
mk gtmut_e1s <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="      ssize_t b = libssh2_channel_read_stderr(e->chan, (char *)buf, cap);"
new="      ssize_t b = libssh2_channel_read(e->chan, (char *)buf, cap); /* GT_MUTATION_MARK */"
assert old in s, "E2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_e1s.py"; expect_e2e red "E1 stderr 混入 stdout"; rollback

echo "== E2：exec 退出码恒 0 =="
mk gtmut_e2s <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  int rc = libssh2_channel_get_exit_status(e->chan);"
new="  int rc = 0; (void)e->chan; /* GT_MUTATION_MARK */"
assert old in s, "E3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate "$TMP/gtmut_e2s.py"; expect_e2e red "E2 退出码断言失效"; rollback

echo "== 回滚复绿 =="
expect_e2e green "回滚后"

restore_all
trap - INT TERM
echo
echo "mutation-check-exec: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
