#!/usr/bin/env python3
"""Verify the three downloaded Android Release assets agree with this CI commit.

This checks publication integrity and source identity. Signing, R8, resource
shrinking and native alignment are verified by the build workflow separately.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
APK = 'Ghosterm-android-arm64-dev.apk'
FORMAL_APK = 'Ghosterm-android-arm64-release.apk'
spec = importlib.util.spec_from_file_location('apk_source', ROOT / 'scripts/check-android-build-info.py')
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


def verify(folder, commit, apk_name=APK):
    apk = folder / apk_name
    if not all((folder / name).is_file() for name in (apk_name, 'android-SHA256SUMS', 'android-build-info.json')):
        raise ValueError('missing Android Release asset')
    receipt = source.read_metadata(apk)
    if receipt['git_commit'] != commit or receipt['source_state'] != 'clean':
        raise ValueError('published APK source mismatch')
    digest = hashlib.sha256(apk.read_bytes()).hexdigest()
    if (folder / 'android-SHA256SUMS').read_bytes() != (digest + '  ' + apk_name + '\n').encode():
        raise ValueError('published APK checksum mismatch')
    metadata = json.loads((folder / 'android-build-info.json').read_text(), object_pairs_hook=source.distinct_object)
    expected = dict(receipt, build_type='release', abi='arm64-v8a', min_sdk=29,
                    bytes=apk.stat().st_size, sha256=digest, r8=True, resource_shrinking=True)
    if not isinstance(metadata, dict) or metadata != expected or any(
            type(metadata[key]) is not type(value) for key, value in expected.items()):
        raise ValueError('published build info mismatch')
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--expect-commit', required=True)
    parser.add_argument('--formal', action='store_true')
    args = parser.parse_args()
    apk_name = FORMAL_APK if args.formal else APK
    print('ANDROID RELEASE ASSETS PASS:', verify(args.folder, args.expect_commit, apk_name))


if __name__ == '__main__':
    main()
