#!/usr/bin/env bash
# known_hosts 的**算法覆盖**对拍：起一个只发 ECDSA 主机密钥的 sshd。
#
# 为什么要有它（真缺陷）：读侧早先按 `ssh` 前缀过滤公钥条目——那是照抄
# OpenSSH 对 ssh-rsa 的写法，而 ECDSA 的 key type 是
# `ecdsa-sha2-nistp256`，根本不以 ssh 开头，于是 ECDSA 条目被整条跳过。
# 真机的 SSH 服务器常给 ECDSA，症状就是「点过信任并连接，下次还问」。
# 常规 e2e 用的是 ed25519，永远盖不到这条路径——所以必须专起一个
# ECDSA-only 的 sshd。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TMP="$ROOT/tmp"; mkdir -p "$TMP"
BIN="${1:?}"
WORK="$(mktemp -d "$TMP/ecdsa-e2e-XXXXXX")"
cleanup() { [ -f "$WORK/sshd.pid" ] && kill "$(cat "$WORK/sshd.pid")" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT INT TERM
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
ssh-keygen -t ecdsa -b 256 -f "$WORK/hostkey_ecdsa" -N '' -q </dev/null
ssh-keygen -t ed25519 -f "$WORK/clientkey" -N '' -q </dev/null
cp "$WORK/clientkey.pub" "$WORK/authorized_keys"
{
  echo "Port $PORT"; echo "ListenAddress 127.0.0.1"
  echo "HostKey $WORK/hostkey_ecdsa"   # 只给 ecdsa
  echo "PidFile $WORK/sshd.pid"
  echo "AuthorizedKeysFile $WORK/authorized_keys"
  echo "PubkeyAuthentication yes"; echo "PasswordAuthentication no"
  echo "UsePAM no"; echo "StrictModes no"; echo "LogLevel ERROR"
} > "$WORK/sshd_config"
/usr/sbin/sshd -f "$WORK/sshd_config" -D -e 2>"$WORK/sshd.log" &
for _ in $(seq 1 50); do
  python3 -c "import socket,sys;s=socket.socket();s.settimeout(0.2);sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)" && break
  sleep 0.1
done
if [ ! -f "$WORK/sshd.pid" ]; then
  echo "SKIP ecdsa_e2e: sshd 未能起在 127.0.0.1:${PORT}（本机环境限制）"
  sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
  exit 0
fi
echo "== ECDSA-only sshd 在 127.0.0.1:$PORT =="
"$BIN" 127.0.0.1 "$PORT" "$(id -un)" "$WORK/clientkey" "$WORK"
