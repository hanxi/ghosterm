#!/usr/bin/env bash
# 出「本机可跑」的 .app（开发/内测快速路径）。
#
# 这只是 scripts/package-mac.sh 的薄壳：**打包逻辑只有一份**，避免「快速脚本」
# 与「发行脚本」各写一套 install_name / 签名而悄悄分叉（历史上就是这么分叉
# 出了「libssh2 拷进来了、它依赖的 OpenSSL 没拷」的假自包含）。
#
# 要 dmg / zip / 校验和 / 公证，直接用：
#   bash scripts/package-mac.sh [--version X.Y.Z] [--sign "Developer ID Application: ..."]
#
# 用法：bash scripts/build-mac-app.sh [额外参数…]
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
exec bash "$ROOT/scripts/package-mac.sh" --out "$ROOT/tmp" --app-only --no-zip "$@"
