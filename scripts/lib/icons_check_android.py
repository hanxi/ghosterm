#!/usr/bin/env python3
"""核对 Android 自适应图标前景：内容必须落在 108dp 视口的 72dp 安全区内。

用法: icons_check_android.py <ic_launcher_foreground.xml>

为什么要有这条断言：真机裁切只有上机才知道，但「前景是否落在安全区」是纯几何问题，
可以在生成时就判定。安全区取官方口径：108dp 画布中心 72dp 见方（内容再留余量缩到 66dp）。
"""
import os
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

AND = "{http://schemas.android.com/apk/res/android}"
VIEWPORT = 108.0   # dp
SAFE = 72.0        # dp，安全区边长
RENDER = 432       # 光栅化像素（108dp × 4）


def flatten(root):
    parts = []
    for path in root.findall("path"):
        d = path.get(AND + "pathData")
        fill = path.get(AND + "fillColor")
        stroke = path.get(AND + "strokeColor")
        if stroke and not fill:
            width = path.get(AND + "strokeWidth") or "1"
            parts.append(f'<path d="{d}" fill="none" stroke="{stroke}" '
                         f'stroke-width="{width}" stroke-linecap="round" '
                         f'stroke-linejoin="round"/>')
        else:
            parts.append(f'<path d="{d}" fill="{fill}"/>')
    return "".join(parts)


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    xml_path = sys.argv[1]
    root = ET.parse(xml_path).getroot()
    if (float(root.get(AND + "viewportWidth")),
            float(root.get(AND + "viewportHeight"))) != (VIEWPORT, VIEWPORT):
        sys.exit(f"icons_check_android: 视口应为 {VIEWPORT}dp")

    group = root.find("group")
    if group is None:
        sys.exit("icons_check_android: 缺少 <group>，无法换算安全区")
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{VIEWPORT}" '
           f'height="{VIEWPORT}" viewBox="0 0 {VIEWPORT} {VIEWPORT}">'
           f'<g transform="translate({group.get(AND + "translateX")} '
           f'{group.get(AND + "translateY")}) scale({group.get(AND + "scaleX")})">'
           f'{flatten(group)}</g></svg>')

    # 临时文件一律放工程 tmp/（AGENTS.md 环境硬约束），不污染产物目录。
    work = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "tmp", "icons")
    os.makedirs(work, exist_ok=True)
    with tempfile.NamedTemporaryFile("w", suffix=".svg", dir=work, delete=False) as fh:
        fh.write(svg)
        tmp_svg = fh.name
    tmp_png = tmp_svg + ".png"
    try:
        subprocess.run(["rsvg-convert", "-w", str(RENDER), "-h", str(RENDER),
                        tmp_svg, "-o", tmp_png], check=True)
        from PIL import Image
        box = Image.open(tmp_png).convert("RGBA").getbbox()
    finally:
        for p in (tmp_svg, tmp_png):
            if os.path.exists(p):
                os.unlink(p)
    if box is None:
        sys.exit("icons_check_android: 前景是空白图")

    scale = RENDER / VIEWPORT
    pad = (VIEWPORT - SAFE) / 2.0 * scale
    inside = (box[0] >= pad - 1 and box[1] >= pad - 1
              and box[2] <= RENDER - pad + 1 and box[3] <= RENDER - pad + 1)
    dp = tuple(round(v / scale, 1) for v in box)
    if not inside:
        sys.exit(f"icons_check_android: 前景 {dp}dp 越出 {SAFE}dp 安全区"
                 f"（允许 {pad:.0f}..{RENDER - pad:.0f}px）—— 系统裁切会切到图形")
    print(f"  android 前景 {dp}dp，落在 {SAFE}dp 安全区内")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
