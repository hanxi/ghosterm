#!/usr/bin/env python3
"""从母版抽出 mark 组，输出透明底、无投影、带自身 palette 的独立 SVG。

用法: icons_mark_svg.py <母版.svg> <输出.svg>

只搬运母版里的 <defs> 与 #gt-mark，不重定义任何形状；
供端侧「只想要幽灵窗标记」的场景（如 Linux 通知图标、Web mask-icon）。
"""
import re
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    src, out = sys.argv[1], sys.argv[2]
    text = open(src, encoding="utf-8").read()
    defs = re.search(r"<defs>.*?</defs>", text, re.S)
    mark = re.search(r'<g id="gt-mark">.*?</g>', text, re.S)
    if not defs or not mark:
        print("母版缺少 <defs> 或 #gt-mark", file=sys.stderr)
        return 1
    # 移除外层投影（标记本身不需要），其余保留。
    body = mark.group(0).replace(' filter="url(#gt-shadow)"', "")
    svg = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<!-- 由 scripts/lib/icons_mark.py 从 assets/icons/ghosterm.svg 派生，勿手改 -->\n'
        '<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" '
        'viewBox="0 0 1024 1024" role="img" aria-label="Ghosterm">\n'
        f"  {defs.group(0)}\n  {body}\n</svg>\n"
    )
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(svg)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
