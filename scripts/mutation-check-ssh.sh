#!/usr/bin/env bash
# 变异验证：证明 SSH 后端闸门（ssh_e2e）有牙（非空跑）。
#
# 纪律（照 AGENTS.md）：
#   - 起跑前扫残留变异标记，命中即 exit 2；
#   - trap INT TERM 复原；回滚后比对起跑前记录的 sha256；
#   - 无 sshd 时整体 SKIP（闸门本身会 SKIP，变异无法验证，不假装通过）。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
mkdir -p "$TMP"
SRC="$ROOT/core/src/ssh.c"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

if ! command -v sshd >/dev/null 2>&1 && [ ! -x /usr/sbin/sshd ]; then
  echo "SKIP mutation-check-ssh: 本机无 sshd，闸门会 SKIP，变异无法验证"
  exit 0
fi

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
  echo "检测到残留变异标记（${MARK}）——工作区不干净，拒绝起跑（exit 2）" >&2
  exit 2
fi

BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutssh-XXXXXX")"; BACKUP="$BACKUP.c"
cp "$SRC" "$BACKUP"

run_gate_quiet() { (cd "$ROOT" && bash scripts/gate.sh >/dev/null 2>&1); }
expect_gate() {
  if run_gate_quiet; then actual=green; else actual=red; fi
  if [ "$actual" = "$1" ]; then echo "  ✓ $2 → 闸门 $actual"; PASS=$((PASS+1));
  else echo "  ✗ $2 → 期望 $1，实际 $actual"; FAIL=$((FAIL+1)); fi
}
rollback() {
  cp "$BACKUP" "$SRC"
  local now; now="$(shasum -a 256 "$SRC" | awk '{print $1}')"
  [ "$now" = "$BASE_SHA" ] || { echo "  ✗ 回滚后 sha256 不一致！" >&2; FAIL=$((FAIL+1)); }
}
mutate() { python3 "$2" "$3"; }

echo "== 基线 =="
expect_gate green "基线"

# ---- C1：未知主机密钥被折叠成接受（三态崩坏） ----
echo "== C1：UNKNOWN 折叠成 ACCEPT =="
cat > "$TMP/gtmut_c1.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  } else {\n    decision = GT_HOSTKEY_UNKNOWN;\n  }"
new="  } else {\n    decision = GT_HOSTKEY_ACCEPT; /* GT_MUTATION_MARK */\n  }"
assert old in s, "C1 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate C1 "$TMP/gtmut_c1.py" "$SRC"
expect_gate red "C1 未知主机密钥被接受"
rollback

# ---- C2：known_hosts 不一致被当成一致（密钥变更被吞） ----
echo "== C2：密钥变更（MISMATCH）被当成接受 =="
cat > "$TMP/gtmut_c2.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  if (kh == KH_MISMATCH) {\n    decision = GT_HOSTKEY_REJECT; /* 密钥已变更：绝不自动接受 */"
new="  if (kh == KH_MISMATCH) {\n    decision = GT_HOSTKEY_ACCEPT; /* GT_MUTATION_MARK */"
assert old in s, "C2 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate C2 "$TMP/gtmut_c2.py" "$SRC"
expect_gate red "C2 密钥变更被自动接受"
rollback

# ---- C3：TOFU 不落盘 ----
echo "== C3：TOFU 未写入 known_hosts =="
cat > "$TMP/gtmut_c3.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="      kh_append(ssh->kh_path, ssh->host, port, ktype, b64);"
new="      (void)0; /* GT_MUTATION_MARK */"
assert old in s, "C3 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate C3 "$TMP/gtmut_c3.py" "$SRC"
expect_gate red "C3 TOFU 未落盘"
rollback

# ---- C4：远端 pty 尺寸不下发（resize 无效） ----
echo "== C4：resize 不下发 pty size =="
cat > "$TMP/gtmut_c4.py" <<'PY'
import sys
p=sys.argv[1]; s=open(p).read()
old="  if (libssh2_channel_request_pty_size(sh->chan, cols ? cols : 80,\n                                       rows ? rows : 24) != 0) {"
new="  if (0) { /* GT_MUTATION_MARK */"
assert old in s, "C4 anchor missing"
open(p,'w').write(s.replace(old,new,1))
PY
mutate C4 "$TMP/gtmut_c4.py" "$SRC"
expect_gate red "C4 resize 未下发"
rollback

echo "== 回滚复绿 =="
expect_gate green "回滚后"

restore_all
trap - INT TERM
echo
echo "mutation-check-ssh: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
