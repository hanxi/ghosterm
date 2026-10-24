#!/usr/bin/env python3
"""Apple 风格连续圆角（squircle）几何 —— 单一实现，供裁切共用。

用法: icons_squircle.py <圆角比例 0..0.5> <输出.svg>   # 蒙版 SVG（白=保留）

做法：**直边 + 连续圆角**。直边就是普通圆角矩形的直边；四个角不再用圆弧，
而是用超椭圆角——以该角圆心 (cx,cy) 为原点、半径 r、指数 n=5 的参数弧：
    x = cx ± r·cos^(2/n)θ ,  y = cy ± r·sin^(2/n)θ ,  θ: 0→90°
θ=0 落在一条直边上、θ=90° 落在另一条直边上，与直边切线连续。
n=5 时观感接近 Apple 连续圆角；0.2237 是 Big Sur 起的 macOS/iOS 图标比例。

（先前版本误把参数化扫过「象限」而非「角」，生成自交路径导致裁掉整块，
本版按角心生成，并对外环做闭合/自交断言。）
"""
import math
import sys

SIZE = 1024
N = 5.0  # 超椭圆指数：5 ≈ Apple 连续圆角，2 = 普通圆角
STEPS = 48  # 每个角的采样步数


def squircle_path(ratio: float, size: int = SIZE) -> str:
    r = ratio * size
    m = size / 2.0

    def corner(cx: float, cy: float, sx: int, sy: int):
        pts = []
        for i in range(STEPS + 1):
            t = (math.pi / 2) * i / STEPS
            dx = r * abs(math.cos(t)) ** (2.0 / N)
            dy = r * abs(math.sin(t)) ** (2.0 / N)
            pts.append((cx + sx * dx, cy + sy * dy))
        return pts

    # 顺时针：右上角 → 左上角 → 左下角 → 右下角（SVG 坐标 y 向下）
    ur = corner(size - r, r, +1, -1)       # (size, r) → (size-r, 0)
    ul = corner(r, r, -1, -1)              # (0, r) → (r, 0)
    ll = corner(r, size - r, -1, +1)       # (0, size-r) → (r, size)
    lr = corner(size - r, size - r, +1, +1)  # (size, size-r) → (size-r, size)

    # 顺时针串成闭合外环；每个角弧的方向要顺着行进方向接上相邻直边。
    ring = []
    ring += ur                    # (size, r) → (size-r, 0)，沿右上角
    ring += list(reversed(ul))    # (r, 0) → (0, r)，沿左上角
    ring += ll                    # (0, size-r) → (r, size)，沿左下角
    ring += list(reversed(lr))    # (size-r, size) → (size, size-r)，沿右下角
    assert_convex(ring)
    return "M" + " L".join(f"{x:.2f} {y:.2f}" for x, y in ring) + " Z"


def assert_convex(ring) -> None:
    """外环必须是**简单多边形且同向凸**。

    这条断言就是为了拦住「角弧拼错顺序」这类错误：拼错会自交，
    裁切时把整块图形裁没（PNG 变成全透明），而肉眼在 1024 缩略图上未必看得出。
    """
    n = len(ring)
    signs = set()
    for i in range(n):
        ax, ay = ring[i]
        bx, by = ring[(i + 1) % n]
        cx, cy = ring[(i + 2) % n]
        cross = (bx - ax) * (cy - by) - (by - ay) * (cx - bx)
        if abs(cross) < 1e-9:
            continue
        signs.add(cross > 0)
        if len(signs) > 1:
            raise SystemExit(
                "icons_squircle: 圆角外环不是同向凸多边形（角弧拼接顺序错了），"
                "拒绝生成会裁掉整块图形的裁切路径"
            )
    if not signs:
        raise SystemExit("icons_squircle: 圆角外环退化（没有有效转角）")


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    ratio = float(sys.argv[1])
    if not 0 < ratio <= 0.5:
        print(f"圆角比例必须在 (0, 0.5]，收到 {ratio}", file=sys.stderr)
        return 2
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{SIZE}" height="{SIZE}">'
           f'<path d="{squircle_path(ratio)}" fill="#fff"/></svg>\n')
    with open(sys.argv[2], "w", encoding="utf-8") as fh:
        fh.write(svg)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
