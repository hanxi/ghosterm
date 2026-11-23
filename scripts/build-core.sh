#!/usr/bin/env bash
# 只构建 C 核心（快速路径，不跑对拍）。完整闸门见 scripts/gate.sh。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# 把 SDK 钉到 Xcode 自带的那份，而不是 CommandLineTools 的。
# 必要性：CMake 探测到 /usr/bin/cc + /usr/bin/ld（CommandLineTools 的 shim），
# 这一套在没有 -isysroot 时默认落在 CommandLineTools 的 MacOSX27.0.sdk；
# 该 SDK 的 libSystem.B.tbd 用了 `arm64e.x1-macos` 这类新架构串，而经典 ld/tapi
# 解析不了 → 链接报 `tapi error: malformed file ... unknown architecture`
# （实测挂在拉了 -lssh2 的 test_config 上：它逼着 tapi 去解析 libSystem 的 TBD）。
# xcrun --sdk macosx 给的是 Xcode 的 26.5 SDK，经典 ld 能正常解析。
# SwiftPM 走 xcrun 已是 26.5，所以只有 CMake 这条路径要钉。
SYSROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
SYSROOT_FLAG=()
CORE_PLUGIN_LUA_FLAG=(-DGHOSTERM_WITH_PLUGIN_LUA=OFF)
CORE_BUILD_DIR="${GHOSTERM_CORE_BUILD_DIR:-$ROOT/core/build}"
case "${GHOSTERM_BUILD_PLUGIN_LUA:-0}" in
  0) ;;
  1)
    [ -n "${GHOSTERM_LUA_ARCHIVE:-}" ] || { echo "GHOSTERM_LUA_ARCHIVE is required" >&2; exit 2; }
    CORE_PLUGIN_LUA_FLAG=(-DGHOSTERM_WITH_PLUGIN_LUA=ON -DGHOSTERM_BUILD_TESTS=OFF "-DGHOSTERM_LUA_ARCHIVE=$GHOSTERM_LUA_ARCHIVE")
    ;;
  *) echo "GHOSTERM_BUILD_PLUGIN_LUA must be 0 or 1" >&2; exit 2 ;;
esac
# 显式保持与 App/Helper 的 macOS 13 下限一致；只钉 SDK 而不钉部署目标会把
# 静态库对象标成 SDK 版本（26.5），Swift 链接时误以为库只支持 macOS 26.5。
[ -n "$SYSROOT" ] && SYSROOT_FLAG=(-DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0)

python3 "$ROOT/scripts/build-host-ssh2.py"

# 签名实验用独立 CMake 构建目录，避免普通 Swift 链接继承缓存中的 libcrypto。
cmake -S "$ROOT/core" -B "$CORE_BUILD_DIR" -DCMAKE_BUILD_TYPE=Debug -DGHOSTERM_WITH_RELEASE_SIGNATURE=OFF "${SYSROOT_FLAG[@]}" "${CORE_PLUGIN_LUA_FLAG[@]}" >/dev/null
cmake --build "$CORE_BUILD_DIR"
