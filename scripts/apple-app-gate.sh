#!/usr/bin/env bash
# 闸门：apple/ 宿主（SwiftUI/AppKit）——M1 竖切的顶端口。
#
# 四层证据：
#   0. C ABI 绑定对拍：Apple 引用的 gt_* 必须都在 ghosterm.h 里；未引用的
#      必须显式登记理由（新增遗漏会红，见 check-abi-apple.sh）；
#   1. HostListModel：ssh_config → 主机列表（读视图，不改文件）；
#   2. SessionController + libghostty：本地 PTY / SSH shell 的字节流真的进了
#      终端网格（断言读的是 ghostty_surface_read_text，不是自己记的账）；
#   3. SSH e2e：起本机真 sshd 注入 GHOSTERM_SSH_*（required 档缺 sshd 明确失败，optional 才可 SKIP）。
#   4. HTML preview：真实 WebKit custom-scheme CSS / image 像素绘制。
#
# 前置：scripts/build-ghostty.sh 已产出补丁版 GhosttyKit.xcframework。
# 普通 AppModel 夹具只使用本地 PTY / SSH，不需要 sessiond 或 runtime catalog。
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Native mutation runners may select an isolated copy; the default app and
# shared Core/ABI checks still come from this checkout.
PKG="${GHOSTERM_APP_TEST_PACKAGE:-$ROOT/apple/GhostermApp}"
TMP="${GHOSTERM_APP_TEST_TMP:-$ROOT/tmp}"
SSHD="${SSHD:-/usr/sbin/sshd}"
mkdir -p "$TMP"
source "$ROOT/scripts/lib/gt-timeout.sh"
gt_timeout_require apple-app-gate
source "$ROOT/scripts/apple-gate-profile.sh" || exit $?

if ! command -v swift >/dev/null 2>&1; then
  apple_gate_missing "未找到 swift 工具链"
  apple_gate_finish "APPLE APP GATE"
  exit 0
fi

XCFW="$ROOT/build/GhosttyKit.xcframework/macos-arm64"
if [ ! -d "$XCFW" ]; then
  echo "apple-app-gate: 缺 GhosttyKit.xcframework —— 先跑 scripts/build-ghostty.sh" >&2
  exit 2
fi

echo "== 0/4 C ABI 绑定对拍（Apple 侧引用 ↔ 头文件声明） =="
bash "$ROOT/scripts/check-abi-apple.sh" || exit $?

PLUGIN_LUA_PROFILE=0
if [ -n "${GHOSTERM_TEST_GTERM:-}" ]; then
  if [ ! -x "$GHOSTERM_TEST_GTERM" ]; then
    echo "apple-app-gate: GHOSTERM_TEST_GTERM is not executable" >&2; exit 2
  fi
  PLUGIN_LUA_PROFILE=1
fi
# SwiftPM resolves Lua linkage during test discovery and compilation.
export GHOSTERM_CORE_TEST_PLUGIN_LUA="$PLUGIN_LUA_PROFILE"

echo "== 构建 C 核心（宿主经 GhostermCore 链接它） =="
if [ "$PLUGIN_LUA_PROFILE" = 1 ]; then
  GHOSTERM_BUILD_PLUGIN_LUA=1 bash "$ROOT/scripts/build-core.sh" >"$TMP/apple-app-core.log" 2>&1 || {
    tail -20 "$TMP/apple-app-core.log" >&2; exit 1
  }
else
  bash "$ROOT/scripts/build-core.sh" >"$TMP/apple-app-core.log" 2>&1 || {
    tail -20 "$TMP/apple-app-core.log" >&2; exit 1
  }
fi
echo "   ok"

WORK=""
HTML_CAPTURE=""
PROCESS_FIXTURE=""
SRV=""
JUMP_SRV=""
cleanup() {
  local server
  for server in "$JUMP_SRV" "$SRV"; do
    if [ -n "$server" ]; then
      kill "$server" 2>/dev/null || true
      wait "$server" 2>/dev/null || true
    fi
  done
  if [ -n "$PROCESS_FIXTURE" ]; then rm -rf "$PROCESS_FIXTURE"; fi
  [ -n "$WORK" ] && rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# The Lua core profile omits C test targets. Build the standalone child
# used by the Swift local process tests.
if [ "$PLUGIN_LUA_PROFILE" = 1 ] && [ -z "${GHOSTERM_PROCESS_CHANNEL_CHILD:-}" ]; then
  PROCESS_FIXTURE="$(mktemp -d "$TMP/apple-app-child-XXXXXX")"
  PROCESS_FIXTURE="$(cd "$PROCESS_FIXTURE" && pwd)"
  export GHOSTERM_PROCESS_CHANNEL_CHILD="$PROCESS_FIXTURE/process-channel-child"
  xcrun clang -std=c11 -Wall -Wextra -Werror -DGT_PROCESS_CHANNEL_STANDALONE \
    "$ROOT/core/tests/process_channel_child.c" -o "$GHOSTERM_PROCESS_CHANNEL_CHILD"
fi

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
    echo "Subsystem sftp /usr/libexec/sftp-server -d \"$WORK\""
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
    ENVARGS=(GHOSTERM_SSH_HOST=127.0.0.1
             GHOSTERM_SSH_PORT="$PORT"
             GHOSTERM_SSH_USER="$(id -un)"
             GHOSTERM_SSH_KEY="$WORK/clientkey"
             GHOSTERM_TEST_KEYCHAIN_OWNED_REFERENCE="$WORK/clientkey"
             GHOSTERM_SSH_KNOWN_HOSTS="$WORK/known_hosts")
    if [ "${GHOSTERM_TEST_SSH_JUMP:-${GHOSTERM_TEST_CODEX_JUMP:-0}}" = 1 ]; then
      JUMP_PORT="$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')"
      ssh-keygen -t ed25519 -f "$WORK/jump-hostkey" -N '' -q </dev/null
      {
        echo "Port $JUMP_PORT"; echo "ListenAddress 127.0.0.1"
        echo "HostKey $WORK/jump-hostkey"; echo "PidFile $WORK/jump.pid"
        echo "AuthorizedKeysFile $WORK/authorized_keys"
        echo "PubkeyAuthentication yes"; echo "PasswordAuthentication no"
        echo "KbdInteractiveAuthentication no"; echo "UsePAM no"; echo "StrictModes no"
        echo "PerSourcePenalties no"; echo "LogLevel ERROR"; echo "PermitUserRC no"
        echo "AllowAgentForwarding no"; echo "AllowStreamLocalForwarding no"
        echo "AllowTcpForwarding local"; echo "PermitOpen 127.0.0.1:$PORT"
        echo "MaxSessions 0"
      } > "$WORK/jump-config"
      "$SSHD" -f "$WORK/jump-config" -D -e 2>"$WORK/jump.log" &
      JUMP_SRV=$!
      jump_ready=0
      for _ in $(seq 1 50); do
        if python3 -c "import socket,sys;s=socket.socket();s.settimeout(0.2);sys.exit(0 if s.connect_ex(('127.0.0.1',$JUMP_PORT))==0 else 1)"; then
          jump_ready=1; break
        fi
        kill -0 "$JUMP_SRV" 2>/dev/null || break
        sleep 0.1
      done
      if [ "$jump_ready" != 1 ]; then
        echo "apple-app-gate: requested jump fixture failed" >&2
        cat "$WORK/jump.log" >&2
        exit 1
      fi
      ENVARGS+=(GHOSTERM_SSH_JUMP_HOST=127.0.0.1 GHOSTERM_SSH_JUMP_PORT="$JUMP_PORT")
      echo "== Owned SSH jump fixture: 127.0.0.1:${JUMP_PORT} -> 127.0.0.1:${PORT} =="
    fi
    if [ "$PLUGIN_LUA_PROFILE" = 1 ]; then
      ENVARGS+=(GHOSTERM_CORE_TEST_PLUGIN_LUA=1)
    fi
    if [ -n "${GHOSTERM_TEST_GTERM:-}" ]; then
      ENVARGS+=(GHOSTERM_TEST_GTERM="$GHOSTERM_TEST_GTERM")
    fi
    if [ -n "${GHOSTERM_TEST_GTERM:-}" ] &&
       { [ -z "${GHOSTERM_APP_TEST_FILTER:-}" ] ||
         [[ "$GHOSTERM_APP_TEST_FILTER" == *testAppModelReceivesScopedResourceFromRemoteGterm* ||
            "$GHOSTERM_APP_TEST_FILTER" == *PluginPaneFlowTests* ]]; }; then
      PREVIEW_PACKAGE="$WORK/preview-package"
      python3 -B "$ROOT/scripts/gterm-plugin" build "$ROOT/plugins/official/preview" --out "$PREVIEW_PACKAGE" || exit $?
      ENVARGS+=(GHOSTERM_TEST_LUA_PREVIEW_PACKAGE="$PREVIEW_PACKAGE")
      if [ -z "${GHOSTERM_APP_TEST_FILTER:-}" ] ||
         [[ "$GHOSTERM_APP_TEST_FILTER" == *testAppModelReceivesScopedResourceFromRemoteGterm* ]]; then
        HTML_CAPTURE="$WORK/html-preview-capture"
        ENVARGS+=(GHOSTERM_HTML_PROBE_CAPTURE="$HTML_CAPTURE")
      fi
    fi
    echo "== SSH e2e：真 sshd 已在 127.0.0.1:$PORT =="
  else
    sed 's/^/  sshd: /' "$WORK/sshd.log" 2>/dev/null
    apple_gate_missing "sshd 未能启动；SSH e2e 未验证"
  fi
else
  apple_gate_missing "未找到 sshd/ssh-keygen"
fi

echo "== 3/3 GhostermApp 构建 + 测试 =="
# Run each test class in a fresh serial xctest process. A single full-package
# process can retain AppKit/WebKit state after all assertions pass and never
# exit, while `--parallel` lets shared UserDefaults fixtures race each other.
# Bash 3.2 treats an empty array as unset under nounset, so expand optional
# argv only when populated and preserve each environment assignment as one word.
(
  cd "$PKG"
  : >"$TMP/apple-app-test.log"
  SWIFT_FLAGS=()
  if [ "$PKG" != "$ROOT/apple/GhostermApp" ]; then
    SWIFT_FLAGS=(--cache-path "$TMP/swift-cache" --disable-keychain --disable-netrc)
  fi
  SUITES=()
  if ! DISCOVERED_TESTS=$(gt_timeout 300 swift test ${SWIFT_FLAGS[@]+"${SWIFT_FLAGS[@]}"} --list-tests 2>>"$TMP/apple-app-test.log"); then
    echo "apple-app-gate: Swift 测试发现失败" >&2
    exit 1
  fi
  while IFS= read -r suite; do
    if [ -n "${GHOSTERM_APP_TEST_FILTER:-}" ]; then
      if [[ "$suite" =~ $GHOSTERM_APP_TEST_FILTER || "${suite/\//.}" =~ $GHOSTERM_APP_TEST_FILTER ]]; then
        SUITES+=("$suite")
      fi
    elif [ -n "$suite" ]; then
      SUITES+=("$suite")
    fi
  done < <(if [ -n "${GHOSTERM_APP_TEST_FILTER:-}" ]; then
      printf '%s\n' "$DISCOVERED_TESTS" | sed -nE 's#^([A-Za-z0-9_]+\.[A-Za-z0-9_]+/[A-Za-z0-9_]+)$#\1#p'
    else
      printf '%s\n' "$DISCOVERED_TESTS" | sed -nE 's#^([A-Za-z0-9_]+\.[A-Za-z0-9_]+)/.*#\1#p' | sort -u
    fi)
  [ "${#SUITES[@]}" -gt 0 ] || { echo "apple-app-gate: 未发现 Swift 测试套件" >&2; exit 1; }
  swift build ${SWIFT_FLAGS[@]+"${SWIFT_FLAGS[@]}"} --build-tests --enable-xctest --enable-swift-testing >/dev/null
  TEST_BUNDLE="$(find .build -name '*.xctest' -print -quit)"
  [ -d "$TEST_BUNDLE" ] || { echo "apple-app-gate: 测试包不存在: $TEST_BUNDLE" >&2; exit 1; }
  # AppKit view tests can inherit a stale SwiftPM/XCTest process state after
  # the conversation model suite on the hosted runner. Run that independent
  # UI suite first while retaining every suite in the serial pass.
  for index in "${!SUITES[@]}"; do
    if [ "${SUITES[$index]}" = "GhostermAppTests.ClaudeConversationViewTests" ]; then
      first="${SUITES[0]}"; SUITES[0]="${SUITES[$index]}"; SUITES[$index]="$first"; break
    fi
  done
  for suite in "${SUITES[@]}"; do
    printf '== Swift suite: %s ==\n' "$suite" | tee -a "$TMP/apple-app-test.log" || exit $?
    # Discard inherited endpoints. Tests that need SSH or a plugin runtime
    # receive only this invocation's fixture settings.
    gt_timeout 1800 env \
      -u GHOSTERM_SSH_HOST -u GHOSTERM_SSH_PORT -u GHOSTERM_SSH_USER \
      -u GHOSTERM_SSH_KEY -u GHOSTERM_SSH_KNOWN_HOSTS \
      -u GHOSTERM_SSH_JUMP_HOST -u GHOSTERM_SSH_JUMP_PORT \
      -u GHOSTERM_TEST_GTERM -u GHOSTERM_CORE_TEST_PLUGIN_LUA \
      -u GHOSTERM_TEST_KEYCHAIN_OWNED_REFERENCE -u GHOSTERM_HTML_PROBE_CAPTURE \
      -u GHOSTERM_TEST_LUA_PREVIEW_PACKAGE \
      ${ENVARGS[@]+"${ENVARGS[@]}"} \
      GHOSTERM_AUTOMATION_OFFSCREEN=1 xcrun xctest -XCTest "$suite" "$TEST_BUNDLE" \
      >>"$TMP/apple-app-test.log" 2>&1 || {
        status=$?
        echo "apple-app-gate: XCTest $suite 退出码 $status" >&2
        tail -12 "$TMP/apple-app-test.log" >&2
        exit "$status"
      }
  done
) || {
  echo "apple-app-gate: swift test 失败" >&2
  grep -E "(^|: )error:|Test Case .* failed|Fatal error:|unexpected signal|panic" "$TMP/apple-app-test.log" >&2 | head -30
  exit 1
}
apple_gate_log "$TMP/apple-app-test.log" \
  SessionSmokeTests.testLocalShellReachesGrid \
  SessionSmokeTests.testSSHShellReachesGrid \
  SFTPBrowserTests.testDualPaneBrowseAndDownload
sed -n 's/^.*Executed \(.*\)/\   执行: \1/p' "$TMP/apple-app-test.log" | tail -1
if grep -q "skipped" "$TMP/apple-app-test.log" && [ "${#ENVARGS[@]}" -eq 0 ]; then
  echo "   （SSH e2e 已 SKIP；本地 PTY + 主机列表用例仍为绿）"
fi

echo "== 4/4 受限 HTML Preview：真实 WebKit 子资源绘制 =="
if [ -n "$HTML_CAPTURE" ]; then
  for relative in site/web/index.html site/web/style.css site/web/image.png; do
    if [ ! -s "$HTML_CAPTURE/$relative" ]; then
      echo "apple-app-gate: AppModel HTML receipt capture is missing $relative" >&2
      exit 1
    fi
  done
  GHOSTERM_HTML_PROBE_CAPTURE="$HTML_CAPTURE" bash "$ROOT/scripts/test-plugin-html-webkit.sh" || exit $?
elif [ -z "${GHOSTERM_APP_TEST_FILTER:-}" ]; then
  bash "$ROOT/scripts/test-plugin-html-webkit.sh" || exit $?
fi

apple_gate_finish "APPLE APP GATE"
