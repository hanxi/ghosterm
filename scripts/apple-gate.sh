#!/usr/bin/env bash
# Apple 平台闸门：构建 core + 跑 GhostermCore Swift 绑定测试（离线优先）。
#
# 覆盖三层证据：
#   1. SPM 能编译 Cghosterm ↔ core/include/ghosterm.h（头改了这里编译不过
#      = 绑定未同步，等价于 C ABI 对拍）；
#   2. Swift 层穿透断言（配置往返逐字节相同 / 稀疏编辑 / 本地 PTY / 三态）；
#   3. SSH e2e：起**本机真 sshd**（非特权 + 公钥）注入 GHOSTERM_SSH_*，
#      证明 Swift 绑定 → C 核心 → libssh2 → 真 sshd 全链路；无 sshd/Swift
#      required 缺工具链/服务即失败；optional 可跳过但只报 PARTIAL。
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
source "$ROOT/scripts/apple-gate-profile.sh" || exit $?
if [ "$E2E" = 0 ] && [ "$APPLE_GATE_MODE" = required ]; then
  echo "apple-gate: required mode forbids --no-e2e" >&2; exit 2
fi

if ! command -v swift >/dev/null 2>&1; then
  apple_gate_missing "未找到 swift 工具链"
  apple_gate_finish "APPLE GATE"
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
apple_gate_log "$TMP/apple-swift-test.log"
if grep -q "test skipped\|skipped" "$TMP/apple-swift-test.log"; then
  echo "   离线用例全绿（含 SKIP，见下）"
else
  echo "   ok"
fi
sed -n 's/^.*Executed \(.*\)/\   执行: \1/p' "$TMP/apple-swift-test.log" | tail -1

if [ "$E2E" -ne 1 ]; then
  echo "apple-gate: --no-e2e，跳过 SSH e2e"
  APPLE_GATE_PARTIAL=1
  apple_gate_finish "APPLE GATE"
  exit 0
fi

echo "== 3/3 SSH e2e（本机真 sshd + Swift 绑定） =="
if [ ! -x "$SSHD" ] && ! command -v sshd >/dev/null 2>&1; then
  apple_gate_missing "未找到 sshd（${SSHD}）"
  apple_gate_finish "APPLE GATE"
  exit 0
fi
SSHD="$(command -v sshd 2>/dev/null || echo "$SSHD")"

WORK="$(mktemp -d "$TMP/apple-e2e-XXXXXX")"
PIDFILE="$WORK/sshd.pid"
SRV=""
cleanup() {
  if [ -n "$SRV" ]; then
    kill "$SRV" 2>/dev/null || true
    wait "$SRV" 2>/dev/null || true
  fi
  rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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
  sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
  apple_gate_missing "sshd 未能起在 127.0.0.1:${PORT}"
  apple_gate_finish "APPLE GATE"
  exit 0
fi

# Required reruns the complete binding suite with owned SSH credentials; the
# optional focused pass remains available for local development.
SWIFT_E2E_ARGS=()
if [ "$APPLE_GATE_MODE" = optional ]; then
  SWIFT_E2E_ARGS=(--filter 'GhostermCoreTests.testS.*EndToEnd')
fi
( cd "$PKG" && env GHOSTERM_SSH_HOST=127.0.0.1 \
    GHOSTERM_SSH_PORT="$PORT" \
    GHOSTERM_SSH_USER="$USERNAME" \
    GHOSTERM_SSH_KEY="$WORK/clientkey" \
    GHOSTERM_SSH_KNOWN_HOSTS="$WORK/known_hosts" \
    swift test ${SWIFT_E2E_ARGS[@]+"${SWIFT_E2E_ARGS[@]}"} ) \
    >"$TMP/apple-e2e.log" 2>&1
rc=$?
if [ $rc -ne 0 ]; then
  echo "apple-gate: SSH e2e 未通过" >&2
  cat "$TMP/apple-e2e.log" >&2
  exit 1
fi
apple_gate_log "$TMP/apple-e2e.log" \
  GhostermCoreTests.testSSHEndToEnd \
  GhostermCoreTests.testSSHControlledEndToEnd \
  GhostermCoreTests.testSSHAbortStopsOwnedSFTP \
  GhostermCoreTests.testSFTPEndToEnd \
  GhostermCoreTests.testSFTPDirectoryCreationEndToEnd \
  GhostermCoreTests.testSFTPRemovalEndToEnd \
  GhostermCoreTests.testSFTPPublishEndToEnd \
  GhostermCoreTests.testSFTPReceiveEndToEnd \
  GhostermCoreTests.testSFTPExclusiveCreateEndToEnd \
  GhostermCoreTests.testSFTPPreviewEndToEnd \
  GhostermCoreTests.testSFTPBrowseEndToEnd \
  GhostermCoreTests.testSFTPControlledEndToEnd
if grep -q "skipped" "$TMP/apple-e2e.log"; then
  echo "   条件用例跳过仍见日志；required SSH 收据已单独核对"
else
  echo "   ok（真 sshd：认证 + shell 回显 + resize 穿透 Swift 绑定）"
fi

apple_gate_finish "APPLE GATE"