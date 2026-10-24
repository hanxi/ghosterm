#!/usr/bin/env python3
"""从母版生成 Android 资源：adaptive icon XML + 前景 VectorDrawable + 背景色。

用法: icons_android.py <母版.svg> <输出目录> <scale> <tx> <ty>

说明（为什么不是逐像素照搬）：
- VectorDrawable 表达不了 SVG 的 feDropShadow，故前景不含投影（自适应图标本来
  也不该有投影，平台会自己打光）；
- 渐变在 VectorDrawable 需要 API 24+ 的 <aapt:attr> 语法，这里用取自母版渐变的
  **平色近似**，几何仍然逐点来自母版；
- mark 组里**所有** `<path>`/`<rect>`/`<circle>` 按文档顺序逐个转成 VectorDrawable
  路径（渐变引用 → 母版渐变的平色近似），所以母版换造型不必改这里——前景始终
  等于「mark 组去掉投影、颜色降为平色」；
- 缩放/平移由调用方用母版实测外接框算出，保证内容落在 108dp 视口的 66dp 安全区。
"""
import os
import re
import sys

VECTOR_HEAD = '''<?xml version="1.0" encoding="utf-8"?>
<!-- 由 scripts/build-icons.sh 从 assets/icons/ghosterm.svg 派生，勿手改。 -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="108dp"
    android:height="108dp"
    android:viewportWidth="108"
    android:viewportHeight="108">
'''

ADAPTIVE = '''<?xml version="1.0" encoding="utf-8"?>
<!-- 由 scripts/build-icons.sh 从 assets/icons/ghosterm.svg 派生，勿手改。 -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@color/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
</adaptive-icon>
'''

BG_COLOR = '''<?xml version="1.0" encoding="utf-8"?>
<!-- 由 scripts/build-icons.sh 从 assets/icons/ghosterm.svg 派生，勿手改。 -->
<resources>
    <color name="ic_launcher_background">%s</color>
</resources>
'''


def attr(attrs: str, name: str) -> str:
    m = re.search(rf'\b{name}="([^"]*)"', attrs)
    if not m:
        raise ValueError(f"属性 {name} 不存在：{attrs.strip()}")
    return m.group(1)


def has(attrs: str, name: str) -> bool:
    return re.search(rf'\b{name}="', attrs) is not None


def num(attrs: str, name: str) -> float:
    return float(attr(attrs, name))


def rrect(x, y, w, h, r) -> str:
    if r <= 0:
        return f"M{x} {y} H{x + w} V{y + h} H{x} Z"
    return (f"M{x + r} {y} H{x + w - r} A{r} {r} 0 0 1 {x + w} {y + r} "
            f"V{y + h - r} A{r} {r} 0 0 1 {x + w - r} {y + h} "
            f"H{x + r} A{r} {r} 0 0 1 {x} {y + h - r} "
            f"V{y + r} A{r} {r} 0 0 1 {x + r} {y} Z")


def circle(cx, cy, r) -> str:
    return (f"M{cx - r} {cy} a{r} {r} 0 1 0 {2 * r} 0 "
            f"a{r} {r} 0 1 0 {-2 * r} 0 Z")


def to_rgb(hex_color: str):
    h = hex_color.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def flat(stops) -> str:
    """取渐变两端的近似平色（端点均值）。"""
    a, b = to_rgb(stops[0]), to_rgb(stops[-1])
    return "#%02X%02X%02X" % tuple((x + y) // 2 for x, y in zip(a, b))


def gradient_map(defs: str):
    """母版 <defs> 里所有渐变的 {id: [stop-color, ...]}。"""
    out = {}
    pattern = r'<(linear|radial)Gradient id="([\w-]+)".*?</\1Gradient>'
    for m in re.finditer(pattern, defs, re.S):
        out[m.group(2)] = re.findall(r'stop-color="([^"]+)"', m.group(0))
    return out


def resolve(color: str, grads) -> str:
    """把 `url(#id)` 换成母版渐变的平色近似；纯色原样返回。"""
    m = re.fullmatch(r"url\(#([\w-]+)\)", color.strip())
    if not m:
        return color
    stops = grads.get(m.group(1))
    if not stops:
        raise ValueError(f"母版缺少渐变 {m.group(1)}")
    return flat(stops)


def geometry(tag: str, attrs: str) -> str:
    if tag == "path":
        return attr(attrs, "d")
    if tag == "rect":
        rx = num(attrs, "rx") if has(attrs, "rx") else 0.0
        return rrect(num(attrs, "x"), num(attrs, "y"),
                     num(attrs, "width"), num(attrs, "height"), rx)
    return circle(num(attrs, "cx"), num(attrs, "cy"), num(attrs, "r"))


def mark_layers(mark: str, grads):
    layers = []
    for m in re.finditer(r"<(path|rect|circle)\b([^>]*?)/?>", mark, re.S):
        tag, attrs = m.group(1), m.group(2)
        fill = attr(attrs, "fill") if has(attrs, "fill") else None
        stroke = attr(attrs, "stroke") if has(attrs, "stroke") else None
        d = geometry(tag, attrs)
        if stroke and fill in (None, "none"):
            width = attr(attrs, "stroke-width") if has(attrs, "stroke-width") else "1"
            layers.append(
                f'        <path android:strokeColor="{resolve(stroke, grads)}"'
                f' android:strokeWidth="{width}" android:strokeLineCap="round"'
                f' android:strokeLineJoin="round" android:pathData="{d}"/>')
        else:
            color = resolve(fill if fill and fill != "none" else "#000000", grads)
            layers.append(f'        <path android:fillColor="{color}" android:pathData="{d}"/>')
    return layers


def main() -> int:
    if len(sys.argv) != 6:
        print(__doc__, file=sys.stderr)
        return 2
    src, outdir, scale, tx, ty = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
    text = open(src, encoding="utf-8").read()
    defs = re.search(r"<defs>.*?</defs>", text, re.S).group(0)
    m = re.search(r"<!-- BEGIN:mark -->\n(.*?)<!-- END:mark -->", text, re.S)
    if not m:
        raise SystemExit("母版缺少 <!-- BEGIN:mark --> / <!-- END:mark --> 标记")
    mark = m.group(1)
    grads = gradient_map(defs)

    layers = mark_layers(mark, grads)
    if not layers:
        raise SystemExit("母版 mark 组里没有 <path>/<rect>/<circle>，无法生成前景")

    vector = (VECTOR_HEAD
              + f'    <group android:scaleX="{scale}" android:scaleY="{scale}"'
                f' android:translateX="{tx}" android:translateY="{ty}">\n'
              + "\n".join(layers) + "\n    </group>\n</vector>\n")

    for sub in ("drawable", "mipmap-anydpi-v26", "values"):
        os.makedirs(f"{outdir}/{sub}", exist_ok=True)
    with open(f"{outdir}/drawable/ic_launcher_foreground.xml", "w", encoding="utf-8") as fh:
        fh.write(vector)
    for name in ("ic_launcher", "ic_launcher_round"):
        with open(f"{outdir}/mipmap-anydpi-v26/{name}.xml", "w", encoding="utf-8") as fh:
            fh.write(ADAPTIVE)
    bg = grads.get("gt-bg")
    if not bg:
        raise SystemExit("母版缺少 gt-bg 渐变，无法生成 adaptive 背景色")
    with open(f"{outdir}/values/ic_launcher_background.xml", "w", encoding="utf-8") as fh:
        fh.write(BG_COLOR % bg[-1])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
