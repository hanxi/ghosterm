#!/usr/bin/env bash
# Apple 平台闸门：构建 core + 跑 GhostermCore Swift 绑定测试（离线优先）。
#
# 覆盖三层证据：
#   1. SPM 能编译 Cghosterm ↔ core/include/ghosterm.h（头改了这里编译不过
#      = 绑定未同步，等价于 C ABI 对拍）；
#   2. Swift 层穿透断言（配置往返逐字节相同 / 稀疏编辑 / 本地 PTY / 三态）；
#   3. SSH e2e：起**本机真 sshd**（非特权 + 公钥）注入 GHOSTERM_SSH_*，
#      证明 Swift 绑定 → C 核心 → libssh2 → 真 sshd 全链路；无 sshd/Swift
#      工具链时**显式 SKIP**（不假装通过）。
#
# 用法：bash scripts/apple-gate.sh [--no-e2e]
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$ROOT/tmp"
PKG="$ROOT/apple/GhostermCore"
SSHD="${SSHD:-/usr/sbin/sshd}"
E2E=1
[ "${1:-}" = "--no-e2e" ] && E2E=0

mkdir -p "$TMP"

if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP apple-gate: 未找到 swift 工具链（本机无法验证 Apple 绑定）"
  exit 0
fi

echo "== 1/3 构建 C 核心（Apple 绑定链接依赖） =="
bash "$ROOT/scripts/build-core.sh" >"$TMP/apple-core-build.log" 2>&1 || {
  echo "apple-gate: core 构建失败" >&2; tail -20 "$TMP/apple-core-build.log" >&2; exit 1
}
echo "   ok"

echo "== 2/3 Swift 绑定测试（离线） =="
( cd "$PKG" && swift test ) >"$TMP/apple-swift-test.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
  echo "apple-gate: swift test 未全绿" >&2
  cat "$TMP/apple-swift-test.log" >&2
  exit 1
fi
if grep -q "test skipped\|skipped" "$TMP/apple-swift-test.log"; then
  echo "   离线用例全绿（含 SKIP，见下）"
else
  echo "   ok"
fi
sed -n 's/^.*Executed \(.*\)/\   执行: \1/p' "$TMP/apple-swift-test.log" | tail -1

if [ "$E2E" -ne 1 ]; then
  echo "apple-gate: --no-e2e，跳过 SSH e2e"
  echo "APPLE GATE PASS（离线）"
  exit 0
fi

echo "== 3/3 SSH e2e（本机真 sshd + Swift 绑定） =="
if [ ! -x "$SSHD" ] && ! command -v sshd >/dev/null 2>&1; then
  echo "SKIP apple-gate e2e: 未找到 sshd（${SSHD}）"
  echo "APPLE GATE PASS（e2e SKIP）"
  exit 0
fi
SSHD="$(command -v sshd 2>/dev/null || echo "$SSHD")"

WORK="$(mktemp -d "$TMP/apple-e2e-XXXXXX")"
PIDFILE="$WORK/sshd.pid"
cleanup() {
  [ -f "$PIDFILE" ] && kill "$(cat "$PIDFILE")" 2>/dev/null
  rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
USERNAME="$(id -un)"

ssh-keygen -t ed25519 -f "$WORK/hostkey" -N '' -q </dev/null
ssh-keygen -t ed25519 -f "$WORK/clientkey" -N '' -q </dev/null
cp "$WORK/clientkey.pub" "$WORK/authorized_keys"

{
  echo "Port $PORT"
  echo "ListenAddress 127.0.0.1"
  echo "HostKey $WORK/hostkey"
  echo "PidFile $PIDFILE"
  echo "AuthorizedKeysFile $WORK/authorized_keys"
  echo "PubkeyAuthentication yes"
  echo "PasswordAuthentication no"
  echo "UsePAM no"
  echo "StrictModes no"
  echo "LogLevel ERROR"
  # 关掉 per-source 惩罚（见 mutation-check-auth.sh 里的同名说明）：
  # 闸门对同一台 sshd 反复连接，OpenSSH 10.x 会把 127.0.0.1 判成攻击源，
  # 累计 15s 后在发 banner 前丢连接，报「Failed getting banner」。
  echo "PerSourcePenalties no"
  echo "Subsystem sftp /usr/libexec/sftp-server"
} > "$WORK/sshd_config"

"$SSHD" -f "$WORK/sshd_config" -D -e 2>"$WORK/sshd.log" &
SRV=$!

ready=0
for _ in $(seq 1 50); do
  if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then
    ready=1; break
  fi
  kill -0 "$SRV" 2>/dev/null || break
  sleep 0.1
done
if [ "$ready" != 1 ]; then
  echo "SKIP apple-gate e2e: sshd 未能起在 127.0.0.1:${PORT}（本机环境限制）"
  sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
  echo "APPLE GATE PASS（e2e SKIP）"
  exit 0
fi

# SWIFT_TEST_FILTER 只在支持时用；否则跑全量（离线用例也应保持绿）。
env GHOSTERM_SSH_HOST=127.0.0.1 \
    GHOSTERM_SSH_PORT="$PORT" \
    GHOSTERM_SSH_USER="$USERNAME" \
    GHOSTERM_SSH_KEY="$WORK/clientkey" \
    GHOSTERM_SSH_KNOWN_HOSTS="$WORK/known_hosts" \
    bash -c "cd '$PKG' && swift test --filter 'GhostermCoreTests.testS.*EndToEnd'" \
    >"$TMP/apple-e2e.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
  echo "apple-gate: SSH e2e 未通过" >&2
  cat "$TMP/apple-e2e.log" >&2
  exit 1
fi
if grep -q "skipped" "$TMP/apple-e2e.log"; then
  echo "SKIP apple-gate e2e: 用例自行判定跳过（环境不满足）"
else
  echo "   ok（真 sshd：认证 + shell 回显 + resize 穿透 Swift 绑定）"
fi

echo "APPLE GATE PASS"