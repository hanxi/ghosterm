#!/usr/bin/env python3
"""Mutate an actual APK receipt; DEX/native bytes independently remain identical."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / 'scripts/check-android-build-info.py'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('apk', type=Path)
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('build_metadata', CHECK)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    baseline = module.read_metadata(args.apk)
    assert baseline['git_commit'] is not None, 'Fixture: provide a repository-built APK'
    original = hashlib.sha256(args.apk.read_bytes()).hexdigest()
    command = [sys.executable, str(CHECK), '--expect-commit', baseline['git_commit'], '--expect-state', baseline['source_state']]
    def check(path, message=None):
        result = subprocess.run([*command, str(path)], capture_output=True, text=True)
        if message is None:
            assert result.returncode == 0 and 'ANDROID APK SOURCE PASS' in result.stdout, result.stderr
        else:
            assert result.returncode != 0 and message in result.stderr, result.stderr
    check(args.apk)
    with ZipFile(args.apk) as archive:
        payload = {i.filename: archive.read(i) for i in archive.infolist()}
        headers = archive.infolist()
    def hashes(entries):
        return {name: hashlib.sha256(value).hexdigest() for name, value in entries.items()
            if name.endswith(('.dex', '.so')) or name == 'AndroidManifest.xml'}
    control = hashes(payload); assert any(n.endswith('.so') for n in control)
    value = baseline
    variants = [
        ('commit', json.dumps(dict(value, git_commit=('b' if value['git_commit'][0] != 'b' else 'c') * 40)).encode(), 'commit mismatch'),
        ('unknown-state', json.dumps(dict(value, source_state=('dirty' if value['source_state'] == 'unknown' else 'unknown'))).encode(), 'source_state mismatch'),
        ('missing-state', json.dumps({k: v for k, v in value.items() if k != 'source_state'}).encode(), 'schema key set'),
        ('schema', json.dumps(dict(value, schema=2)).encode(), 'schema version'),
        ('missing-commit', json.dumps(dict(value, git_commit=None, source_state='clean')).encode(), 'missing commit'),
        ('duplicate-key', ('{"schema":1,"schema":1,"git_commit":"'+value['git_commit']+'","source_state":"'+value['source_state']+'"}').encode(), 'duplicate metadata key'),
        ('missing-receipt', None, 'exactly one APK source receipt'),
    ]
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='android-build-info-', dir=ROOT / 'tmp') as temporary:
        for name, receipt, message in variants:
            target = Path(temporary) / (name + '.apk')
            with ZipFile(target, 'w') as archive:
                for header in headers:
                    if header.filename == 'assets/ghosterm-build.json':
                        if receipt is not None: archive.writestr(header, receipt)
                    else: archive.writestr(header, payload[header.filename])
            check(target, message)
            with ZipFile(target) as archive:
                assert hashes({n: archive.read(n) for n in control}) == control
            check(args.apk)
            assert hashlib.sha256(args.apk.read_bytes()).hexdigest() == original
            print(name + ': metadata red / same DEX-native-manifest payload green / original green / SHA256')
    print('ANDROID APK SOURCE MUTATIONS PASS: 7 groups (metadata scope; signature and alignment verified separately)')


if __name__ == '__main__':
    main()
