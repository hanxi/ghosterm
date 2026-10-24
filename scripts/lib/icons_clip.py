#!/usr/bin/env python3
"""给母版套一个圆角（连续圆角 squircle）裁切，输出独立 SVG。

用法: icons_clip.py <母版.svg> <输出.svg> <圆角比例>

裁切写进 clipPath，**内容仍是母版原样**（渐变/投影都不动），
只把画布限制成圆角矩形。端侧 PNG/ICO 全部由这个 SVG 直接渲染得到，
因此不需要任何位图蒙版合成。
"""
import re
import sys

from icons_squircle import squircle_path


def clipped_svg(master_text: str, ratio: float) -> str:
    head_end = master_text.index(">", master_text.index("<svg")) + 1
    head, body = master_text[:head_end], master_text[head_end:]
    # 母版头部的注释（「唯一事实来源」那段）在这里会误导——派生产物不是母版。
    head = re.sub(r"<!--.*?-->", "", head, flags=re.S)
    body = body[: body.rindex("</svg>")]
    clip = (
        f'  <defs><clipPath id="gt-clip"><path d="{squircle_path(ratio)}"/>'
        f'</clipPath></defs>\n'
        f'  <g clip-path="url(#gt-clip)">\n{body}  </g>\n'
    )
    note = ("<!-- 由 scripts/lib/icons_clip.py 从 assets/icons/ghosterm.svg 派生，"
            "勿手改；圆角为连续圆角近似。 -->\n")
    return head + "\n" + note + clip + "</svg>\n"


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    src, out, ratio = sys.argv[1], sys.argv[2], float(sys.argv[3])
    if not 0 < ratio <= 0.5:
        print(f"圆角比例必须在 (0, 0.5]，收到 {ratio}", file=sys.stderr)
        return 2
    text = clipped_svg(open(src, encoding="utf-8").read(), ratio)
    with open(out, "w", encoding="utf-8") as fh:
        fh.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
