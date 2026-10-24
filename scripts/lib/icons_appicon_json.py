#!/usr/bin/env python3
"""生成 Apple AppIcon.appiconset 的 Contents.json。

用法: icons_appicon_json.py <appiconset 目录> <mac 条目...> -- <ios 条目...>
条目形如 "1024 2x 512x512" = 像素边长 缩放 idiom尺寸。
"""
import json
import sys


def entries(specs, prefix, idiom_for):
    out = []
    for spec in specs:
        px, scale, size = spec.split()
        out.append({
            "filename": f"{prefix}-{px}.png",
            "idiom": idiom_for(size),
            "scale": scale,
            "size": size,
        })
    return out


def main() -> int:
    if "--" not in sys.argv or len(sys.argv) < 4:
        print(__doc__, file=sys.stderr)
        return 2
    cut = sys.argv.index("--")
    outdir = sys.argv[1]
    mac = sys.argv[2:cut]
    ios = sys.argv[cut + 1:]

    images = entries(mac, "mac", lambda _size: "mac")
    images += entries(ios, "ios", lambda size: {
        "76x76": "ipad", "83.5x83.5": "ipad", "1024x1024": "ios-marketing",
    }.get(size, "iphone"))

    payload = {
        "images": images,
        "info": {"author": "xcode", "version": 1},
    }
    with open(f"{outdir}/Contents.json", "w", encoding="utf-8") as fh:
        json.dump(payload, fh, indent=2, ensure_ascii=False, sort_keys=False)
        fh.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
