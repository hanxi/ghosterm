#!/usr/bin/env bash
# 打 macOS 发行包：Ghosterm.app + .dmg（+ .zip / SHA256SUMS / BUILD-INFO.txt）。
#
# 本地与 CI 共用同一份脚本（.github/workflows/macos-release.yml 直接调它），
# 避免「CI 出的包」和「本地出的包」是两套逻辑。
#
# 用法：
#   bash scripts/package-mac.sh                          # 出 dist/Ghosterm.app + dmg
#   bash scripts/package-mac.sh --version 0.2.0          # 指定版本号
#   bash scripts/package-mac.sh --app-only --out tmp     # 只要 .app（等价 build-mac-app.sh）
#   bash scripts/package-mac.sh --sign "Developer ID Application: X (TEAM)" --notarize ghosterm
#
# 选项：
#   --version V        版本号（默认 git describe --tags --always，无 tag 则 0.1.0-dev）
#   --build N          构建号（默认 git rev-list --count HEAD）
#   --out DIR          产物目录（默认 <repo>/dist）
#   --arch ARCH        目标架构（默认本机；目前只支持 arm64——GhosttyKit 只有 arm64 切片）
#   --app-only         只出 .app（不出 dmg/zip）
#   --no-zip           不出 .zip
#   --sign ID|adhoc|none   签名（默认 adhoc；ID 必须是 Developer ID Application 或
#                      Apple Development 两类之一，按证书**真实的类**记成 id/dev 两档）
#   --notarize PROFILE     公证（notarytool 的 keychain profile；只有 id 档能公证）
#   --min-os auto|X.Y  最低系统（默认 auto = 取实际二进制与内嵌库的最大 minos）
#   --force-ghostty    强制重建 libghostty xcframework（默认有就复用）
#   --keep-work        保留中间目录（默认清理 tmp/ 下的工作目录）
#   --gui-smoke        出包后真的 open 一次并确认进程活着（需要 GUI 会话；
#                      会在屏幕上弹一个窗口，故默认关闭）
#
# 诚实口径（AGENTS.md 铁律 #6，不静默降级）：
#   · 缺 libghostty 产物且缺 zig/Metal 工具链 → 明确失败并说清缺什么；
#   · 内嵌依赖没做到自包含 → 失败（不是警告）；
#   · 依赖的 minos 高于声明的 LSMinimumSystemVersion → 抬到真实值并**打印原因**，
#     绝不写一个自己都跑不起来的最低系统版本；
#   · 代码签名：默认 ad-hoc（本机/内测可跑），真分发需 Developer ID + 公证；
#     没证书时**显式说明**产物未公证，不假装可分发给终端用户；
#   · 签名档位按证书**真实的类**记账：`Developer ID Application:` → `id`（可公证），
#     `Apple Development:` → `dev`（免费个人账号，不能公证），其余身份直接拒。
# 已知坑：本脚本大量用中文提示。**`$VAR` 后面紧跟中文字符时，bash 会把非 ASCII
# 字节并进变量名**（表现为 `unbound variable`）。所以脚本里一律写 `${VAR}`，
# 避免变量名后直接紧跟全角括号。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

# 把 SDK 钉到 Xcode 自带的那份（26.5），而不是 CommandLineTools 的 27.0。
# 必要性：本机 /usr/bin/cc + /usr/bin/ld（CommandLineTools 的 shim）在没有 -isysroot
# 时默认落在 CommandLineTools 的 MacOSX27.0.sdk，其 libSystem.B.tbd 用了
# `arm64e.x1-macos` 这类新架构串，经典 ld/tapi 解析不了 → 链接报
# `tapi error: malformed file ... unknown architecture`（实测挂在拉了 -lssh2 的
# test_config 上）。export 出去后，build-core.sh 与 swift build 都继承这份 SDKROOT，
# 一处修齐整条打包链。Swift 本来就经 xcrun 落 26.5，所以只有 CMake 这条路径受影响。
# 见 scripts/build-core.sh 的等价注释（独立运行也钉）。
SYSROOT="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
[ -n "$SYSROOT" ] && export SDKROOT="$SYSROOT"

VERSION=""
BUILD_NUMBER=""
OUT="$ROOT/dist"
ARCH="$(uname -m)"
APP_ONLY=0
MAKE_ZIP=1
SIGN_MODE="adhoc"
SIGN_ID=""
NOTARIZE_PROFILE=""
MIN_OS="auto"
FORCE_GHOSTTY=0
KEEP_WORK=0
GUI_SMOKE=0

while [ $# -gt 0 ]; do
  case "$1" in
    --version) VERSION="${2:?--version 需要值}"; shift 2 ;;
    --build) BUILD_NUMBER="${2:?--build 需要值}"; shift 2 ;;
    --out) OUT="${2:?--out 需要目录}"; shift 2 ;;
    --arch) ARCH="${2:?--arch 需要值}"; shift 2 ;;
    --app-only) APP_ONLY=1; shift ;;
    --no-zip) MAKE_ZIP=0; shift ;;
    --sign) SIGN_MODE="id"; SIGN_ID="${2:?--sign 需要身份}"; shift 2 ;;
    --adhoc) SIGN_MODE="adhoc"; shift ;;
    --no-sign) SIGN_MODE="none"; shift ;;
    --notarize) NOTARIZE_PROFILE="${2:?--notarize 需要 keychain profile 名}"; shift 2 ;;
    --min-os) MIN_OS="${2:?--min-os 需要值}"; shift 2 ;;
    --force-ghostty) FORCE_GHOSTTY=1; shift ;;
    --keep-work) KEEP_WORK=1; shift ;;
    --gui-smoke) GUI_SMOKE=1; shift ;;
    -h|--help) sed -n '2,37p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "未知参数：$1（--help 看用法）" >&2; exit 2 ;;
  esac
done

fail() { echo "package-mac FAIL: $*" >&2; exit 1; }
if [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then
  fail "需要 bash ≥ 4（macOS 自带 /bin/bash 是 3.2）。brew install bash 后：bash scripts/package-mac.sh …"
fi
step() { echo "== $* =="; }
note() { echo "   $*"; }

# 签名档位要按**证书本身的类**记账，不能拿「钥匙串里有这串名字」当「有 Developer ID」。
# 免费个人账号签出来的是 `Apple Development: …`：不能公证（Apple 侧也拒），Gatekeeper
# 照样拦，与 ad-hoc 的差别只在签名身份稳定（覆盖安装不再因每轮 cdhash 变化被判成
# 不同来源）。把它记成 `id`，随包 BUILD-INFO 与 version.json 的 `signature` 就成了
# 谎话——而那个字段正是 app 侧判断「能不能自动装」的依据（`RemoteBuild.installBlocked`）。
# 分类放在起跑而不是第 7 步：拿错身份要立刻红，不该先烧掉十几分钟构建才发现。
if [ "$SIGN_MODE" = "id" ]; then
  [ -n "$SIGN_ID" ] || fail "--sign 需要身份字符串"
  case "$SIGN_ID" in
    "Developer ID Application: "*) SIGN_MODE="id" ;;
    "Apple Development: "*)        SIGN_MODE="dev" ;;
    *) fail "签名身份「${SIGN_ID}」不在可记账的两档内：只接受「Developer ID Application: …」（付费计划，可公证）或「Apple Development: …」（免费个人账号，不可公证）。其余身份（Enterprise / Mac Installer / 自签）一律拒——宁可红，不拿别的证书冒充 Developer ID。" ;;
  esac
  # 记账要在任何拒绝之前打印：档位由证书的类决定，身份在不在钥匙串里是另一件事
  # ——混着报错就分不出「分类器判错档」和「这台机器没这张证书」。
  note "签名档：${SIGN_MODE}（身份 ${SIGN_ID}）"
  if [ "$SIGN_MODE" = "dev" ] && [ -n "$NOTARIZE_PROFILE" ]; then
    fail "--notarize 要求 Developer ID（付费计划）签名——当前签名档是 dev（Apple Development，免费个人账号）：这一档 Apple 不给公证，别把包提上去等一个看不懂的错"
  fi
  security find-identity -v -p codesigning 2>/dev/null | grep -qF "$SIGN_ID" \
    || fail "钥匙串里没有签名身份「${SIGN_ID}」（security find-identity -v -p codesigning 自己看）"
fi

mkdir -p "$ROOT/tmp" "$OUT"
OUT="$(cd "$OUT" && pwd)"   # 绝对化：脚本里有若干 ( cd "$OUT" && ... ) 子壳
WORK="$(mktemp -d "$ROOT/tmp/package-mac-XXXXXX")"
. "$ROOT/scripts/lib/package-dmg.sh"
MOUNT=""
cleanup() {
  local status=$?
  trap - EXIT
  if ! gt_package_cleanup_work "$WORK" "$MOUNT" "$KEEP_WORK"; then
    [ "$status" -ne 0 ] || status=1
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

APP="$OUT/Ghosterm.app"
FW="$APP/Contents/Frameworks"
XCFW="$ROOT/build/GhosttyKit.xcframework"
MAIN_EXE="$APP/Contents/MacOS/GhostermApp"

# ------------------------------------------------------------------ 0. 工具链
step "0/9 工具链自检"
for tool in swift cmake codesign iconutil hdiutil otool plutil; do
  command -v "$tool" >/dev/null 2>&1 || fail "缺少 ${tool}（Xcode 命令行工具装了吗？）"
done
[ "$ARCH" = "$(uname -m)" ] || note "注意：--arch ${ARCH} 与主机 $(uname -m) 不同，SwiftPM 需要交叉编译能力"

# 版本号：显式 > git tag > 兜底
if [ -z "$VERSION" ]; then
  # 只认版本形态的 tag（`v1.2.3` 或 `1.2.3`）；仓库里还有归档 tag
  # `archive/lynx-rust-v1`，不加 --match 会被它污染版本号（实测踩过）。
  # 没有版本 tag 时落到 0.1.0-dev —— 诚实表达「还没有发布」。
  VERSION="$(git describe --tags --abbrev=0 \
               --match 'v[0-9]*' --match '[0-9]*' 2>/dev/null | sed 's/^v//')" || VERSION=""
  [ -n "$VERSION" ] || VERSION="0.1.0-dev"
fi
if [ -z "$BUILD_NUMBER" ]; then
  BUILD_NUMBER="$(git rev-list --count HEAD 2>/dev/null || echo 1)"
fi
GIT_SHA="$(git rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
GIT_DIRTY=""
[ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ] && GIT_DIRTY="+dirty"
note "版本 ${VERSION}（build ${BUILD_NUMBER}）· 源码 ${GIT_SHA}${GIT_DIRTY} · 架构 ${ARCH}"

# ------------------------------------------------------------------ 1. libghostty
step "1/9 libghostty（GhosttyKit.xcframework）"
PIN="$(git -C "$ROOT/vendor/ghostty" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
if [ "$FORCE_GHOSTTY" -eq 1 ] || [ ! -d "$XCFW" ]; then
  note "缺产物或强制重建 → 跑 scripts/build-ghostty.sh（需要 zig 0.16.0 + Metal toolchain）"
  bash "$ROOT/scripts/build-ghostty.sh" || fail "libghostty 构建失败（见上方输出）"
else
  note "复用已有 ${XCFW}（pin ${PIN}）；要重建加 --force-ghostty"
fi
[ -d "$XCFW" ] || fail "仍然没有 ${XCFW}——无法链接 libghostty"
GHOSTTY_LIB="$XCFW/macos-${ARCH}/libghostty-internal.a"
[ -f "$GHOSTTY_LIB" ] || fail "GhosttyKit 里没有 ${ARCH} 切片（${GHOSTTY_LIB}）——目前只构建了 arm64"
note "GhosttyKit ${ARCH} 切片 $(du -h "$GHOSTTY_LIB" | awk '{print $1}')"

# ------------------------------------------------------------------ 2. C 核心
step "2/9 构建 C 核心"
bash "$ROOT/scripts/build-core.sh" || fail "core 构建失败"
note "core/build/libghosterm_core.a"

# ------------------------------------------------------------------ 3. release 构建
step "3/9 SwiftPM release 构建"
( cd "$ROOT/apple/GhostermApp" && swift build -c release --arch "$ARCH" ) >"$WORK/swift-build.log" 2>&1 \
  || { tail -30 "$WORK/swift-build.log" >&2; fail "swift build 失败（完整日志 $WORK/swift-build.log）"; }
BIN="$ROOT/apple/GhostermApp/.build/release/GhostermApp"
[ -x "$BIN" ] || fail "找不到 release 产物 $BIN"
note "${BIN}（$(du -h "$BIN" | awk '{print $1}')）"

# ------------------------------------------------------------------ 4. 组装 bundle
step "4/9 组装 .app（图标 / Info.plist / 可执行）"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources" "$FW"
cp "$BIN" "$MAIN_EXE"
# 图标：从 assets/icons 的母版派生产物生成 .icns（不在打包时重画 SVG，避免第二套真相）
ICONSET_SRC="$ROOT/assets/icons/apple/AppIcon.appiconset"
[ -d "$ICONSET_SRC" ] || fail "缺 ${ICONSET_SRC}（先跑 bash scripts/build-icons.sh apple）"
ICONSET="$WORK/AppIcon.iconset"
mkdir -p "$ICONSET"
copy_icon() { # <像素尺寸> <iconset 文件名>
  [ -f "$ICONSET_SRC/mac-$1.png" ] || fail "缺图标 mac-$1.png"
  cp "$ICONSET_SRC/mac-$1.png" "$ICONSET/$2"
}
copy_icon 16  icon_16x16.png
copy_icon 32  icon_16x16@2x.png
copy_icon 32  icon_32x32.png
copy_icon 64  icon_32x32@2x.png
copy_icon 128 icon_128x128.png
copy_icon 256 icon_128x128@2x.png
copy_icon 256 icon_256x256.png
copy_icon 512 icon_256x256@2x.png
copy_icon 512 icon_512x512.png
copy_icon 1024 icon_512x512@2x.png
iconutil -c icns "$ICONSET" -o "$APP/Contents/Resources/AppIcon.icns" \
  || fail "iconutil 生成 AppIcon.icns 失败"
note "AppIcon.icns $(du -h "$APP/Contents/Resources/AppIcon.icns" | awk '{print $1}')（源：assets/icons/apple）"

# 主题目录（Ghostty 配色集）：libghostty 从**资源目录**找
# `<Resources>/ghostty/themes`（src/config/theme.zig + src/os/resourcesdir.zig）。
# 少了它，界面里的主题选择器只能拿到空列表。
#
# **本步自给自足**（踩过的坑）：早先这里只认 `tmp/ghostty-resources/`
# ——那是 build-ghostty.sh 的副作用，而本脚本在 xcframework 已缓存时
# **不跑** build-ghostty.sh，于是这个目录根本不存在；而当时的实现把它
# 当成 `note`（SKIP）放过去了，出过一个**没有主题目录**的包，用户看到
# 的是「浅色/深色配色两个下拉全空」而找不出原因。现在：直接找 zig-pkg
# 的配色集；找不到就**失败**，不静默出一个「主题功能像是没做」的包。
. "$ROOT/scripts/lib/ghostty-themes.sh"
THEMES_DST="$APP/Contents/Resources/ghostty/themes"
if THEMES_SRC="$(ghostty_themes_find "$ROOT/vendor/ghostty/zig-pkg")"; then
  N="$(ghostty_themes_stage "$THEMES_SRC" "$THEMES_DST")"
  note "主题 ${N} 个 → Contents/Resources/ghostty/themes（源 ${THEMES_SRC}）"
elif [ -d "$ROOT/tmp/ghostty-resources/ghostty/themes" ]; then
  # zig-pkg 被清了但暂存目录还在（早先构建留下的）——用暂存的那份。
  N="$(ghostty_themes_stage "$ROOT/tmp/ghostty-resources/ghostty/themes" "$THEMES_DST")"
  note "主题 ${N} 个 → Contents/Resources/ghostty/themes（源 tmp 暂存目录）"
else
  fail "找不到 Ghostty 配色集（既不在 vendor/ghostty/zig-pkg，也不在 tmp/ghostty-resources 暂存目录）——跑一次 bash scripts/build-ghostty.sh 会把 zig-pkg 拉下来"
fi

# Record the verified patched SSH dependency before bundle relocation/signing.
python3 "$ROOT/core/cmake/verify-ssh2.py" --root "$ROOT" --prefix "$ROOT/build/host-native" --stage-app "$APP" \
  || fail "锁定 SSH 依赖校验失败"

# ------------------------------------------------------------------ 5. 内嵌动态库
step "5/9 内嵌依赖闭包（自包含）"
EMBED_LOG="$WORK/embed.log"
python3 "$ROOT/scripts/lib/embed-dylibs.py" embed "$APP" >"$EMBED_LOG" 2>&1 \
  || { cat "$EMBED_LOG" >&2; fail "依赖内嵌失败"; }
grep '^embed:' "$EMBED_LOG" | sed 's/^embed:/   /'
REAL_MIN_OS="$(sed -n 's/^MINOS_MAX=//p' "$EMBED_LOG")"
MIN_OS_SRC="$(sed -n 's/^MINOS_SRC=//p' "$EMBED_LOG")"
[ -n "$REAL_MIN_OS" ] || fail "拿不到内嵌库的 minos（embed 输出异常）"

# ------------------------------------------------------------------ 6. Info.plist
step "6/9 Info.plist（版本 / 最低系统）"
TEMPLATE="$ROOT/apple/GhostermApp/Resources/Info.plist.in"
[ -f "$TEMPLATE" ] || fail "缺模板 $TEMPLATE"
APP_MIN_OS="$(otool -l "$MAIN_EXE" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')"
if [ "$MIN_OS" = "auto" ]; then
  # 声明的最低系统 = max(二进制, 内嵌库)。宁可写高，也不写一个跑不起来的低值。
  MIN_OS="$(printf '%s\n%s\n' "$APP_MIN_OS" "$REAL_MIN_OS" | sort -V | tail -1)"
  if [ "$MIN_OS" != "$APP_MIN_OS" ]; then
    echo "   ⚠️  依赖把最低系统从 ${APP_MIN_OS} 抬到 ${MIN_OS}（最高者：${MIN_OS_SRC}）"
    echo "      原因：内嵌的动态库来自 Homebrew，是按本机系统（minos ${REAL_MIN_OS}）构建的。"
    echo "      要真正降到 ${APP_MIN_OS}：自建 OpenSSL 并指定 MACOSX_DEPLOYMENT_TARGET，"
    echo "      或改用不依赖 Homebrew 的方案（见 docs/reference/roadmap.md 打包条目）。"
  fi
else
  # 显式指定的值不得低于实际需求，否则就是在谎报可运行范围。
  # 判据：两者取高者，若高者不是用户给的值，说明用户给低了。
  # （曾经的写法是跟「两者取低者」比，方向正好反了——变异 A4 抓到的。）
  HIGHEST="$(printf '%s\n%s\n' "$MIN_OS" "$REAL_MIN_OS" | sort -V | tail -1)"
  [ "$HIGHEST" = "$MIN_OS" ] \
    || fail "--min-os ${MIN_OS} 低于依赖实际要求 ${REAL_MIN_OS}（${MIN_OS_SRC}）：拒绝谎报可运行范围"
fi
sed -e "s/@VERSION@/${VERSION}/" -e "s/@BUILD@/${BUILD_NUMBER}/" -e "s/@MIN_OS@/${MIN_OS}/" \
  "$TEMPLATE" > "$APP/Contents/Info.plist"
plutil -lint "$APP/Contents/Info.plist" >/dev/null || fail "Info.plist 不合法"
note "CFBundleShortVersionString=${VERSION} · CFBundleVersion=${BUILD_NUMBER} · LSMinimumSystemVersion=${MIN_OS}"

# 构建信息：**必须在签名前**写进 bundle —— 签名后再加文件会破坏 sealed resource
# （codesign 报 "a sealed resource is missing or invalid / file added"，实测被
#  dmg 挂载复验抓到）。所以这里生成一次，bundle 内与 dist/ 用同一份内容。
INFO="$WORK/BUILD-INFO.txt"
# 打包时刻（UTC）：这是**本机装的这份包**唯一可用的时间锚。dev 渠道的检测在拿不到
# commit 时要拿 `version.json` 的 build_time 和本地这一刻比大小（见
# .github/workflows/dev-build.yml 与 docs/how-to/dev-build.md）——两侧必须是同一个
# 语义（都是「这份包打出来的时刻」），否则兜底那条路永远在比错的东西。
BUILD_STAMP="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
{
  echo "Ghosterm macOS 包"
  echo "版本:        ${VERSION} (build ${BUILD_NUMBER})"
  echo "构建时刻:    ${BUILD_STAMP}"
  echo "架构:        ${ARCH}"
  echo "最低系统:    ${MIN_OS}   # 二进制自身 ${APP_MIN_OS}，内嵌库最高 ${REAL_MIN_OS}（${MIN_OS_SRC}）"
  echo "签名:        ${SIGN_MODE}${SIGN_ID:+ ${SIGN_ID}}"
  echo "公证:        ${NOTARIZE_PROFILE:-无（未公证的包不能直接分发给终端用户）}"
  echo "源码:        git ${GIT_SHA}${GIT_DIRTY}"
  echo "libghostty:  pin ${PIN} + patches/libghostty/${PIN}/"
  echo "内嵌动态库:"
  grep '^embed:' "$EMBED_LOG" | sed 's/^embed: /  - /'
  echo "工具链:      $(swift --version 2>/dev/null | head -1) · $(xcodebuild -version 2>/dev/null | head -1) · cmake $(cmake --version | head -1 | awk '{print $3}')"
} > "$INFO"
cp "$INFO" "$APP/Contents/Resources/BUILD-INFO.txt"
note "BUILD-INFO.txt 已写入包内（签名会覆盖它）"
python3 "$ROOT/scripts/check-terminal-package.py" --app "$APP" \
  || fail "普通终端包仍含旧会话 runtime 或引擎不匹配"

# ------------------------------------------------------------------ 7. 签名
step "7/9 代码签名（模式：${SIGN_MODE}）"
SIGN_ARGS=()
case "$SIGN_MODE" in
  adhoc)
    SIGN_ARGS=(-s -)
    note "ad-hoc 签名：本机/内测可跑；终端用户下载来首次打开要不要放行，取决于取包工具贴不贴"
    note "   com.apple.quarantine（浏览器与 brew --cask 都贴；curl + ditto 不贴）——证据档见 docs/how-to/dev-build.md"
    ;;
  id)
    # 身份类型与钥匙串存在性在起跑时已核过（见上面的签名档分类）——这里只负责签。
    SIGN_ARGS=(-s "$SIGN_ID" --options runtime --timestamp)
    note "Developer ID 签名 + hardened runtime：${SIGN_ID}"
    ;;
  dev)
    SIGN_ARGS=(-s "$SIGN_ID" --options runtime --timestamp)
    note "Apple Development 签名（免费个人账号）+ hardened runtime：${SIGN_ID}"
    note "   这一档**不能公证**：用户下载来仍会被 Gatekeeper 拦（同 ad-hoc，看取包工具贴不贴 quarantine）；"
    note "   BUILD-INFO 与 version.json 记的是 dev，不是 id，app 侧照样不许自动安装。"
    ;;
  none)
    note "跳过签名（--no-sign）：产物未签名，不能分发"
    ;;
esac
if [ "$SIGN_MODE" != "none" ]; then
  # 先签内嵌库，再签 App（顺序重要：改一个内嵌库的签名会让 App 的签名失效）
  for lib in "$FW"/*.dylib; do
    [ -e "$lib" ] || continue
    codesign --force "${SIGN_ARGS[@]}" "$lib" >/dev/null 2>&1 || fail "签名失败：$lib"
  done
  codesign --force "${SIGN_ARGS[@]}" "$APP" >/dev/null 2>&1 || fail "签名失败：$APP"
  codesign --verify --strict --deep --verbose=2 "$APP" 2>&1 | sed 's/^/   /'
  if [ "$SIGN_MODE" = "adhoc" ]; then
    note "签名身份：ad-hoc（无 CA 身份，Gatekeeper 不认，内测尺度）"
  else
    note "签名身份：$(codesign -dv "$APP" 2>&1 | awk -F= '/^Authority=/{print $2; exit}')"
  fi
fi

# ------------------------------------------------------------------ 8. 校验
step "8/9 校验（自包含 / 运行时 / 签名）"
python3 "$ROOT/scripts/lib/embed-dylibs.py" check "$APP" || fail "自包含复验失败"
python3 "$ROOT/core/cmake/verify-ssh2.py" --root "$ROOT" --prefix "$ROOT/build/host-native" --app "$APP" \
  || fail "随包 SSH 依赖不是已校验的修复版本"
for key in CFBundleExecutable CFBundleIdentifier CFBundleShortVersionString CFBundleVersion LSMinimumSystemVersion CFBundleIconFile; do
  plutil -extract "$key" raw "$APP/Contents/Info.plist" >/dev/null || fail "Info.plist 缺 $key"
done
[ -x "$MAIN_EXE" ] || fail "可执行位丢了：$MAIN_EXE"
[ -f "$APP/Contents/Resources/AppIcon.icns" ] || fail "缺 AppIcon.icns"

# 运行时断言：真正跑一次包内二进制，并让它把「加载了哪些库」打出来。
# 只要有任何一条落到 /opt/homebrew 或 /usr/local，就说明自包含是假的。
if [ "$ARCH" = "$(uname -m)" ]; then
  SMOKE_OUT="$(DYLD_PRINT_LIBRARIES=1 "$MAIN_EXE" --local /bin/sh 2>"$WORK/dyld.log" | head -1 || true)"
  echo "$SMOKE_OUT" | grep -q "GRID" || { sed 's/^/   /' "$WORK/dyld.log" | tail -20 >&2; fail "包内二进制冒烟失败（无 GRID 输出）"; }
  if grep -E "/opt/homebrew|/usr/local" "$WORK/dyld.log" >/dev/null 2>&1; then
    grep -E "/opt/homebrew|/usr/local" "$WORK/dyld.log" | sed 's/^/   /' >&2
    fail "运行时仍从 Homebrew 加载了库（自包含不成立）"
  fi
  note "冒烟 ok：${SMOKE_OUT}"

  # 包内配置检查：主题这类资源靠「沿可执行文件路径推导资源目录」找，
  # 那是打包布局的函数，只有真装配好的 .app 里才验得到。
  # 写一份用**随包带的主题名**的配置 → 引擎必须接受（诊断为空）。
  THEME_CONF="$WORK/theme-check.conf"
  [ -d "$APP/Contents/Resources/ghostty/themes" ] \
    || fail "包内没有主题目录——上面的配色集步骤应当已经拦住，这里是最后一道"
  # 挑一个有代表性的名字（带空格，能顺带验引号处理）；没有就用第一个。
  THEME_NAME="iTerm2 Default"
  [ -f "$APP/Contents/Resources/ghostty/themes/$THEME_NAME" ] \
    || THEME_NAME="$(ls "$APP/Contents/Resources/ghostty/themes" | head -1)"
  printf 'theme = "%s"\n' "$THEME_NAME" > "$THEME_CONF"
  CONFIG_OUT="$("$MAIN_EXE" --config-check "$THEME_CONF" 2>&1 | grep -E 'CONFIG_OK|诊断|失败' | head -1 || true)"
  echo "${CONFIG_OUT}" | grep -q "CONFIG_OK diagnostics=0" \
    || fail "包内配置检查失败（随包主题 ${THEME_NAME} 未被引擎接受）：${CONFIG_OUT}"
  note "包内配置检查 ok：随包主题「${THEME_NAME}」被引擎接受"
  # 统计要诚实：`grep -c Frameworks/` 会把 /System/Library/Frameworks 一起数进去
  # （数字好看但没意义）。这里只数「来自本 bundle」与「内嵌库」两个口径。
  TOTAL_LOADS="$(grep -c '^dyld\[' "$WORK/dyld.log" 2>/dev/null || true)"
  BUNDLE_LOADS="$(grep -cF "$APP/" "$WORK/dyld.log" 2>/dev/null || true)"
  note "运行时加载镜像 ${TOTAL_LOADS:-0} 个；来自本 bundle ${BUNDLE_LOADS:-0} 个（内嵌库 $(ls "$FW" | wc -l | tr -d ' ') 个）"
else
  note "SKIP 运行冒烟：目标架构 ${ARCH} 与主机 $(uname -m) 不同"
fi

if [ "$GUI_SMOKE" -eq 1 ]; then
  # 走真实启动路径（LaunchServices 认不认这个 bundle，只有 open 才知道）。
  if [ "$(launchctl managername 2>/dev/null)" != "Aqua" ]; then
    note "SKIP GUI 冒烟：当前不是 Aqua 图形会话（${USER}）"
  else
    open -n "$APP" || fail "open 启动失败（Info.plist/签名有问题？）"
    sleep 3
    if pgrep -f "${MAIN_EXE}" >/dev/null 2>&1; then
      note "GUI 冒烟 ok：open 后进程存活，收工前关掉"
      pkill -f "${MAIN_EXE}" >/dev/null 2>&1 || true
    else
      fail "open 之后进程没起来（崩溃？看 Console.app 或 ~/Library/Logs）"
    fi
  fi
fi

if [ -n "$NOTARIZE_PROFILE" ]; then
  [ "$SIGN_MODE" = "id" ] || fail "--notarize 需要 Developer ID 真证书签名（--sign \"Developer ID Application: ...\"）——当前签名档是「${SIGN_MODE}」"
fi

# ------------------------------------------------------------------ 9. 产物
step "9/9 产物（dmg / zip / 校验和 / 构建信息）"
BASE="Ghosterm-${VERSION}-${ARCH}"
cp "$INFO" "$OUT/BUILD-INFO.txt"
note "$(basename "$OUT/BUILD-INFO.txt")"

DIST_FILES=()
if [ "$APP_ONLY" -eq 0 ]; then
  # DMG：暂存目录里放 .app + /Applications 软链（标准的拖拽安装形态）
  STAGE="$WORK/dmg"
  mkdir -p "$STAGE"
  ditto "$APP" "$STAGE/Ghosterm.app"   # 用 ditto：保留符号链接/权限，且不塞 FinderInfo
  ln -s /Applications "$STAGE/Applications"
  DMG="$OUT/${BASE}.dmg"
  rm -f "$DMG"
  hdiutil create -volname "Ghosterm" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null \
    || fail "hdiutil 创建 dmg 失败"
  hdiutil verify "$DMG" >/dev/null || fail "dmg 校验失败"
  # 真挂载一次：证明 dmg 不是「能生成但打不开」，且里面的 .app 签名完整。
  MOUNT="$WORK/mnt"
  mkdir -p "$MOUNT"
  if hdiutil attach "$DMG" -nobrowse -readonly -mountpoint "$MOUNT" >/dev/null 2>&1; then
    [ -d "$MOUNT/Ghosterm.app" ] || { fail "dmg 里没有 Ghosterm.app"; }
    codesign --verify --strict --deep "$MOUNT/Ghosterm.app" >/dev/null 2>&1 \
      || { fail "dmg 内的 .app 签名不完整"; }
    if [ -x "$MOUNT/Ghosterm.app/Contents/MacOS/GhostermApp" ]; then
      MOUNTED_SMOKE="$(DYLD_PRINT_LIBRARIES=1 "$MOUNT/Ghosterm.app/Contents/MacOS/GhostermApp" --local /bin/sh 2>"$WORK/dyld-mount.log" | head -1 || true)"
      echo "${MOUNTED_SMOKE}" | grep -q GRID || { fail "dmg 内的二进制跑不起来（无 GRID）"; }
      grep -qE "/opt/homebrew|/usr/local" "$WORK/dyld-mount.log" && { fail "从 dmg 运行时仍在用 Homebrew 的库"; }
      note "dmg 内二进制冒烟 ok：${MOUNTED_SMOKE}"
    fi
    [ -L "$MOUNT/Applications" ] || note "（提示：dmg 内缺 /Applications 软链）"
    gt_package_detach_dmg "$MOUNT" || fail "dmg 卸载失败，保留挂载与临时目录供排查"
    note "dmg 已挂载复验（.app 存在 + 签名完整）"
  else
    note "SKIP dmg 挂载复验：当前环境挂不上（CI/无权限），只做了 checksum 校验"
  fi
  note "$(basename "$DMG")（$(du -h "$DMG" | awk '{print $1}')）"
  DIST_FILES+=("$DMG")

  if [ -n "$NOTARIZE_PROFILE" ]; then
    step "公证（notarytool profile：${NOTARIZE_PROFILE}）"
    xcrun notarytool submit "$DMG" --keychain-profile "$NOTARIZE_PROFILE" --wait \
      || fail "公证失败（凭据/网络/Apple 侧问题）"
    xcrun stapler staple "$DMG" || fail "stapler staple 失败"
    xcrun stapler validate "$DMG" || fail "staple 校验失败"
    note "已公证并 staple：$(basename "$DMG")"
  fi
fi

if [ "$MAKE_ZIP" -eq 1 ]; then
  ZIP="$OUT/${BASE}.zip"
  rm -f "$ZIP"
  ( cd "$OUT" && ditto -c -k --sequesterRsrc --keepParent Ghosterm.app "$ZIP" ) || fail "ditto 打 zip 失败"
  note "$(basename "$ZIP")（$(du -h "$ZIP" | awk '{print $1}')）"
  DIST_FILES+=("$ZIP")
fi

(
  cd "$OUT"
  : > SHA256SUMS
  for f in ${DIST_FILES[@]+"${DIST_FILES[@]}"} "$INFO"; do
    shasum -a 256 "$(basename "$f")" >> SHA256SUMS
  done
) || fail "生成 SHA256SUMS 失败"
note "SHA256SUMS"
sed 's/^/   /' "$OUT/SHA256SUMS"

echo
echo "PACKAGE-MAC PASS"
echo "  App:  ${APP}（$(du -sh "$APP" | awk '{print $1}')）"
for f in ${DIST_FILES[@]+"${DIST_FILES[@]}"}; do echo "  产物: $f"; done
echo "  信息: $OUT/BUILD-INFO.txt"
case "$SIGN_MODE" in
  adhoc)
    echo "  提示: 这是 ad-hoc 签名的包，适合本机与内测；正式分发请用 --sign \"Developer ID Application: ...\" --notarize <profile>"
    ;;
  dev)
    echo "  提示: Apple Development（免费个人账号）签名——身份稳定但**不可公证**，用户下载来仍被 Gatekeeper 拦；正式分发需要付费计划的 Developer ID + 公证"
    ;;
esac
