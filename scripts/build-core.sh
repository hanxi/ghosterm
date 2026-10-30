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
# 显式保持与 App/Helper 的 macOS 13 下限一致；只钉 SDK 而不钉部署目标会把
# 静态库对象标成 SDK 版本（26.5），Swift 链接时误以为库只支持 macOS 26.5。
[ -n "$SYSROOT" ] && SYSROOT_FLAG=(-DCMAKE_OSX_SYSROOT="$SYSROOT" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0)

python3 "$ROOT/scripts/build-host-ssh2.py"

cmake -S "$ROOT/core" -B "$ROOT/core/build" -DCMAKE_BUILD_TYPE=Debug "${SYSROOT_FLAG[@]}" >/dev/null
cmake --build "$ROOT/core/build"
