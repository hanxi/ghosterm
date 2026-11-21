#!/usr/bin/env python3
"""Verify the three downloaded Android Release assets agree with this CI commit.

This checks publication integrity and source identity. With Android Build Tools
it also verifies the signature, alignment, and manifest and native ABI metadata.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import subprocess
from zipfile import ZipFile

ROOT = Path(__file__).resolve().parents[1]
APK = 'Ghosterm-android-arm64-dev.apk'
FORMAL_APK = 'Ghosterm-android-arm64-release.apk'
spec = importlib.util.spec_from_file_location('apk_source', ROOT / 'scripts/check-android-build-info.py')
source = importlib.util.module_from_spec(spec)
spec.loader.exec_module(source)


def verify_apk_version(apk, version, aapt):
    if version is None:
        version, expected_code = '0.1.0-dev', 1
    else:
        major, minor, patch = map(int, version.split('.'))
        expected_code = major * 1000000 + minor * 1000 + patch + 2
    result = subprocess.run([aapt, 'dump', 'badging', apk], capture_output=True,
                            text=True, check=True, timeout=30)
    lines = result.stdout.splitlines()
    packages = [line for line in lines if line.startswith('package:')]
    if len(packages) != 1 or not re.match(
            r"^package: name='com\.hanxi\.ghosterm' versionCode='"
            + str(expected_code) + r"' versionName='" + re.escape(version) + r"'(?: |$)", packages[0]):
        raise ValueError('APK package identity or release version mismatch')
    if [line.strip() for line in lines if line.startswith('sdkVersion:')] != ["sdkVersion:'29'"]:
        raise ValueError('APK minimum Android SDK does not match the release metadata')
    abi = [line.removeprefix('native-code:').split() for line in lines if line.startswith('native-code:')]
    if abi != [["'arm64-v8a'"]]:
        raise ValueError('APK native ABI does not match the arm64 release metadata')


def verify_native_page_alignment(apk):
    with ZipFile(apk) as archive:
        libraries = [entry for entry in archive.infolist()
                     if re.fullmatch(r'lib/arm64-v8a/[^/]+\.so', entry.filename)]
        if not libraries:
            raise ValueError('APK is missing arm64 native libraries')
        for library in libraries:
            with archive.open(library) as stream:
                header = stream.read(64)
                if len(header) != 64 or header[:6] != b'\x7fELF\x02\x01':
                    raise ValueError('expected a little-endian ELF64 library: ' + library.filename)
                offset = struct.unpack_from('<Q', header, 32)[0]
                entry_size, count = struct.unpack_from('<HH', header, 54)
                if entry_size != 56 or not count or offset < 64 or offset + entry_size * count > library.file_size:
                    raise ValueError('invalid ELF program headers: ' + library.filename)
                stream.seek(offset)
                loads = 0
                for _ in range(count):
                    entry = stream.read(entry_size)
                    if struct.unpack_from('<I', entry)[0] == 1:  # PT_LOAD
                        loads += 1
                        if struct.unpack_from('<Q', entry, 48)[0] < 16384:
                            raise ValueError('ELF load segment alignment below 16 KiB: ' + library.filename)
                if not loads:
                    raise ValueError('missing ELF load segments: ' + library.filename)


def verify(folder, commit, formal_version=None, aapt=None, build_tools=None, certificate_sha256=None):
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
    if build_tools is not None:
        result = subprocess.run([build_tools / 'apksigner', 'verify', '--print-certs', apk],
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise ValueError('apksigner verification failed: ' + (result.stderr.strip() or result.stdout.strip()))
        if certificate_sha256 is not None:
            fingerprints = re.findall(r'^Signer #\d+ certificate SHA-256 digest: ([0-9a-f]{64})$',
                                      result.stdout, re.M)
            if fingerprints != [certificate_sha256]:
                raise ValueError('APK signing certificate does not match the expected SHA-256 fingerprint')
        subprocess.run([build_tools / 'zipalign', '-c', '-P', '16', '4', apk], check=True, timeout=30)
        verify_native_page_alignment(apk)
        aapt = build_tools / 'aapt'
    if aapt is not None:
        verify_apk_version(apk, formal_version, aapt)
    return digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder', type=Path)
    parser.add_argument('--expect-commit', required=True)
    parser.add_argument('--formal-version', help='verify a formal APK and receipt against this release version')
    tools = parser.add_mutually_exclusive_group()
    tools.add_argument('--aapt', help='also verify the version in the APK manifest')
    tools.add_argument('--android-build-tools', type=Path,
                       help='verify APK signature, alignment and version using this Build Tools directory')
    parser.add_argument('--android-certificate-sha256', help='expected APK signer certificate SHA-256 in lowercase hex')
    args = parser.parse_args()
    if args.android_certificate_sha256 is not None and (
            not args.android_build_tools or not re.fullmatch(r'[0-9a-f]{64}', args.android_certificate_sha256)):
        parser.error('--android-certificate-sha256 requires --android-build-tools and 64 lowercase hex digits')
    print('ANDROID RELEASE ASSETS PASS:', verify(args.folder, args.expect_commit, args.formal_version,
                                                args.aapt, args.android_build_tools, args.android_certificate_sha256))


if __name__ == '__main__':
    main()
