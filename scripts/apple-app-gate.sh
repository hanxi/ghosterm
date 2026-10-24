#!/usr/bin/env bash
# 闸门：apple/ 宿主（SwiftUI/AppKit）——M1 竖切的顶端口。
#
# 四层证据：
#   0. C ABI 绑定对拍：Apple 引用的 gt_* 必须都在 ghosterm.h 里；未引用的
#      必须显式登记理由（新增遗漏会红，见 check-abi-apple.sh）；
#   1. HostListModel：ssh_config → 主机列表（读视图，不改文件）；
#   2. SessionController + libghostty：本地 PTY / SSH shell 的字节流真的进了
#      终端网格（断言读的是 ghostty_surface_read_text，不是自己记的账）；
#   3. SSH e2e：起本机真 sshd 注入 GHOSTERM_SSH_*（无 sshd 显式 SKIP）。
#
# 前置：scripts/build-ghostty.sh 已产出补丁版 GhosttyKit.xcframework。
# 前置：GhostermApp 的 owned-session fixture 读 build/runtime/catalog.json——
#       它由 scripts/build-sessiond-mac.py + scripts/stage-session-runtime.py
#       现造（CI 在闸门**之前**跑这两步；干净检出上没有这文件，WorkspaceTests
#       会整片报「catalog.json 不存在」，见 macos-package.yml 的装配步骤）。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PKG="$ROOT/apple/GhostermApp"
TMP="$ROOT/tmp"
SSHD="${SSHD:-/usr/sbin/sshd}"
mkdir -p "$TMP"

if ! command -v swift >/dev/null 2>&1; then
  echo "SKIP apple-app-gate: 未找到 swift 工具链"
  exit 0
fi

XCFW="$ROOT/build/GhosttyKit.xcframework/macos-arm64"
if [ ! -d "$XCFW" ]; then
  echo "apple-app-gate: 缺 GhosttyKit.xcframework —— 先跑 scripts/build-ghostty.sh" >&2
  exit 2
fi

echo "== 0/4 C ABI 绑定对拍（Apple 侧引用 ↔ 头文件声明） =="
bash "$ROOT/scripts/check-abi-apple.sh"

echo "== 构建 C 核心（宿主经 GhostermCore 链接它） =="
bash "$ROOT/scripts/build-core.sh" >"$TMP/apple-app-core.log" 2>&1 || {
  tail -20 "$TMP/apple-app-core.log" >&2; exit 1
}
echo "   ok"

WORK=""
cleanup() {
  [ -n "$WORK" ] && [ -f "$WORK/sshd.pid" ] && kill "$(cat "$WORK/sshd.pid")" 2>/dev/null
  [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

ENVARGS=()
if { [ -x "$SSHD" ] || command -v sshd >/dev/null 2>&1; } && command -v ssh-keygen >/dev/null 2>&1; then
  SSHD="$(command -v sshd 2>/dev/null || echo "$SSHD")"
  WORK="$(mktemp -d "$TMP/apple-app-XXXXXX")"
  # sshd 与 AuthorizedKeysFile 都要绝对路径（相对路径按 root 的 cwd 解析）。
  WORK="$(cd "$WORK" && pwd)"
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
    # 关掉 per-source 惩罚（见 mutation-check-auth.sh 的同名说明）：闸门在
    # 同一台 sshd 上反复连接，OpenSSH 10.x 默认会把 127.0.0.1 当攻击源，
    # 累计 15s 后在发 banner 前丢连接 → 「Failed getting banner」随机红。
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
  if [ "$ready" = 1 ]; then
    ENVARGS=(env GHOSTERM_SSH_HOST=127.0.0.1
             GHOSTERM_SSH_PORT="$PORT"
             GHOSTERM_SSH_USER="$(id -un)"
             GHOSTERM_SSH_KEY="$WORK/clientkey"
             GHOSTERM_SSH_KNOWN_HOSTS="$WORK/known_hosts")
    echo "== SSH e2e：真 sshd 已在 127.0.0.1:$PORT =="
  else
    echo "SKIP apple-app-gate e2e: sshd 未能起（本机环境限制）；其余用例仍跑"
    sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
  fi
else
  echo "SKIP apple-app-gate e2e: 未找到 sshd/ssh-keygen"
fi

echo "== 3/3 GhostermApp 构建 + 测试 =="
( cd "$PKG" && "${ENVARGS[@]}" swift test ) >"$TMP/apple-app-test.log" 2>&1 || {
  echo "apple-app-gate: swift test 失败" >&2
  grep -E "error:|Test Case .* failed|panic" "$TMP/apple-app-test.log" >&2 | head -30
  exit 1
}
sed -n 's/^.*Executed \(.*\)/\   执行: \1/p' "$TMP/apple-app-test.log" | tail -1
if grep -q "skipped" "$TMP/apple-app-test.log" && [ "${#ENVARGS[@]}" -eq 0 ]; then
  echo "   （SSH e2e 已 SKIP；本地 PTY + 主机列表用例仍为绿）"
fi

echo "APPLE APP GATE PASS"