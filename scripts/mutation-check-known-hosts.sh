#!/usr/bin/env bash
# 变异验证：证明 known_hosts 读侧的两条断言有牙。
#
# 这两条守的是一个**真缺陷**：读侧按 `ssh` 前缀过滤公钥条目（照抄 OpenSSH
# 对 ssh-rsa 的写法），而 ECDSA 的 key type 是 `ecdsa-sha2-nistp256`——
# 整条被跳过。真机 SSH 服务器常给 ECDSA，症状是「点过信任并连接，
# 下次还问」。常规 e2e 用 ed25519，永远盖不到。
#
#   K1 恢复 `ssh` 前缀过滤（ECDSA 条目被跳过）→ 两条 e2e 都红；
#   K2 只读第一行（多算法主机漏掉后面那条）→ 红；
#   K3 裸名条目不再匹配任意端口 → 红。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-known-hosts

TMP="$ROOT/tmp"
SRC="$ROOT/core/src/ssh.c"
# 每个被调用的套件都带 timeout：某些变异会让被测代码**真的卡住**
# （例如「关闭标签页不停会话」的变异：读循环永远在 poll，deinit
# 就永远等下去）。没有超时的话脚本会一直挂着，而不是把那次判成红。
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

mkdir -p "$TMP"

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
  echo "检测到残留变异标记（${MARK}）——拒绝起跑（exit 2）" >&2; exit 2
fi
BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutkh-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

run_kh() { # 两种算法各跑一遍（缺 sshd 时脚本自身 SKIP 并按 0 退出）
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    gt_timeout 300 ctest --test-dir "$ROOT/core/build" -R "known_hosts" >/dev/null 2>&1 )
}
run_other() { # 指向性对照：ssh_config 那批不受影响
  ( cmake --build "$ROOT/core/build" >/dev/null 2>&1 && \
    "$ROOT/core/build/test_config" "$ROOT/core/tests/fixtures" >/dev/null 2>&1 )
}
expect() {
  if "$3"; then actual=green; else actual=red; fi
  if [ "${actual}" = "$1" ]; then echo "  ✓ ${2} → ${actual}"; PASS=$((PASS+1))
  else echo "  ✗ ${2} → 期望 ${1}，实际 ${actual}"; FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}

echo "== 基线 =="
expect green "known_hosts 断言基线" run_kh
expect green "对照（ssh_config）基线" run_other

echo "== K1：恢复「按 ssh 前缀过滤公钥条目」 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    if (!*hosts || !*ktype || !*kb64) continue;
    if (strlen(kb64) < 16) continue;
    if (strpbrk(kb64, "/=") == NULL && strncmp(ktype, "ssh-", 4) != 0)
      continue; /* 既不像 base64 也不像算法名 → 不是公钥条目 */"""
new = """    if (!*hosts || !*ktype || !*kb64) continue;
    if (ktype[0] != 's' || ktype[1] != 's' || ktype[2] != 'h')
      continue; /* GT_MUTATION_MARK：照抄 ssh-rsa 的写法 */"""
assert old in s, "K1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "K1 ECDSA 条目被整条跳过（点过信任还再问）" run_kh
expect green "K1 指向性（ssh_config 不受影响）" run_other
rollback

echo "== K2：只读第一行 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    n++;
    if (!cb(ud, hosts, ktype, kb64, 0)) break;"""
new = """    n++;
    cb(ud, hosts, ktype, kb64, 0);
    break; /* GT_MUTATION_MARK：只看第一行 */"""
assert old in s, "K2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "K2 多算法主机漏掉后面的条目" run_kh
rollback

echo "== K3：裸名条目不再匹配任意端口 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """    if (strcmp(t, q->host) == 0 && t[0] != '[') {"""
new = """    if (false && strcmp(t, q->host) == 0 && t[0] != '[') { /* GT_MUTATION_MARK */"""
assert old in s, "K3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "K3 裸名（不限端口）条目认不出来" run_kh
rollback

echo "== 回滚复绿 =="
expect green "回滚后 known_hosts 断言复绿" run_kh
expect green "回滚后 ssh_config 断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-known-hosts: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
