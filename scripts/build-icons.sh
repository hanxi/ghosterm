#!/usr/bin/env bash
# Ghosterm 图标生成器 —— 全部产物由 assets/icons/ghosterm.svg（母版）派生。
#
#   bash scripts/build-icons.sh            # 全端生成（= all）
#   bash scripts/build-icons.sh apple      # 只生成 Apple（macOS + iOS）
#   bash scripts/build-icons.sh android    # Android（含 adaptive icon）
#   bash scripts/build-icons.sh harmonyos  # HarmonyOS
#   bash scripts/build-icons.sh linux      # Linux（含 hicolor 图标树）
#   bash scripts/build-icons.sh web        # Web（favicon / PWA）
#   bash scripts/build-icons.sh windows    # Windows（ICO / MSIX）
#   bash scripts/build-icons.sh mark       # 只要幽灵窗标记（透明底 SVG）
#   bash scripts/build-icons.sh check      # 重新生成并核对产物无漂移（CI 用）
#
# 依赖：rsvg-convert(librsvg) + magick(ImageMagick) + python3。
# 产物全部落在 assets/icons/ 下，SHA256SUMS 记录每次生成的哈希。
# 生成是确定性的（-strip 去元数据）：同一母版 + 同一工具链 → 同一字节。
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/icons.sh
source "$HERE/lib/icons.sh"

OUT="$ICONS_OUT"

# ---------------------------------------------------------------- mark（透明底标记）
build_mark() {
  info "ghosterm-mark.svg（透明底幽灵窗标记）"
  icons_mark_svg "$OUT/ghosterm-mark.svg"
}

# ---------------------------------------------------------------- Apple（macOS + iOS）
# macOS（Tahoe 26 起）与 iOS 都由**系统**套圆角遮罩，故两边一律满画布直角、不透明：
#   · 预圆角的图在 Tahoe 会被塞进新的"底座"(enclosure)——外面套一圈浅色圆角框、
#     里面缩进一个方块，看着像"黑板上加白框"（详见 assets/icons/README.md）；
#   · 满画布直角图则被系统干净地裁成圆角，不留底座。
# 两边 idiom 不同、像素尺寸会撞（如 1024），故文件按前缀区分：mac-* / ios-*。
APPLE_MAC=(
  "16 1x 16x16" "32 2x 16x16"
  "32 1x 32x32" "64 2x 32x32"
  "128 1x 128x128" "256 2x 128x128"
  "256 1x 256x256" "512 2x 256x256"
  "512 1x 512x512" "1024 2x 512x512"
)
APPLE_IOS=(
  "40 2x 20x20" "60 3x 20x20"
  "58 2x 29x29" "87 3x 29x29"
  "80 2x 40x40" "120 3x 40x40"
  "120 2x 60x60" "180 3x 60x60"
  "76 1x 76x76" "152 2x 76x76"
  "167 2x 83.5x83.5"
  "1024 1x 1024x1024"
)
build_apple() {
  local dir="$OUT/apple/AppIcon.appiconset"
  info "apple/AppIcon.appiconset（macOS / iOS 均满画布直角、不透明，系统自己裁圆角）"
  local entry seen=""
  for entry in "${APPLE_MAC[@]}"; do
    set -- $entry
    if [[ " $seen " == *" $1 "* ]]; then continue; fi
    seen="$seen $1"
    icons_png "$1" "$dir/mac-$1.png" "$ICONS_RADIUS_SQUARE" 1
  done
  seen=""
  for entry in "${APPLE_IOS[@]}"; do
    set -- $entry
    if [[ " $seen " == *" $1 "* ]]; then continue; fi
    seen="$seen $1"
    icons_png "$1" "$dir/ios-$1.png" "$ICONS_RADIUS_SQUARE" 1
  done
  python3 "$HERE/lib/icons_appicon_json.py" "$dir" "${APPLE_MAC[@]}" -- "${APPLE_IOS[@]}"
}

# ---------------------------------------------------------------- Android
# legacy：圆角 PNG（API < 26）；adaptive：anydpi-v26 XML + 前景矢量 + 背景色。
ANDROID_DENSITIES=("mdpi 48" "hdpi 72" "xhdpi 96" "xxhdpi 144" "xxxhdpi 192")
build_android() {
  local dir="$OUT/android" d size
  info "android/mipmap-*（legacy 圆角 + 圆形）"
  for d in "${ANDROID_DENSITIES[@]}"; do
    set -- $d
    icons_png "$2" "$dir/mipmap-$1/ic_launcher.png" "$ICONS_RADIUS_APPLE" 0
    icons_png "$2" "$dir/mipmap-$1/ic_launcher_round.png" 0.5 0
  done
  info "android/adaptive（108dp 视口，前景 66dp 安全区）"
  # 108dp 视口；内容按 66dp 见方缩放并居中，落在中间 72dp 安全区内。
  local bbox s tx ty
  bbox=$(icons_mark_bbox)
  read -r s tx ty <<<"$(icons_fit "$bbox" 108 66)"
  python3 "$HERE/lib/icons_android.py" "$ICONS_MASTER" "$dir" "$s" "$tx" "$ty"
  python3 "$HERE/lib/icons_check_android.py" "$dir/drawable/ic_launcher_foreground.xml"
}

# ---------------------------------------------------------------- HarmonyOS
build_harmonyos() {
  info "harmonyos（圆角独立 SVG）"
  icons_rounded_svg "$ICONS_RADIUS_APPLE" "$OUT/harmonyos/app_icon.svg"
  cp "$OUT/harmonyos/app_icon.svg" "$OUT/harmonyos/icon.svg"
}

# ---------------------------------------------------------------- Linux
build_linux() {
  local name="io.github.ghosterm.Ghosterm" size
  info "linux/hicolor（SVG 矢量 + 16…512 PNG）"
  icons_rounded_svg "$ICONS_RADIUS_APPLE" "$OUT/linux/$name.svg"
  for size in 16 24 32 48 64 128 256 512; do
    icons_png "$size" "$OUT/linux/hicolor/${size}x${size}/apps/$name.png" "$ICONS_RADIUS_APPLE" 0
  done
}

# ---------------------------------------------------------------- Web
build_web() {
  local dir="$OUT/web"; mkdir -p "$dir"
  info "web（favicon / PWA）"
  icons_rounded_svg "$ICONS_RADIUS_APPLE" "$dir/icon.svg"
  icons_png 192 "$dir/icon-192.png" "$ICONS_RADIUS_APPLE" 0
  icons_png 512 "$dir/icon-512.png" "$ICONS_RADIUS_APPLE" 0
  icons_png 180 "$dir/apple-touch-icon.png" "$ICONS_RADIUS_SQUARE" 1
  icons_png 32  "$dir/favicon-32.png" "$ICONS_RADIUS_SQUARE" 0
  local work; work=$(_icons_workdir)
  icons_png 16 "$work/f16.png" "$ICONS_RADIUS_SQUARE" 0
  icons_png 32 "$work/f32.png" "$ICONS_RADIUS_SQUARE" 0
  icons_png 48 "$work/f48.png" "$ICONS_RADIUS_SQUARE" 0
  magick "$work/f16.png" "$work/f32.png" "$work/f48.png" "$dir/favicon.ico"
  rm -rf "$work"
}

# ---------------------------------------------------------------- Windows
build_windows() {
  local dir="$OUT/windows" work; mkdir -p "$dir"
  # Windows 不对应用图标做遮罩（Win11 任务栏 / 开始菜单直接显示原图），
  # 故这里自带连续圆角 + 透明角，与相邻的现代应用图标观感一致。
  info "windows（ICO 多尺寸 + MSIX PNG，连续圆角 + 透明角）"
  work=$(_icons_workdir)
  local size pngs=()
  for size in 16 24 32 48 64 128 256; do
    icons_png "$size" "$work/$size.png" "$ICONS_RADIUS_APPLE" 0
    pngs+=("$work/$size.png")
  done
  magick "${pngs[@]}" "$dir/Ghosterm.ico"
  rm -rf "$work"
  icons_png 44  "$dir/Ghosterm-44.png"  "$ICONS_RADIUS_APPLE" 0
  icons_png 150 "$dir/Ghosterm-150.png" "$ICONS_RADIUS_APPLE" 0
  icons_png 256 "$dir/Ghosterm.png"     "$ICONS_RADIUS_APPLE" 0
  icons_png 50  "$dir/StoreLogo.png"    "$ICONS_RADIUS_APPLE" 0
}

# ---------------------------------------------------------------- 汇总 / 校验
write_sums() {
  # 只对「生成物 + 母版」记账；README 是文档，改它不该要求重新生成图标。
  ( cd "$OUT" && find . -type f ! -name SHA256SUMS ! -name README.md -print0 \
      | LC_ALL=C sort -z | xargs -0 shasum -a 256 > SHA256SUMS )
  info "SHA256SUMS（$(wc -l < "$OUT/SHA256SUMS" | tr -d ' ') 个文件）"
}

build_all() { build_mark; build_apple; build_android; build_harmonyos; build_linux; build_web; build_windows; }

case "${1:-all}" in
  all)       icons_check_tools; mkdir -p "$ICONS_TMP"; build_all; write_sums ;;
  mark)      icons_check_tools; build_mark; write_sums ;;
  apple)     icons_check_tools; build_apple; write_sums ;;
  android)   icons_check_tools; build_android; write_sums ;;
  harmonyos) icons_check_tools; build_harmonyos; write_sums ;;
  linux)     icons_check_tools; build_linux; write_sums ;;
  web)       icons_check_tools; build_web; write_sums ;;
  windows)   icons_check_tools; build_windows; write_sums ;;
  check)
    # 重新生成一遍，再和已入库的 SHA256SUMS 逐行对拍：
    # 母版/SVG/脚本任一改动却没重新生成，这里必须红。
    icons_check_tools
    [ -f "$OUT/SHA256SUMS" ] || die "缺少 $OUT/SHA256SUMS，先跑 bash scripts/build-icons.sh"
    local_before="$ICONS_TMP/SHA256SUMS.before"
    mkdir -p "$ICONS_TMP"
    cp "$OUT/SHA256SUMS" "$local_before"
    build_all >/dev/null
    write_sums >/dev/null
    if ! diff -u "$local_before" "$OUT/SHA256SUMS"; then
      die "图标产物与母版不一致（上方 diff：左=已入库，右=刚生成）——重跑 bash scripts/build-icons.sh 并提交"
    fi
    printf 'icons: 产物与母版一致（%s 个文件逐字节相同）\n' "$(wc -l < "$OUT/SHA256SUMS" | tr -d ' ')"
    ;;
  *) die "未知目标 '$1'（all|mark|apple|android|harmonyos|linux|web|windows|check）" ;;
esac
