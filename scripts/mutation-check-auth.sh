#!/usr/bin/env bash
# 变异验证：证明「认证桥」那批断言有牙（SSHAuthTests，真 sshd e2e）。
#
# 这批断言护的是用户实测报回来的那条：
#   「为何 ssh 提示需要私钥口令？我用 ssh 命令行测试过这个密钥是没口令的。」
# 三个根因各一个变异：
#   A1 `~` 展开把分隔符吃掉（`/Users/x.ssh/id_rsa`）→ 默认钥匙链一把都读不到；
#   A2 「文件读得出来就先弹口令框」，没加密的钥匙也被问口令；
#   A3 「服务器允许什么」的方法表在握手**前**读（恒为空）→ 一律猜「私钥口令」。
#
# 纪律（AGENTS.md）：起跑前扫残留标记（命中 exit 2）；trap INT TERM 复原；
# 回滚后比对起跑前记录的 sha256。无 sshd 时整体 SKIP（闸门也会 SKIP，
# 变异无从验证，不假装通过）。
#
# 注意：`$var` 紧跟全角字符会被 bash 吃进变量名，故一律写 `${var}`。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require mutation-check-auth

TMP="$ROOT/tmp"
SRC="$ROOT/apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift"
PKG="$ROOT/apple/GhostermApp"
SSHD="${SSHD:-/usr/sbin/sshd}"
MARK="GT_MUTATION_MARK"
PASS=0; FAIL=0
BACKUP=""

mkdir -p "$TMP"
if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP mutation-check-auth: 未找到 swift"; exit 0
fi
if [ ! -d "$ROOT/build/GhosttyKit.xcframework" ]; then
  echo "SKIP mutation-check-auth: 缺 GhosttyKit"; exit 0
fi
if [ ! -x "$SSHD" ] && ! command -v sshd >/dev/null 2>&1; then
  echo "SKIP mutation-check-auth: 本机无 sshd，闸门会 SKIP，变异无法验证"; exit 0
fi
SSHD="$(command -v sshd 2>/dev/null || echo "$SSHD")"

WORK="$(mktemp -d "$TMP/gt-mutauth-XXXXXX")"
SRV=""
cleanup() {
  [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
  rm -rf "$WORK"
}
restore_all() {
  # 只有现场还在变异态才复原：脚本被 kill -9 后残留的 EXIT trap 会拿**旧备份**
  # 把源文件盖回去，把别人的新改动一起抹掉（真踩过）。
  if ! grep -q "$MARK" "$SRC" 2>/dev/null; then return 0; fi
  [ -n "$BACKUP" ] && [ -f "$BACKUP" ] && cp "$BACKUP" "$SRC" && rm -f "$BACKUP"
}
# EXIT 里也要收 sshd：本脚本自己起了服务，正常退出时若不 kill，
# 它会把父进程的 stdout 管道一直撑着（父进程明明跑完了，调用方却永远等不到 EOF）。
restore_exit() { [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null; restore_all; }
trap 'restore_exit' EXIT
trap 'restore_all; echo "已复原（trap）"' INT TERM

if grep -q "$MARK" "$SRC" 2>/dev/null; then
  echo "检测到残留变异标记（${MARK}）于 ${SRC}——拒绝起跑（exit 2）" >&2; exit 2
fi
BASE_SHA="$(shasum -a 256 "$SRC" | awk '{print $1}')"
BACKUP="$(mktemp "$TMP/gt-mutauth-bak-XXXXXX")"; BACKUP="$BACKUP.swift"
cp "$SRC" "$BACKUP"

# ---- 真 sshd（公钥唯一）----
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
ssh-keygen -t ed25519 -f "$WORK/hostkey" -N '' -q </dev/null
ssh-keygen -t ed25519 -f "$WORK/clientkey" -N '' -q </dev/null
cp "$WORK/clientkey.pub" "$WORK/authorized_keys"
{
  echo "Port $PORT"; echo "ListenAddress 127.0.0.1"
  echo "HostKey $WORK/hostkey"; echo "PidFile $WORK/sshd.pid"
  echo "AuthorizedKeysFile $WORK/authorized_keys"
  echo "PubkeyAuthentication yes"; echo "PasswordAuthentication no"
  echo "UsePAM no"; echo "StrictModes no"; echo "LogLevel ERROR"
  # 关掉 per-source 惩罚：本闸门对**同一台** sshd 反复连接（握手探针 +
  # 故意失败的那把钥匙），OpenSSH 10.x 默认会把 127.0.0.1 判成攻击源，
  # 累计到 15s 后**在发 banner 之前**直接丢连接——libssh2 报
  # 「Failed getting banner」，于是基线随机变红（实测复现）。这是测试
  # 环境的人造流量，不是产品行为；关掉它只是让闸门可复现。
  echo "PerSourcePenalties no"
  echo "Subsystem sftp /usr/libexec/sftp-server"
} > "$WORK/sshd_config"
"$SSHD" -f "$WORK/sshd_config" -D -e 2>"$WORK/sshd.log" &
SRV=$!
ready=0
for _ in $(seq 1 50); do
  if python3 -c "import socket,sys;s=socket.socket();s.settimeout(0.2);sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then
    ready=1; break
  fi
  kill -0 "$SRV" 2>/dev/null || break
  sleep 0.1
done
if [ "$ready" != 1 ]; then
  echo "SKIP mutation-check-auth: sshd 未能起在 127.0.0.1:${PORT}"
  sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
  exit 0
fi

run_auth() {
  ( cd "$PKG" && env GHOSTERM_SSH_HOST=127.0.0.1 GHOSTERM_SSH_PORT="$PORT" \
      GHOSTERM_SSH_USER="$(id -un)" GHOSTERM_SSH_KEY="$WORK/clientkey" \
      GHOSTERM_SSH_KNOWN_HOSTS="$WORK/known_hosts" \
      gt_timeout 300 swift test --filter SSHAuthTests >"$WORK/auth-test-${PASS}.log" 2>&1 )
}
run_other() {  # 指向性对照：与认证无关的那批（主机列表解析）
  ( cd "$PKG" && gt_timeout 300 swift test --filter HostListTests >/dev/null 2>&1 )
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
expect green "认证桥基线" run_auth
expect green "对照（主机列表）基线" run_other

echo "== A1：~ 展开丢掉分隔符 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = '        if p.hasPrefix("~/") { return home + p.dropFirst(1) }'
new = '        if p.hasPrefix("~/") { return home + p.dropFirst(2) /* GT_MUTATION_MARK */ }'
assert old in s, "A1 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "A1 展开出 /Users/x.ssh/id_rsa（默认钥匙链全废）" run_auth
expect green "A1 指向性（主机列表不受影响）" run_other
rollback

echo "== A2：没口令的钥匙也弹口令框 =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = "            switch Ghosterm.keyPassphraseRequirement(pem) {"
new = "            switch PrivateKeyPassphraseRequirement.needed { /* GT_MUTATION_MARK */"
assert old in s, "A2 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "A2 没加密的钥匙也被问口令" run_auth
expect green "A2 指向性（主机列表不受影响）" run_other
rollback

echo "== A3：方法表在握手前读（恒为空 → 只会猜口令） =="
python3 - "$SRC" <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
old = """        try ssh.handshake()
        let methods = ssh.authMethods"""
new = """        let methods: SSHAuthMethods = [] /* GT_MUTATION_MARK */
        try ssh.handshake()"""
assert old in s, "A3 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PY
expect red "A3 服务器收密码却去问私钥口令" run_auth
expect green "A3 指向性（主机列表不受影响）" run_other
rollback

echo "== A4：把「已解锁的 PEM」又缓存进 Keychain =="
python3 - "$SRC" <<'PYA4'
import sys
p = sys.argv[1]; s = open(p).read()
old = """                try ssh.authenticate(privateKeyPEM: pem, passphrase: passphrase)
                try ssh.authenticate()
                return ssh"""
new = """                try ssh.authenticate(privateKeyPEM: pem, passphrase: passphrase)
                try ssh.authenticate()
                try? KeychainStore.setPrivateKeyPEM(pem, forPath: path) /* GT_MUTATION_MARK */
                return ssh"""
assert old in s, "A4 锚点缺失"
open(p, 'w').write(s.replace(old, new, 1))
PYA4
expect red "A4 又往 Keychain 里塞已解锁的 PEM" run_auth
rollback

echo "== 回滚复绿 =="
expect green "回滚后认证桥断言复绿" run_auth
expect green "回滚后主机列表断言仍绿" run_other

restore_all
trap - INT TERM
echo
echo "mutation-check-auth: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
