#!/usr/bin/env python3
"""Build a local side-by-side review from real-device manifests and HTML captures."""
import argparse
import html
import json
import os
from pathlib import Path
import struct

ROOT = Path(__file__).resolve().parents[1]


def dimensions(path):
    data = path.read_bytes()
    if data[:8] != b'\x89PNG\r\n\x1a\n':
        raise ValueError(f'Not a PNG: {path}')
    return struct.unpack('>II', data[16:24])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', type=Path, required=True)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / 'tmp/android-design/review.html')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    panels = []
    names = None
    for theme in ('light', 'dark'):
        manifest = json.loads((args.captures / f'{theme}-manifest.json').read_text())
        current = [item['name'] for item in manifest]
        if len(current) != len(set(current)) or not current:
            raise ValueError('Empty or duplicate captures')
        if names is not None and current != names:
            raise ValueError('Light and dark states differ')
        names = current
        for item in manifest:
            native = args.captures / item['file']
            if dimensions(native) != (item['width'], item['height']):
                raise ValueError(f'Capture receipt does not match PNG: {native}')
            reference = args.reference / item['file']
            def img(path, caption):
                link = html.escape(os.path.relpath(path.resolve(), args.output.parent.resolve()), quote=True)
                return f'<figure><figcaption>{caption}</figcaption><a href="{link}"><img src="{link}" loading="lazy" alt="{html.escape(item["name"])} {caption}"></a></figure>'
            pair = ''
            if reference.exists():
                if dimensions(reference) != dimensions(native):
                    raise ValueError('Reference and device dimensions differ')
                pair += img(reference, 'HTML 参考')
            else:
                pair += '<figure class="unmatched">原型无此独立状态。审核原生页面的一致性与实际数据。</figure>'
            pair += img(native, 'Android 实际截图')
            receipt = html.escape(json.dumps(item, ensure_ascii=False, indent=2))
            panels.append(f'<section data-theme="{theme}" data-name="{html.escape(item["name"])}"><h2>{theme} · {html.escape(item["name"])}</h2><div class="pair">{pair}</div><details><summary>状态收据</summary><pre>{receipt}</pre></details></section>')
    args.output.write_text('''<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Ghosterm Android 视觉审核</title>
<style>body{font:14px/1.6 system-ui;margin:0;background:#edf0f7;color:#232c3b}header{position:sticky;top:0;background:#fff;padding:14px 24px;z-index:1;border-bottom:1px solid #d5dce9}h1{font-size:20px;margin:0}header p{max-width:1000px;margin:8px 0}select,input{font:inherit;padding:8px;margin-right:10px}main{padding:20px;display:grid;gap:24px;grid-template-columns:repeat(auto-fit,minmax(min(100%,800px),1fr))}section{background:white;border-radius:18px;padding:16px}h2{font-size:16px}.pair{display:flex;gap:12px;align-items:flex-start}figure{margin:0;width:50%}figcaption{padding:5px 0;color:#626e82}img{width:100%;max-width:388px;display:block;border:1px solid #d5dce9}pre{white-space:pre-wrap}.unmatched{padding-top:36px;color:#626e82}button{font:inherit;padding:8px}details{margin-top:12px}</style>
<header><h1>Ghosterm Android · HTML 与实际运行截图</h1><p>基准尺寸 388×844，具体 API、页大小、密度与字号见状态收据。系统状态栏不作像素一致要求。原型采用演示数据；Android 的连接、文件、会话和安全信息来自真实控制器。点击图片查看原尺寸。</p><p>已知适配：首页保留本机文件入口；没有会话时禁用“回到终端”；安全页展示真实记录；确认操作使用原生对话框；主机与配置的长表单采用完整页面；系统键盘由 Android 提供。功能通过不代表逐像素一致。</p>
<select id="theme"><option value="">所有主题</option><option>light</option><option>dark</option></select><input id="filter" placeholder="筛选页面，如 files、settings"><span id="count"></span></header><main>'''+''.join(panels)+'''</main><script>const theme=document.querySelector('#theme'),filter=document.querySelector('#filter');function apply(){let n=0;document.querySelectorAll('section').forEach(s=>{s.hidden=!!(theme.value&&theme.value!==s.dataset.theme||!s.dataset.name.includes(filter.value));if(!s.hidden)n++});document.querySelector('#count').textContent=n+' 个状态'}theme.onchange=filter.oninput=apply;apply();</script></html>''')
    print(f'Wrote {len(panels)} verified capture panels to {args.output}')


if __name__ == '__main__':
    main()
