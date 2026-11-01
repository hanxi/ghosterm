#!/usr/bin/env python3
"""Corrupt actual Release assets; independent APK code/source controls stay green."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('release_assets', ROOT / 'scripts/check-android-release-assets.py')
check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(check)


def payload(apk):
    with ZipFile(apk) as archive:
        return {name: hashlib.sha256(archive.read(name)).hexdigest() for name in archive.namelist()
                if name.endswith(('.dex', '.so')) or name == 'AndroidManifest.xml'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--expect-commit', required=True)
    args = parser.parse_args()
    baseline = check.verify(args.folder, args.expect_commit)
    original = {name: (args.folder / name).read_bytes() for name in
                (check.APK, 'android-SHA256SUMS', 'android-build-info.json')}
    code = payload(args.folder / check.APK)
    assert code and any(name.endswith('.so') for name in code), 'Fixture: actual native APK required'
    info = json.loads(original['android-build-info.json'])
    variants = [
        ('apk-bytes', check.APK, original[check.APK] + b'\0', 'published APK checksum mismatch', args.expect_commit),
        ('checksum', 'android-SHA256SUMS', b'0' * 64 + ('  ' + check.APK + '\n').encode(), 'published APK checksum mismatch', args.expect_commit),
        ('metadata-sha', 'android-build-info.json', json.dumps(dict(info, sha256='0' * 64)).encode(), 'published build info mismatch', args.expect_commit),
        ('metadata-bytes', 'android-build-info.json', json.dumps(dict(info, bytes=info['bytes'] + 1)).encode(), 'published build info mismatch', args.expect_commit),
        ('metadata-commit', 'android-build-info.json', json.dumps(dict(info, git_commit='0' * 40)).encode(), 'published build info mismatch', args.expect_commit),
        ('metadata-state', 'android-build-info.json', json.dumps(dict(info, source_state='dirty')).encode(), 'published build info mismatch', args.expect_commit),
        ('missing-asset', 'android-build-info.json', None, 'missing Android Release asset', args.expect_commit),
        ('duplicate-key', 'android-build-info.json', ('{"schema":1,' + original['android-build-info.json'].decode().lstrip()[1:]).encode(), 'duplicate metadata key', args.expect_commit),
        ('expected-commit', 'android-build-info.json', original['android-build-info.json'], 'published APK source mismatch', '0' * 40),
        ('release-kind', 'android-build-info.json', json.dumps(dict(info, build_type='debug')).encode(), 'published build info mismatch', args.expect_commit),
    ]
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='android-release-assets-', dir=ROOT / 'tmp') as temporary:
        for name, target, value, message, commit in variants:
            folder = Path(temporary) / name
            folder.mkdir()
            for filename, data in original.items():
                (folder / filename).write_bytes(data)
            if value is None:
                (folder / target).unlink()
            else:
                (folder / target).write_bytes(value)
            try:
                check.verify(folder, commit)
            except ValueError as error:
                assert message in str(error), str(error)
            else:
                raise AssertionError(name + ': expected specific integrity rejection')
            assert payload(folder / check.APK) == code
            assert check.source.read_metadata(folder / check.APK)['git_commit'] == args.expect_commit
            assert check.verify(args.folder, args.expect_commit) == baseline
            assert all((args.folder / filename).read_bytes() == data for filename, data in original.items())
            print(name + ': target red / same APK code-source green / original green / SHA256')
    print('ANDROID RELEASE ASSET MUTATIONS PASS: 10 groups')


if __name__ == '__main__':
    main()
