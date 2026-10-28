#!/bin/bash
# 用户主动执行的 dev 安装入口；兼容 macOS 自带 Bash 3.2，无需 Homebrew/Python。
# curl -fsSL https://raw.githubusercontent.com/hanxi/ghosterm/main/scripts/install-macos.sh | /bin/bash
set -euo pipefail
APP_DIR=/Applications
while [ "$#" -gt 0 ]; do
  case "$1" in
    --app-dir) [ "$#" -ge 2 ] || { echo '--app-dir 需要目录' >&2; exit 2; }; APP_DIR=$2; shift 2 ;;
    -h|--help) echo '用法：bash install-macos.sh [--app-dir /Applications]'; exit 0 ;;
    *) echo "未知参数：$1" >&2; exit 2 ;;
  esac
done
fail() { echo "安装失败：$*" >&2; exit 1; }
[ "$(uname -s)" = Darwin ] || fail '只支持 macOS'
HOST_ARCH=$(uname -m)
TRANSLATED=$(sysctl -in sysctl.proc_translated 2>/dev/null || true)
case "$HOST_ARCH:$TRANSLATED" in arm64:*|*:1) ;; *) fail '当前包只支持 Apple Silicon（arm64）' ;; esac
for tool in curl plutil shasum ditto codesign xattr awk sw_vers mktemp pgrep; do
  command -v "$tool" >/dev/null 2>&1 || fail "缺少系统工具：${tool}"
done

# 在检出中使用工程 tmp/；curl 管道执行时使用用户缓存，永不使用系统 /tmp。
WORK_ROOT=${GHOSTERM_INSTALL_WORK_DIR:-}
if [ -z "$WORK_ROOT" ] && [ -f "${BASH_SOURCE[0]:-}" ]; then
  PROJECT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
  [ ! -f "$PROJECT/core/include/ghosterm.h" ] || WORK_ROOT="$PROJECT/tmp"
fi
WORK_ROOT=${WORK_ROOT:-${HOME}/Library/Caches/Ghosterm/Install}
mkdir -p "$WORK_ROOT"
WORK=$(mktemp -d "$WORK_ROOT/ghosterm-install-XXXXXXXX")
STAGE=""; TARGET=""; USE_SUDO=0
as_owner() { if [ "$USE_SUDO" -eq 1 ]; then sudo "$@"; else "$@"; fi; }
cleanup() {
  local status=$? keep=0
  trap - EXIT INT TERM
  # 只有本次已保存的旧应用才会恢复，失败时不把唯一的备份删掉。
  if [ "$status" -ne 0 ] && [ -n "$STAGE" ] && [ -e "$STAGE/previous.app" ]; then
    if [ -e "$TARGET" ]; then
      as_owner mv "$TARGET" "$STAGE/failed.app" || keep=1
    fi
    if [ "$keep" -eq 0 ]; then
      as_owner mv "$STAGE/previous.app" "$TARGET" || keep=1
    fi
  fi
  if [ "$keep" -eq 1 ]; then
    echo "旧应用保留在：${STAGE}/previous.app" >&2
  elif [ -n "$STAGE" ]; then
    as_owner rm -rf -- "$STAGE" || status=1
  fi
  rm -rf -- "$WORK" || status=1
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

BASE=https://github.com/hanxi/ghosterm/releases/download/dev
fetch() { curl -fLSs --connect-timeout 15 --max-time 180 --retry 3 "$1" -o "$2"; }
field() { plutil -extract "$1" raw -expect string -o - "$WORK/version.json"; }
version_at_least() {
  awk -v actual="$1" -v minimum="$2" 'BEGIN {
    split(actual,a,"."); split(minimum,b,".");
    for(i=1;i<=3;i++){if(a[i]+0>b[i]+0)exit 0;if(a[i]+0<b[i]+0)exit 1}exit 0
  }'
}
echo '读取 Ghosterm dev 发布清单…'
fetch "$BASE/version.json" "$WORK/version.json" || fail '无法取得发布清单'
[ "$(field channel)" = dev ] || fail '发布渠道不符'
[ "$(field arch)" = arm64 ] || fail '发布架构不支持'
ZIP_NAME=$(field assets.zip)
case "$ZIP_NAME" in Ghosterm-*-arm64.zip) ;; *) fail '发布包名称无效' ;; esac
case "$ZIP_NAME" in *[!A-Za-z0-9._-]*) fail '发布包名称含非法字符' ;; esac
MIN=$(field min_macos)
[[ "$MIN" =~ ^[0-9]+(\.[0-9]+){1,2}$ ]] || fail '最低系统版本无效'
HOST_VERSION=$(sw_vers -productVersion)
version_at_least "$HOST_VERSION" "$MIN" || fail "需要 macOS ${MIN} 或更高版本（当前 ${HOST_VERSION}）"
COMMIT=$(field git_commit)
[[ "$COMMIT" =~ ^[0-9a-f]{40}$ ]] || fail '发布 commit 无效'
fetch "$BASE/SHA256SUMS" "$WORK/SHA256SUMS" || fail '无法取得校验和'
EXPECTED=$(awk -v name="$ZIP_NAME" '$2==name && NF==2 {print $1; n++} END{if(n!=1)exit 1}' "$WORK/SHA256SUMS") || fail '校验和清单缺失或重复'
[[ "$EXPECTED" =~ ^[0-9a-f]{64}$ ]] || fail 'SHA256 格式无效'
fetch "$BASE/$ZIP_NAME" "$WORK/Ghosterm.zip" || fail '下载失败'
ACTUAL=$(shasum -a 256 "$WORK/Ghosterm.zip" | awk '{print $1}')
[ "$ACTUAL" = "$EXPECTED" ] || fail 'SHA256 不匹配；dev 可能正在滚动更新，请稍后重试'
echo 'SHA256 校验通过，检查应用…'
ditto -x -k "$WORK/Ghosterm.zip" "$WORK/unpacked" || fail '解压失败'
CANDIDATE="$WORK/unpacked/Ghosterm.app"
[ -d "$CANDIDATE" ] && [ ! -L "$CANDIDATE" ] || fail '包内没有 Ghosterm.app'
BUNDLE_ID=$(plutil -extract CFBundleIdentifier raw -o - "$CANDIDATE/Contents/Info.plist")
[ "$BUNDLE_ID" = dev.ghosterm.mac ] || fail '应用标识不符'
codesign --verify --strict --deep "$CANDIDATE" || fail '应用签名或资源完整性校验失败'
BUNDLE_COMMIT=$(sed -nE 's/^源码:[[:space:]]*git ([0-9a-f]+)$/\1/p' "$CANDIDATE/Contents/Resources/BUILD-INFO.txt")
[[ "$BUNDLE_COMMIT" =~ ^[0-9a-f]{7,40}$ ]] || fail '包内缺少干净的构建 commit'
case "$COMMIT" in "$BUNDLE_COMMIT"*) ;; *) fail '发布清单与包内 commit 不一致，请稍后重试' ;; esac
if pgrep -x GhostermApp >/dev/null 2>&1; then fail '请先退出 Ghosterm，再运行安装脚本'; fi

case "$APP_DIR" in /*) ;; *) fail '--app-dir 必须是绝对路径' ;; esac
TARGET="${APP_DIR%/}/Ghosterm.app"
[ ! -L "$TARGET" ] || fail '目标是符号链接，请先自行处理'
if [ -e "$TARGET" ]; then
  EXISTING_ID=$(plutil -extract CFBundleIdentifier raw -o - "$TARGET/Contents/Info.plist") || fail '目标不是有效应用'
  [ "$EXISTING_ID" = dev.ghosterm.mac ] || fail '目标位置已有其他应用，拒绝覆盖'
fi
[ ! -e "$APP_DIR" ] || [ -d "$APP_DIR" ] || fail '安装目录不是目录'
if [ ! -d "$APP_DIR" ]; then mkdir -p "$APP_DIR" 2>/dev/null || true; fi
if [ ! -d "$APP_DIR" ] || [ ! -w "$APP_DIR" ] || { [ -e "$TARGET" ] && [ ! -w "$TARGET" ]; }; then
  command -v sudo >/dev/null 2>&1 || fail '目标目录需要管理员权限，可用 --app-dir 指定用户目录'
  echo "安装到 ${APP_DIR} 需要管理员权限。"
  sudo -v || fail '未取得安装权限'
  USE_SUDO=1
fi
as_owner mkdir -p "$APP_DIR"
STAGE=$(as_owner mktemp -d "${APP_DIR%/}/.ghosterm-install-XXXXXXXX")
as_owner chmod 755 "$STAGE"
as_owner ditto "$CANDIDATE" "$STAGE/new.app" || fail '暂存应用失败'
as_owner codesign --verify --strict --deep "$STAGE/new.app" || fail '暂存副本完整性校验失败'
as_owner xattr -dr com.apple.quarantine "$STAGE/new.app" || fail '无法清除 quarantine，旧应用未替换'
if [ -e "$TARGET" ]; then as_owner mv "$TARGET" "$STAGE/previous.app" || fail '无法备份旧应用'; fi
as_owner mv "$STAGE/new.app" "$TARGET" || fail '替换应用失败，正在恢复旧应用'
echo "安装完成：${TARGET}（commit ${BUNDLE_COMMIT}，quarantine 已清除）"
