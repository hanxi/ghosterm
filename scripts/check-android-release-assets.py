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
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
APK = 'Ghosterm-android-arm64-dev.apk'
FORMAL_APK = 'Ghosterm-android-arm64-release.apk'
spec = importlib.util.spec_from_file_location('apk_source', ROOT / 'scripts/check-android-build-info.py')
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


def verify_apk_version(apk, version, aapt):
    major, minor, patch = map(int, version.split('.'))
    expected_code = major * 1000000 + minor * 1000 + patch + 2
    result = subprocess.run([aapt, 'dump', 'badging', apk], capture_output=True,
                            text=True, check=True, timeout=30)
    packages = [line for line in result.stdout.splitlines() if line.startswith('package:')]
    if len(packages) != 1 or not re.match(
            r"^package: name='com\.hanxi\.ghosterm' versionCode='"
            + str(expected_code) + r"' versionName='" + re.escape(version) + r"'(?: |$)", packages[0]):
        raise ValueError('APK package identity or formal version mismatch')


def verify(folder, commit, formal_version=None, aapt=None):
    apk_name = FORMAL_APK if formal_version is not None else APK
    apk = folder / apk_name
    if not all((folder / name).is_file() for name in (apk_name, 'android-SHA256SUMS', 'android-build-info.json')):
        raise ValueError('missing Android Release asset')
    receipt = source.read_metadata(apk)
    if receipt['git_commit'] != commit or receipt['source_state'] != 'clean':
        raise ValueError('published APK source mismatch')
    with apk.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if (folder / 'android-SHA256SUMS').read_bytes() != (digest + '  ' + apk_name + '\n').encode():
        raise ValueError('published APK checksum mismatch')
    metadata = json.loads((folder / 'android-build-info.json').read_text(), object_pairs_hook=source.distinct_object)
    expected = dict(receipt, build_type='release', abi='arm64-v8a', min_sdk=29,
                    bytes=apk.stat().st_size, sha256=digest, r8=True, resource_shrinking=True)
    if formal_version is not None:
        expected['version_name'] = formal_version
    if not isinstance(metadata, dict) or metadata != expected or any(
            type(metadata[key]) is not type(value) for key, value in expected.items()):
        raise ValueError('published build info mismatch')
    if aapt is not None:
        verify_apk_version(apk, formal_version, aapt)
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--expect-commit', required=True)
    parser.add_argument('--formal-version', help='verify a formal APK and receipt against this release version')
    parser.add_argument('--aapt', help='also verify the formal version in the APK manifest')
    args = parser.parse_args()
    if args.aapt and not args.formal_version:
        parser.error('--aapt requires --formal-version')
    print('ANDROID RELEASE ASSETS PASS:', verify(args.folder, args.expect_commit, args.formal_version, args.aapt))


if __name__ == '__main__':
    main()
