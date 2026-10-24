#!/usr/bin/env bash
# Ghosterm 图标生成共享库（被 scripts/build-icons.sh source）。
#
# 唯一事实来源：assets/icons/ghosterm.svg（母版，手工维护）。
# 所有端产物只由它派生：本库不含任何形状/颜色的二次定义，
# 形状从母版解析、配色从母版取引用（gradient id）。
# 见 assets/icons/README.md。
set -euo pipefail

ICONS_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ICONS_MASTER="$ICONS_ROOT/assets/icons/ghosterm.svg"
ICONS_OUT="$ICONS_ROOT/assets/icons"
ICONS_TMP="$ICONS_ROOT/tmp/icons"

# macOS/iOS 连续的圆角半径 / 边长（Big Sur 起的 squircle 近似；两端共用同一比例）。
ICONS_RADIUS_APPLE="0.2237"
# 直角（不裁），用于 iOS 方形图标与 favicon。
ICONS_RADIUS_SQUARE=""

die() { printf 'icons: %s\n' "$*" >&2; exit 1; }
info() { printf '  %s\n' "$*"; }

icons_check_tools() {
  command -v rsvg-convert >/dev/null 2>&1 || die "缺少 rsvg-convert（brew install librsvg）"
  command -v magick      >/dev/null 2>&1 || die "缺少 ImageMagick（brew install imagemagick）"
  command -v python3     >/dev/null 2>&1 || die "缺少 python3"
  [ -f "$ICONS_MASTER" ] || die "找不到母版：$ICONS_MASTER"
}

_icons_workdir() {
  mkdir -p "$ICONS_TMP"
  mktemp -d "$ICONS_TMP/work.XXXXXX"
}

# 母版（可选圆角）→ 目标尺寸 PNG。
# 参数：<边长> <输出> [圆角比例|""] [require_opaque:0/1]
#
# 关键实现约束：**全部像素由 rsvg-convert 直接按目标边长渲染**。
#   · 圆角不用位图蒙版，而是先把连续圆角写进 SVG 的 clipPath，再渲染；
#   · 全程不做光栅重采样、不做 ImageMagick 合成 ——
#     实测本机 ImageMagick（7.1.2-30 Q16-HDRI）的 PNG -resize / -composite
#     会产出斜向撕裂的坏图，故这条路径一概不用。
# require_opaque=1 时断言产物无透明像素（iOS / apple-touch-icon 要求）；
# 母版背景哪天不再铺满画布，这里直接报错，不静默降级。
icons_png() {
  local size=$1 out=$2 ratio=${3:-} require_opaque=${4:-0}
  case "$size" in ''|*[!0-9]*) die "icons_png: 非法边长 '$size'";; esac
  local work; work=$(_icons_workdir)
  mkdir -p "$(dirname "$out")"
  local svg="$ICONS_MASTER"
  if [ -n "$ratio" ]; then
    python3 "$ICONS_ROOT/scripts/lib/icons_clip.py" "$ICONS_MASTER" "$work/clip.svg" "$ratio"
    svg="$work/clip.svg"
  fi
  rsvg-convert -w "$size" -h "$size" "$svg" -o "$out"
  if [ "$require_opaque" = 1 ]; then
    # 注意：不同 IM 版本分别打印 True/true，统一小写再比。
    case "$(magick identify -format '%[opaque]' "$out" | tr '[:upper:]' '[:lower:]')" in
      true) ;;
      *) die "$out 含透明像素，但该端要求不透明（母版背景需铺满画布）" ;;
    esac
  fi
  rm -rf "$work"
}

# 纯标记（透明底、无投影）SVG：把母版 mark 组居中放满画布，供端侧直接消费。
icons_mark_svg() {
  local out=$1
  mkdir -p "$(dirname "$out")"
  python3 "$ICONS_ROOT/scripts/lib/icons_mark.py" "$ICONS_MASTER" "$out"
}

# 带圆角裁切的独立 SVG（HarmonyOS / Linux / Web 用）。
icons_rounded_svg() {
  local ratio=$1 out=$2
  mkdir -p "$(dirname "$out")"
  python3 "$ICONS_ROOT/scripts/lib/icons_clip.py" "$ICONS_MASTER" "$out" "$ratio"
}

# 母版 mark 的实际外接框（在 1024 画布坐标系），形如 "600x699+224+150"。
# 用「纯标记 SVG 渲染后 trim」测得，避免把几何常量抄进脚本造成漂移。
icons_mark_bbox() {
  local work; work=$(_icons_workdir)
  python3 "$ICONS_ROOT/scripts/lib/icons_mark.py" "$ICONS_MASTER" "$work/mark.svg"
  rsvg-convert -w 1024 -h 1024 "$work/mark.svg" -o "$work/mark.png"
  local bbox
  bbox=$(magick "$work/mark.png" -trim -format '%wx%h%X%Y' info:)
  rm -rf "$work"
  printf '%s' "$bbox"
}

# 把外接框 "600x699+224+150" 换算成 <组变换>：内容缩放到 content 见方，
# 并在 box 见方的画布里居中。
# 用法: read -r S TX TY <<<"$(icons_fit "$bbox" "$box" "$content")"
icons_fit() {
  python3 - "$@" <<'PY'
import re, sys
if len(sys.argv) != 4:
    sys.exit("icons_fit: 需要 <外接框> <画布边长> <内容边长>")
m = re.fullmatch(r"(\d+)x(\d+)\+(\d+)\+(\d+)", sys.argv[1].strip())
if not m:
    sys.exit(f"icons_fit: 无法解析外接框 '{sys.argv[1]}'")
w, h, x, y = (int(g) for g in m.groups())
box, content = float(sys.argv[2]), float(sys.argv[3])
s = content / max(w, h)
print(f"{s:.6f} {(box - w * s) / 2.0 - x * s:.4f} {(box - h * s) / 2.0 - y * s:.4f}")
PY
}
