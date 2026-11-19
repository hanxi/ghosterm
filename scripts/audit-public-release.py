#!/usr/bin/env python3
"""Read-only audit of a GitHub Release's published platform assets."""
import argparse
import importlib.util
import json
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('release_assets', ROOT / 'scripts/check-dev-release-assets.py')
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default='hanxi/ghosterm')
    parser.add_argument('--tag', default='dev')
    parser.add_argument('--platform', choices=sorted(CHECK.FILES), default='macos')
    args = parser.parse_args()
    api = f'https://api.github.com/repos/{args.repository}/releases/tags/{args.tag}'
    with urllib.request.urlopen(api, timeout=30) as response:
        release = json.load(response)
    assets = {
        'id': release.get('id'),
        'assets': [
            {'id': item.get('id'), 'name': item.get('name'), 'size': item.get('size'), 'digest': item.get('digest')}
            for item in release.get('assets', [])
        ],
    }
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='ghosterm-release-audit-', dir=ROOT / 'tmp') as value:
        folder = Path(value)
        base = f'https://github.com/{args.repository}/releases/download/{args.tag}/'
        for name in CHECK.FILES[args.platform]:
            with urllib.request.urlopen(base + name, timeout=120) as response:
                (folder / name).write_bytes(response.read())
        CHECK.verify(args.platform, folder, assets)
    print(f'PUBLIC RELEASE PASS: {args.repository}@{args.tag} ({args.platform})')


if __name__ == '__main__':
    main()
