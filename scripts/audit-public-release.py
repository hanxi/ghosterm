#!/usr/bin/env python3
"""Read-only audit of a GitHub Release's published platform assets."""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
import plistlib
import re
import subprocess
import tempfile
import urllib.request
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('release_assets', ROOT / 'scripts/check-dev-release-assets.py')
CHECK = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CHECK)
ANDROID_SPEC = importlib.util.spec_from_file_location('android_release_assets', ROOT / 'scripts/check-android-release-assets.py')
ANDROID = importlib.util.module_from_spec(ANDROID_SPEC)
ANDROID_SPEC.loader.exec_module(ANDROID)
FORMAL_MACOS = {'Ghosterm-macos-arm64-release.dmg', 'Ghosterm-macos-arm64-release.zip',
                'SHA256SUMS', 'BUILD-INFO.txt', 'version.json', 'appcast.xml'}
FORMAL_ANDROID = {ANDROID.FORMAL_APK, 'android-SHA256SUMS', 'android-build-info.json'}
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


def verify_update_signatures(folder, archive, archive_signature, public_key):
    key = base64.b64decode(public_key, validate=True)
    if len(key) != 32:
        raise ValueError('invalid Ed25519 public key in update ZIP')
    signed_feed = (folder / 'appcast.xml').read_bytes()
    trailer = re.search(rb'<!-- sparkle-signatures:\nedSignature: ([A-Za-z0-9+/=]+)\nlength: ([0-9]+)\n-->\n\Z', signed_feed)
    if trailer is None or int(trailer[2]) != trailer.start():
        raise ValueError('invalid signed appcast trailer')
    with tempfile.TemporaryDirectory(prefix='signatures-', dir=folder) as scratch:
        scratch = Path(scratch)
        key_file = scratch / 'public.der'
        # RFC 8410 SubjectPublicKeyInfo for a raw Ed25519 key.
        key_file.write_bytes(bytes.fromhex('302a300506032b6570032100') + key)
        feed = scratch / 'feed.xml'
        feed.write_bytes(signed_feed[:trailer.start()])
        signature_file = scratch / 'signature'
        for asset, encoded in ((archive, archive_signature), (feed, trailer[1])):
            signature = base64.b64decode(encoded, validate=True)
            if len(signature) != 64:
                raise ValueError('invalid Ed25519 signature length: ' + asset.name)
            signature_file.write_bytes(signature)
            result = subprocess.run(['openssl', 'pkeyutl', '-verify', '-pubin', '-keyform', 'DER',
                                     '-inkey', str(key_file), '-rawin', '-sigfile', str(signature_file),
                                     '-in', str(asset)], capture_output=True, text=True, timeout=60)
            if result.returncode:
                raise ValueError('Ed25519 verification failed for ' + asset.name + ': ' + result.stderr.strip())


def verify_formal_macos(folder, assets, version, commit, base, expected_key=None, aapt=None,
                        build_tools=None, certificate_sha256=None):
    inventory = CHECK.inventory(assets)
    if set(inventory) not in (FORMAL_MACOS, FORMAL_MACOS | FORMAL_ANDROID):
        raise ValueError('unexpected formal release asset inventory')
    for name, asset in inventory.items():
        path = folder / name
        with path.open('rb') as stream:
            digest = 'sha256:' + hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != asset['digest'] or path.stat().st_size != asset['size']:
            raise ValueError('published asset bytes mismatch: ' + name)
    manifest = json.loads((folder / 'version.json').read_text())
    if (manifest.get('channel'), manifest.get('version'), manifest.get('git_commit')) != ('release', version, commit):
        raise ValueError('manifest does not match the formal release tag')
    verify_macos_update(folder, inventory, manifest, 'release', base, expected_key)
    if FORMAL_ANDROID <= inventory.keys():
        ANDROID.verify(folder, commit, version, aapt, build_tools, certificate_sha256)


def verify_macos_update(folder, inventory, manifest, channel, base, expected_key):
    version, commit = manifest.get('version'), manifest.get('git_commit')
    if (manifest.get('channel') != channel or not isinstance(version, str) or not version
            or not isinstance(commit, str) or not re.fullmatch(r'[0-9a-f]{40}', commit)):
        raise ValueError('invalid macOS update manifest identity')
    if not isinstance(manifest.get('update_signature'), str) or not manifest['update_signature']:
        raise ValueError('manifest is missing the update signature')
    zip_name = f'Ghosterm-macos-arm64-{channel}.zip'
    with zipfile.ZipFile(folder / zip_name) as archive:
        info = plistlib.loads(archive.read('Ghosterm.app/Contents/Info.plist'))
        receipt = archive.read('Ghosterm.app/Contents/Resources/BUILD-INFO.txt')
    if receipt != (folder / 'BUILD-INFO.txt').read_bytes():
        raise ValueError('published build receipt differs from the receipt inside the update ZIP')
    receipt_text = receipt.decode('utf-8')
    source = re.search(r'^源码:[ \t]*git ([0-9a-f]{7,40})[ \t]*$', receipt_text, re.M)
    if source is None or not commit.startswith(source[1]) or (channel == 'release' and source[1] != commit):
        raise ValueError('update ZIP has a dirty or mismatched source receipt')
    signature = re.findall(r'^签名:[ \t]*(adhoc|dev|id)(?=[ \t]|$)[^\n]*$', receipt_text, re.M)
    notarization = re.findall(r'^公证:[ \t]*([^\s][^\n]*)$', receipt_text, re.M)
    if len(signature) != 1 or len(notarization) != 1:
        raise ValueError('update ZIP is missing unique Apple signing and notarization receipts')
    notarized = signature[0] == 'id' and not notarization[0].startswith('无')
    if manifest.get('signature') != signature[0] or manifest.get('notarized') is not notarized:
        raise ValueError('Apple signing or notarization differs from the build receipt')
    for field in ('update_version', 'min_macos'):
        if not isinstance(manifest.get(field), str) or not manifest[field]:
            raise ValueError('missing application metadata: ' + field)
    if (info.get('GhostermUpdateChannel'), info.get('CFBundleShortVersionString'),
        info.get('CFBundleVersion'), info.get('LSMinimumSystemVersion')) != (
        channel, version, manifest.get('update_version'), manifest.get('min_macos')
    ):
        raise ValueError('application metadata differs from the published release manifest')
    items = ET.parse(folder / 'appcast.xml').findall('./channel/item')
    if len(items) != 1 or manifest.get('download_base') != base:
        raise ValueError('appcast must describe one update at the channel download URL')
    item = items[0]
    enclosure = item.find('enclosure')
    if enclosure is None or (
        item.findtext(SPARKLE + 'shortVersionString'), item.findtext(SPARKLE + 'version'),
        item.findtext(SPARKLE + 'minimumSystemVersion'),
        enclosure.get('url'), enclosure.get('length'), enclosure.get(SPARKLE + 'edSignature')
    ) != (
        version, manifest.get('update_version'), manifest.get('min_macos'), base + '/' + zip_name,
        str(inventory[zip_name]['size']), manifest['update_signature']
    ):
        raise ValueError('appcast does not match the published update ZIP')
    platform_assets = FORMAL_MACOS if channel == 'release' else CHECK.FILES['macos']
    expected = platform_assets - {'SHA256SUMS', 'version.json', 'appcast.xml'}
    rows = [line.split(None, 1) for line in (folder / 'SHA256SUMS').read_text().splitlines()]
    if len(rows) != len(expected) or any(len(row) != 2 for row in rows):
        raise ValueError('invalid macOS release checksum records')
    checksums = {name: digest for digest, name in rows}
    if set(checksums) != expected or checksums != manifest.get('sha256') or any(
        'sha256:' + digest != inventory[name]['digest'] for name, digest in checksums.items()
    ):
        raise ValueError('macOS release checksums do not match the assets')
    key = info.get('SUPublicEDKey')
    if not isinstance(key, str) or (expected_key is not None and key != expected_key):
        raise ValueError('update ZIP does not contain the expected Ed25519 public key')
    verify_update_signatures(folder, folder / zip_name, manifest['update_signature'], key)


def local_assets(folder, names):
    entries = []
    for index, path in enumerate(sorted(folder.iterdir()), 1):
        if path.is_symlink() or not path.is_file() or path.name not in names:
            raise ValueError('unexpected macOS asset: ' + path.name)
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        entries.append(dict(id=index, name=path.name, size=path.stat().st_size, digest='sha256:' + digest))
    return {'id': 1, 'assets': entries}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', default='hanxi/ghosterm')
    parser.add_argument('--tag', default='dev')
    parser.add_argument('--platform', choices=sorted(CHECK.FILES), default='macos')
    parser.add_argument('--update-public-key', help='require this trusted Sparkle public key for a macOS release')
    parser.add_argument('--asset-directory', type=Path, help='verify a local macOS artifact before upload')
    parser.add_argument('--commit', help='expected full commit for a local artifact')
    tools = parser.add_mutually_exclusive_group()
    tools.add_argument('--aapt', help='also verify the published formal APK manifest using Android SDK aapt')
    tools.add_argument('--android-build-tools', type=Path,
                       help='verify the formal APK signature, alignment and manifest using this Build Tools directory')
    parser.add_argument('--android-certificate-sha256', help='expected APK signer certificate SHA-256 in lowercase hex')
    args = parser.parse_args()
    formal = re.fullmatch(r'v([0-9]+\.[0-9]+\.[0-9]+)', args.tag)
    if args.tag != 'dev' and (not formal or args.platform != 'macos'):
        parser.error('expected dev or a stable macOS tag vX.Y.Z')
    if args.update_public_key is not None and args.platform != 'macos':
        parser.error('--update-public-key requires macOS')
    if (args.aapt or args.android_build_tools) and (not formal or args.asset_directory is not None):
        parser.error('Android Build Tools verification requires a public formal release audit')
    if args.android_certificate_sha256 is not None and (
            not args.android_build_tools or not re.fullmatch(r'[0-9a-f]{64}', args.android_certificate_sha256)):
        parser.error('--android-certificate-sha256 requires --android-build-tools and 64 lowercase hex digits')
    base = f'https://github.com/{args.repository}/releases/download/{args.tag}'
    if args.asset_directory is not None:
        if not (args.platform == 'macos' and args.update_public_key and re.fullmatch(r'[0-9a-f]{40}', args.commit or '')):
            parser.error('local verification requires macOS, a trusted update key and a full commit')
        folder = args.asset_directory.resolve(strict=True)
        if formal:
            verify_formal_macos(folder, local_assets(folder, FORMAL_MACOS), formal[1], args.commit, base, args.update_public_key)
        else:
            assets = local_assets(folder, CHECK.FILES['macos'])
            CHECK.verify('macos', folder, assets)
            manifest = json.loads((folder / 'version.json').read_text())
            if manifest.get('git_commit') != args.commit:
                raise ValueError('dev manifest does not match the expected commit')
            verify_macos_update(folder, CHECK.inventory(assets), manifest, 'dev', base, args.update_public_key)
        print(f'LOCAL RELEASE PASS: {args.repository}@{args.tag} ({args.commit})')
        return
    if args.commit is not None:
        parser.error('--commit requires --asset-directory')
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
    inventory = CHECK.inventory(assets)
    names = CHECK.FILES[args.platform]
    if formal:
        names = FORMAL_MACOS | FORMAL_ANDROID
    if not names <= inventory.keys():
        raise ValueError('missing published assets: ' + ', '.join(sorted(names - inventory.keys())))
    if formal and inventory.keys() != names:
        raise ValueError('unexpected published assets: ' + ', '.join(sorted(inventory.keys() - names)))
    with tempfile.TemporaryDirectory(prefix='ghosterm-release-audit-', dir=scratch) as value:
        folder = Path(value)
        for name in sorted(names):
            download_asset(base + '/' + name, folder / name, inventory[name]['size'])
        if formal:
            verify_formal_macos(folder, assets, formal[1], commit, base, args.update_public_key,
                                args.aapt, args.android_build_tools, args.android_certificate_sha256)
        else:
            CHECK.verify(args.platform, folder, assets)
            if args.platform == 'macos':
                manifest = json.loads((folder / 'version.json').read_text())
                verify_macos_update(folder, inventory, manifest, 'dev', base, args.update_public_key)
    print(f'PUBLIC RELEASE PASS: {args.repository}@{args.tag} ({args.platform})')


if __name__ == '__main__':
    main()
