#!/usr/bin/env python3
"""Read-only audit of a GitHub Release's published platform assets."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('release_assets', ROOT / 'scripts/check-dev-release-assets.py')
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)
FORMAL_MACOS = {'Ghosterm-macos-arm64-release.dmg', 'Ghosterm-macos-arm64-release.zip',
                'SHA256SUMS', 'BUILD-INFO.txt', 'version.json', 'appcast.xml'}
SPARKLE = '{http://www.andymatuschak.org/xml-namespaces/sparkle}'


def download_asset(url, path, size):
    with urllib.request.urlopen(url, timeout=120) as response, path.open('xb') as output:
        remaining = size
        while remaining:
            block = response.read(min(remaining, 1024 * 1024))
            if not block:
                raise ValueError('download ended before published asset size: ' + path.name)
            output.write(block)
            remaining -= len(block)
        if response.read(1):
            raise ValueError('download exceeds published asset size: ' + path.name)


def verify_formal_macos(folder, assets, version, commit, base):
    inventory = CHECK.inventory(assets)
    if set(inventory) != FORMAL_MACOS:
        raise ValueError('unexpected formal macOS asset inventory')
    for name, asset in inventory.items():
        path = folder / name
        with path.open('rb') as stream:
            digest = 'sha256:' + hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != asset['digest'] or path.stat().st_size != asset['size']:
            raise ValueError('published asset bytes mismatch: ' + name)
    manifest = json.loads((folder / 'version.json').read_text())
    if (manifest.get('channel'), manifest.get('version'), manifest.get('git_commit')) != ('release', version, commit):
        raise ValueError('manifest does not match the formal release tag')
    if not isinstance(manifest.get('update_signature'), str) or not manifest['update_signature']:
        raise ValueError('manifest is missing the update signature')
    zip_name = 'Ghosterm-macos-arm64-release.zip'
    items = ET.parse(folder / 'appcast.xml').findall('./channel/item')
    if len(items) != 1 or manifest.get('download_base') != base:
        raise ValueError('formal appcast must describe one release at the tag URL')
    item = items[0]
    enclosure = item.find('enclosure')
    if enclosure is None or (
        item.findtext(SPARKLE + 'shortVersionString'), item.findtext(SPARKLE + 'version'),
        enclosure.get('url'), enclosure.get('length'), enclosure.get(SPARKLE + 'edSignature')
    ) != (
        version, manifest.get('update_version'), base + '/' + zip_name,
        str(inventory[zip_name]['size']), manifest['update_signature']
    ):
        raise ValueError('formal appcast does not match the published update ZIP')
    expected = FORMAL_MACOS - {'SHA256SUMS', 'version.json', 'appcast.xml'}
    rows = [line.split(None, 1) for line in (folder / 'SHA256SUMS').read_text().splitlines()]
    if len(rows) != len(expected) or any(len(row) != 2 for row in rows):
        raise ValueError('invalid formal release checksum records')
    checksums = {name: digest for digest, name in rows}
    if set(checksums) != expected or checksums != manifest.get('sha256') or any(
        'sha256:' + digest != inventory[name]['digest'] for name, digest in checksums.items()
    ):
        raise ValueError('formal release checksums do not match the assets')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default='hanxi/ghosterm')
    parser.add_argument('--tag', default='dev')
    parser.add_argument('--platform', choices=sorted(CHECK.FILES), default='macos')
    args = parser.parse_args()
    formal = re.fullmatch(r'v([0-9]+\.[0-9]+\.[0-9]+)', args.tag)
    if args.tag != 'dev' and (not formal or args.platform != 'macos'):
        parser.error('expected dev or a stable macOS tag vX.Y.Z')
    api = f'https://api.github.com/repos/{args.repository}/releases/tags/{args.tag}'
    with urllib.request.urlopen(api, timeout=30) as response:
        release = json.load(response)
    if release.get('tag_name') != args.tag or release.get('draft') is not False:
        raise ValueError('release tag is missing or unpublished')
    if formal:
        if release.get('prerelease') is not False:
            raise ValueError('expected a stable release')
        with urllib.request.urlopen(f'https://api.github.com/repos/{args.repository}/commits/refs/tags/{args.tag}', timeout=30) as response:
            commit = json.load(response)['sha']
        if not re.fullmatch(r'[0-9a-f]{40}', commit):
            raise ValueError('invalid release tag commit')
    assets = {
        'id': release.get('id'),
        'assets': [
            {'id': item.get('id'), 'name': item.get('name'), 'size': item.get('size'), 'digest': item.get('digest')}
            for item in release.get('assets', [])
        ],
    }
    project_tmp = (ROOT / 'tmp').resolve()
    project_tmp.mkdir(exist_ok=True)
    requested_tmp = Path(os.environ.get('TMPDIR', project_tmp)).resolve()
    scratch = requested_tmp if requested_tmp.is_dir() and requested_tmp.is_relative_to(project_tmp) else project_tmp
    names = FORMAL_MACOS if formal else CHECK.FILES[args.platform]
    inventory = CHECK.inventory(assets)
    if not names <= inventory.keys():
        raise ValueError('missing published assets: ' + ', '.join(sorted(names - inventory.keys())))
    with tempfile.TemporaryDirectory(prefix='ghosterm-release-audit-', dir=scratch) as value:
        folder = Path(value)
        base = f'https://github.com/{args.repository}/releases/download/{args.tag}'
        for name in sorted(names):
            download_asset(base + '/' + name, folder / name, inventory[name]['size'])
        if formal:
            verify_formal_macos(folder, assets, formal[1], commit, base)
        else:
            CHECK.verify(args.platform, folder, assets)
    print(f'PUBLIC RELEASE PASS: {args.repository}@{args.tag} ({args.platform})')


if __name__ == '__main__':
    main()
