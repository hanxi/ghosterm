#!/usr/bin/env bash
# 变异验证：证明「本地 PTY 的子进程拿到控制终端」「前台作业判定」与「写绝不
# 无限等（卡住会拖垮整个 app）」三组断言有牙
# （core/tests/test_transport.c 第 11 节 child-state + 第 13 节 write-flow）。
#
#   P1 把父进程的 setpgid(pid, pid) 加回来（曾经的写法）→ 子进程 setsid() 被
#      憋成 EPERM、拿不到控制终端 → tcgetpgrp(master) 恒 0 → child-state 的
#      「空闲=IDLE」断言变红。这条守的就是「本地 shell 根本没有控制终端」。
#   P2 让 UNKNOWN 折叠成 IDLE（`!t` 分支返回 IDLE）→ 「NULL=UNKNOWN（不折叠）」
#      断言变红。铁律 #6：查不出来时绝不能装成空闲。
#   P3 把 close 里「先关 master」那步挪回 join 之后（回到挂断前的顺序）→
#      「有前台作业时 close 要秒关」断言变红——会话首进程卡在撤销控制终端上。
#   P4 把写队列满时的有界等待改回 `poll(...,-1)`（旧写法）→ write-flow 节
#      「写要有界返回」变红（整个套件卡死到 timeout）——用户实测的根因：
#      卡住的后台程序把 pty 输入队列撑满，写就在 poll 上无限等，会话线程被
#      钉死 → 读循环出不来（终端冻住）+ stop() 的 queue.sync 把主线程也钉死
#      （整个 app 无响应）。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap 复原（EXIT 也复原：
# 被 kill -9 时 INT/TERM 不跑，变异会留在源码里）；回滚后比对起跑前 sha256。
# 写脚本注意：`$var` 后缀全角字符会被 bash 吃进变量名，故一律写 ${var}。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-transport

TMP="$ROOT/tmp"
SRC="$ROOT/core/src/io/pty_posix.c"
FACADE="$ROOT/core/src/transport.c"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""
FACADE_BACKUP=""

mkdir -p "$TMP"

restore_all() {
  # 只有现场还在变异态才复原：脚本被 kill -9 后，残留的 EXIT trap 会拿旧备份
  # 把源文件盖回去、抹掉别人的新改动（AGENTS 记录过这个坑）。
  if grep -q "$MARK" "$SRC" 2>/dev/null; then
    if [ -n "$BACKUP" ] && [ -f "$BACKUP" ]; then
      cp "$BACKUP" "$SRC"
      [ "$(shasum -a 256 "$SRC" | awk '{print $1}')" = "$BASE_SHA" ] || return 1
    fi
  fi
  if grep -q "$MARK" "$FACADE" 2>/dev/null; then
    if [ -n "$FACADE_BACKUP" ] && [ -f "$FACADE_BACKUP" ]; then
      cp "$FACADE_BACKUP" "$FACADE"
      [ "$(shasum -a 256 "$FACADE" | awk '{print $1}')" = "$FACADE_SHA" ] || return 1
    fi
  fi
}
cleanup() {
  local rc=$?
  restore_all || rc=1
  [ -z "$BACKUP" ] || rm -f "$BACKUP"
  [ -z "$FACADE_BACKUP" ] || rm -f "$FACADE_BACKUP"
  exit "$rc"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

if grep -q "$MARK" "$SRC" "$FACADE" 2>/dev/null; then
  echo "检测到残留变异标记（${MARK}）——拒绝起跑（exit 2）" >&2; exit 2
fi
BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
FACADE_SHA="$(shasum -a 256 "$FACADE" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-muttransport-XXXXXX")"
FACADE_BACKUP="$(mktemp "$TMP/gt-muttransport-facade-XXXXXX")"
cp "$SRC" "$BACKUP"
cp "$FACADE" "$FACADE_BACKUP"

# 每个被调用的套件都带 timeout：P3 会让 close 真的卡住（直到作业自己结束）。
run_transport() {
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    gt_timeout 180 "$ROOT/core/build/test_transport" "$TMP" >/dev/null 2>&1 )
}
run_config() { # 指向性对照：另一组核心断言（ssh_config）在同样变异下仍绿
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    gt_timeout 120 "$ROOT/core/build/test_config" "$ROOT/core/tests/fixtures" >/dev/null 2>&1 )
}
expect() { # ${1}=期望 red|green ${2}=描述 ${3}=函数名
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP" "$SRC"
  cp "$FACADE_BACKUP" "$FACADE"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
  now="$(shasum -a 256 "$FACADE" | awk '{print $1}')"
  [ "$now" = "$FACADE_SHA" ] || { echo "  ✗ 门面回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}

if ! command -v python3 >/dev/null 2>&1; then
  echo "SKIP mutation-check-transport: 未找到 python3"; exit 0
fi

echo "== 基线 =="
expect green "pty 断言基线" run_transport
expect green "对照（ssh_config）基线" run_config

# ---- P1：父进程抢跑 setpgid → 子进程拿不到控制终端 ----
echo "== P1：父进程 setpgid 竞争（子进程失去控制终端） =="
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = "  t->child = pid;\n  /*"
new = "  t->child = pid;\n  setpgid(pid, pid); /* GT_MUTATION_MARK：把竞争加回来 */\n  /*"
assert old in s, "P1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "P1 空闲的本地 shell 判不出 IDLE（退化成 UNKNOWN）" run_transport
expect green "P1 指向性（ssh_config 不受影响）" run_config
rollback

# ---- P2：把 UNKNOWN 折叠成 IDLE ----
echo "== P2：UNKNOWN 被折叠成 IDLE =="
python3 - "$FACADE" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = "  if (!t) return GT_TRANSPORT_CHILD_UNKNOWN;"
new = "  if (!t) return GT_TRANSPORT_CHILD_IDLE; /* GT_MUTATION_MARK：折叠未知 */"
assert old in s, "P2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "P2 查不出来却装了空闲（铁律 #6 被破）" run_transport
expect green "P2 指向性（ssh_config 不受影响）" run_config
rollback

# ---- P3：close 不再先挂断 master ----
echo "== P3：close 不再先关 master（回到 join 之后） =="
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = """  if (t->master_fd >= 0) {
    close(t->master_fd);
    t->master_fd = -1;
  }
  if (t->has_waiter) pthread_join(t->waiter, NULL);"""
new = """  if (t->has_waiter) pthread_join(t->waiter, NULL);
  if (t->master_fd >= 0) {
    close(t->master_fd);
    t->master_fd = -1;
  } /* GT_MUTATION_MARK：挂断挪到 join 之后 */"""
assert old in s, "P3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "P3 有前台作业时 close 卡住（不秒关）" run_transport
expect green "P3 指向性（ssh_config 不受影响）" run_config
rollback

# ---- P4：写队列满时无限等（回到 poll(-1)） ----
echo "== P4：写队列满时 poll(-1) 无限等（会钉死会话线程 → app 卡死） =="
python3 - "$SRC" <<'PYEOF'
import sys
p = sys.argv[1]; s = open(p).read()
old = """      int pr = poll(&p, 1, (int)rem);"""
new = """      int pr = poll(&p, 1, -1); /* GT_MUTATION_MARK：无限等（旧写法） */"""
assert old in s, "P4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYEOF
expect red "P4 写队列满时无界等待（会话线程被钉死）" run_transport
expect green "P4 指向性（ssh_config 不受影响）" run_config
rollback

echo "== 回滚复绿 =="
expect green "回滚后 pty 断言复绿" run_transport
expect green "回滚后 ssh_config 断言仍绿" run_config

restore_all
echo
echo "mutation-check-transport: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
