#!/usr/bin/env bash
# SSH 后端 e2e：起一个**本机真 sshd**（非特权用户 + 公钥认证），跑 test_ssh。
#
# 无 sshd 或未编入 libssh2 时**显式 SKIP**（不假装通过）。环回 + 离线，不碰外网。
#
# 用法：ssh_e2e.sh <test_ssh 可执行>
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
TMP="$ROOT/tmp"
mkdir -p "$TMP"
BIN="${1:?需要 test_ssh 可执行}"
SSHD="${SSHD:-/usr/sbin/sshd}"

if [ ! -x "$SSHD" ] && ! command -v sshd >/dev/null 2>&1; then
  echo "SKIP ssh_e2e: 未找到 sshd（${SSHD}）"; exit 0
fi
SSHD="$(command -v sshd 2>/dev/null || echo "$SSHD")"

WORK="$(mktemp -d "$TMP/ssh-e2e-XXXXXX")"
PIDFILE="$WORK/sshd.pid"
cleanup() {
  [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# 一个空闲端口
PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
USERNAME="$(id -un)"

ssh-keygen -t ed25519 -f "$WORK/hostkey" -N '' -q </dev/null
# Independent public hostkey fingerprint oracle for connector trust UI.
GT_TEST_HOSTKEY_SHA256="$(ssh-keygen -lf "$WORK/hostkey.pub" -E sha256 | awk '{print $2}')"
export GT_TEST_HOSTKEY_SHA256
ssh-keygen -t ed25519 -f "$WORK/clientkey" -N '' -q </dev/null
cp "$WORK/clientkey.pub" "$WORK/authorized_keys"
if [ "$(basename "$BIN")" = test_ssh_connector ]; then
  # Ephemeral test material only; never print private bytes or the passphrase.
  export GT_CONNECT_ENCRYPTED_KEY="$WORK/encrypted-clientkey"
  export GT_CONNECT_PASSPHRASE='fixture-口令-for-retry'
  cp "$WORK/clientkey" "$GT_CONNECT_ENCRYPTED_KEY" || exit 1
  ssh-keygen -p -q -f "$GT_CONNECT_ENCRYPTED_KEY" -P '' -N "$GT_CONNECT_PASSPHRASE" >/dev/null || exit 1
fi

{
  echo "Port $PORT"
  echo "ListenAddress 127.0.0.1"
  echo "HostKey $WORK/hostkey"
  echo "PidFile $PIDFILE"
  echo "AuthorizedKeysFile $WORK/authorized_keys"
  echo "MaxStartups 128:30:256"
  echo "PubkeyAuthentication yes"
  echo "PasswordAuthentication no"
  echo "UsePAM no"
  echo "StrictModes no"
  echo "LogLevel ERROR"
  # Repeated deliberate failures are fixture traffic, not a password attack.
  if "$SSHD" -T -h "$WORK/hostkey" -o PerSourcePenalties=no >/dev/null 2>&1; then
    echo "PerSourcePenalties no"
  fi
  # OpenSSH 自带实现，避免把 macOS 的外部程序路径带到 Linux runner。
  echo "Subsystem sftp internal-sftp"
} > "$WORK/sshd_config"

"$SSHD" -f "$WORK/sshd_config" -D -e 2>"$WORK/sshd.log" &
SRV=$!

# 等端口就绪（最多 5s）
ready=0
for _ in $(seq 1 50); do
  if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then
    ready=1; break
  fi
  kill -0 "$SRV" 2>/dev/null || break
  sleep 0.1
done
if [ "$ready" != 1 ]; then
  echo "SKIP ssh_e2e: sshd 未能起在 127.0.0.1:${PORT}（本机环境限制）"
  sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
  exit 0
fi

"$BIN" 127.0.0.1 "$PORT" "$USERNAME" "$WORK/clientkey" "$WORK"
rc=$?
exit $rc
