#!/usr/bin/env python3
"""Generate both channels' update metadata from the actual distribution ZIP.

The optional private update key is sent only to Sparkle's signer over stdin.
No signing material is written to disk or included in metadata/logs.
"""
import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import subprocess
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parent.parent
NS = 'http://www.andymatuschak.org/xml-namespaces/sparkle'
ET.register_namespace('sparkle', NS)


def metadata(dist, channel, signer, commit):
    archives = list(dist.glob('*.zip'))
    if len(archives) != 1:
        raise ValueError('expected exactly one update ZIP')
    archive = archives[0]
    with zipfile.ZipFile(archive) as zipped:
        info = plistlib.loads(zipped.read('Ghosterm.app/Contents/Info.plist'))
        receipt = zipped.read('Ghosterm.app/Contents/Resources/BUILD-INFO.txt').decode('utf-8')
    def field(name):
        match = re.search(r'^' + name + r':\s*(\S+)', receipt, re.M)
        if not match:
            raise ValueError('missing build receipt field: ' + name)
        return match[1]
    source = re.search(r'^源码:\s*git ([0-9a-f]{7,40})\s*$', receipt, re.M)
    if (not source or not re.fullmatch(r'[0-9a-f]{40}', commit) or not commit.startswith(source[1])
            or (channel == 'release' and source[1] != commit)):
        raise ValueError('source receipt is dirty, missing or does not match the commit')
    if field('渠道') != channel or info['GhostermUpdateChannel'] != channel:
        raise ValueError('build channel mismatch')
    version = info['CFBundleShortVersionString']
    if channel == 'release' and not re.fullmatch(r'[0-9]+\.[0-9]+\.[0-9]+', version):
        raise ValueError('release updates require a stable semantic version')
    stamp = field('构建时刻')
    if not re.fullmatch(r'\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z', stamp):
        raise ValueError('invalid build timestamp')
    sums = {}
    for line in (dist / 'SHA256SUMS').read_text().splitlines():
        digest, name = line.split(None, 1)
        if not re.fullmatch('[0-9a-f]{64}', digest) or Path(name).name != name:
            raise ValueError('invalid archive checksum record')
        if hashlib.sha256((dist / name).read_bytes()).hexdigest() != digest:
            raise ValueError('distribution checksum mismatch')
        sums[name] = digest
    dmgs = [name for name in sums if name.endswith('.dmg')]
    if len(dmgs) != 1 or archive.name not in sums:
        raise ValueError('missing distribution assets')
    base = 'https://github.com/hanxi/ghosterm/releases/download/' + ('dev' if channel == 'dev' else 'v' + version)
    key = os.environ.get('GHOSTERM_UPDATE_PRIVATE_KEY', '')
    public = info.get('SUPublicEDKey', '')
    if bool(key) != bool(public):
        raise ValueError('update public/private keys must both be configured; refusing an unsigned update')
    signature = None
    rss = ET.Element('rss', {'version': '2.0'})
    feed = ET.SubElement(rss, 'channel')
    ET.SubElement(feed, 'title').text = 'Ghosterm ' + channel
    if key:
        # Capture the signer's output even on failure: upstream diagnostics can
        # include malformed key input. Never forward these diagnostics to logs.
        env = {k: v for k, v in os.environ.items() if k != 'GHOSTERM_UPDATE_PRIVATE_KEY'}
        (ROOT / 'tmp').mkdir(exist_ok=True)
        env['TMPDIR'] = str(ROOT / 'tmp')
        def sign(path, only_signature=False):
            command = [str(signer), '--ed-key-file', '-']
            if only_signature:
                command.append('-p')
            result = subprocess.run(command + [str(path)], input=key + '\n', text=True,
                                    capture_output=True, env=env)
            if result.returncode:
                raise ValueError('Sparkle signing failed; check update signing configuration')
            return result.stdout.strip()
        signature = sign(archive, True)
        if len(base64.b64decode(signature, validate=True)) != 64:
            raise ValueError('invalid archive signature')
        # Verify with the PUBLIC key from the ZIP, independently of the signer.
        subprocess.run(['swift', str(ROOT / 'scripts/verify-macos-update.swift'),
                        public, signature, str(archive)], check=True, env=env,
                       stdout=subprocess.DEVNULL)
        item = ET.SubElement(feed, 'item')
        ET.SubElement(item, 'title').text = version
        ET.SubElement(item, '{' + NS + '}version').text = info['CFBundleVersion']
        ET.SubElement(item, '{' + NS + '}shortVersionString').text = version
        ET.SubElement(item, '{' + NS + '}minimumSystemVersion').text = info['LSMinimumSystemVersion']
        ET.SubElement(item, 'enclosure', {'url': base + '/' + archive.name,
            'length': str(archive.stat().st_size), 'type': 'application/octet-stream',
            '{' + NS + '}edSignature': signature})
    feed_path = dist / 'appcast.xml'
    ET.ElementTree(rss).write(feed_path, encoding='utf-8', xml_declaration=True)
    if key:
        sign(feed_path)
    else:
        print('Update signing is not configured: empty appcast; manual downloads remain available')
    manifest = {'channel': channel, 'version': version, 'build': int(re.search(r'\(build (\d+)\)', receipt)[1]),
        'git_commit': commit, 'build_time': stamp, 'arch': field('架构'),
        'min_macos': info['LSMinimumSystemVersion'], 'signature': field('签名'),
        'notarized': field('签名') == 'id' and not field('公证').startswith('无'),
        'update_version': info['CFBundleVersion'], 'update_signature': signature,
        'assets': {'dmg': dmgs[0], 'zip': archive.name, 'checksums': 'SHA256SUMS', 'build_info': 'BUILD-INFO.txt'},
        'sha256': sums, 'download_base': base}
    (dist / 'version.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
    print('Generated update metadata:', channel, version, commit[:12], 'signed' if signature else 'unsigned')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist', type=Path, required=True)
    parser.add_argument('--channel', choices=['dev', 'release'], required=True)
    parser.add_argument('--signer', type=Path, required=True)
    parser.add_argument('--commit', required=True)
    args = parser.parse_args()
    metadata(args.dist.resolve(), args.channel, args.signer.resolve(), args.commit)
