#!/usr/bin/env python3
"""Verify a shared dev Release without treating another platform as stale files."""
import argparse
import hashlib
import json
from pathlib import Path
import re

FILES = {
    'macos': {'Ghosterm-macos-arm64-dev.dmg', 'Ghosterm-macos-arm64-dev.zip',
              'SHA256SUMS', 'BUILD-INFO.txt', 'version.json'},
    'android': {'Ghosterm-android-arm64-dev.apk', 'android-SHA256SUMS', 'android-build-info.json'},
}


def obsolete_macos(names):
    # Only our current/legacy macOS archive namespace. Never delete a Release,
    # an Android asset or an unknown platform's files.
    pattern = re.compile(r'Ghosterm-(?:macos-[A-Za-z0-9._-]+|[0-9][A-Za-z0-9._-]*-(?:arm64|x86_64))\.(?:zip|dmg)')
    return sorted(name for name in names if pattern.fullmatch(name) and name not in FILES['macos'])


def inventory(value):
    if not isinstance(value, dict) or 'id' not in value or not isinstance(value.get('assets'), list):
        raise ValueError('unreadable dev Release inventory')
    if (value['id'] is None and value['assets']) or (value['id'] is not None and type(value['id']) is not int):
        raise ValueError('unknown dev Release identity')
    result = {}
    for asset in value['assets']:
        if not isinstance(asset, dict) or not isinstance(asset.get('name'), str) or asset['name'] in result:
            raise ValueError('invalid or duplicate dev asset')
        if any(key not in asset for key in ('id', 'size', 'digest')):
            raise ValueError('missing dev asset identity')
        if type(asset['id']) is not int or type(asset['size']) is not int or asset['size'] < 0 or not isinstance(asset['digest'], str) or not re.fullmatch(r'sha256:[0-9a-f]{64}', asset['digest']):
            raise ValueError('unknown dev asset identity or bytes')
        result[asset['name']] = asset
    return result


def verify(platform, folder, after, before=None):
    assets = inventory(after)
    own = FILES[platform]
    other = FILES['android' if platform == 'macos' else 'macos']
    foreign = set(assets) & other
    if not own <= set(assets) or (foreign and foreign != other) or set(assets) - own - other:
        raise ValueError('missing, partial or unexpected platform assets')
    for name in own:
        path = folder / name
        if not path.is_file():
            raise ValueError('missing local platform asset')
        digest = 'sha256:' + hashlib.sha256(path.read_bytes()).hexdigest()
        if assets[name]['digest'] != digest or assets[name]['size'] != path.stat().st_size:
            raise ValueError('published platform asset bytes mismatch')
    if before is not None:
        previous = inventory(before)
        if before['id'] is not None and before['id'] != after['id']:
            raise ValueError('shared Release was replaced')
        for name in set(previous) & other:
            if name not in assets or any(previous[name][key] != assets[name][key] for key in ('id', 'digest', 'size')):
                raise ValueError('other platform asset changed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', required=True, choices=FILES)
    parser.add_argument('--dist', type=Path)
    parser.add_argument('--assets-file', required=True, type=Path)
    parser.add_argument('--before-file', type=Path)
    parser.add_argument('--obsolete-macos', action='store_true')
    args = parser.parse_args()
    after = json.loads(args.assets_file.read_text())
    if args.obsolete_macos:
        if args.platform != 'macos':
            raise ValueError('only macOS archive cleanup is supported')
        for name in obsolete_macos(inventory(after)):
            print(name)
        return
    if args.dist is None:
        parser.error('--dist is required for verification')
    verify(args.platform, args.dist, after,
           json.loads(args.before_file.read_text()) if args.before_file else None)
    print('SHARED DEV RELEASE ASSETS PASS:', args.platform)


if __name__ == '__main__':
    main()
