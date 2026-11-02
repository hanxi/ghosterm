#!/usr/bin/env python3
"""Build real release APKs with bad dependency paths; keep signatures/ELF valid."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import struct
import time
import zipfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_PACKAGE_PATH_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=['prefix', 'filename'], action='append', dest='selected')
    args = parser.parse_args()
    if not os.environ.get('ANDROID_RELEASE_STORE_FILE'):
        raise RuntimeError('Set the release signing environment before running package mutations')
    builder = ROOT / 'scripts/build-android-native.py'
    original = builder.read_bytes()
    if MARK.encode() in original:
        raise RuntimeError('Residual package path mutation')
    text = original.decode()
    cases = [
        ('prefix', "logical_openssl_prefix = '/nonexistent/ghosterm'", 'logical_openssl_prefix = str(prefix)'),
        ('filename', "ssh2_flags = shlex.join(['-ffile-prefix-map=' + str(ssh2) + '=libssh2'])", "ssh2_flags = ''"),
    ]
    if args.selected:
        if len(set(args.selected)) != len(args.selected):
            raise RuntimeError('Duplicate package path case')
        cases = [case for case in cases if case[0] in args.selected]
    for name, old, _ in cases:
        if text.count(old) != 1:
            raise RuntimeError('Missing or ambiguous package path anchor: ' + name)
    out = ROOT / 'tmp/android-package-path-mutations' / str(time.time_ns())
    out.mkdir(parents=True)
    apk = ROOT / 'android/app/build/outputs/apk/release/app-release.apk'
    sdk = Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk'))
    tools = sdk / 'build-tools/35.0.0'

    def logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        (out / (label + '.log')).write_text(result.stdout + result.stderr)
        return result

    def build(label):
        helper.require(logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh', ':app:assembleRelease'], 900))

    def paths(label, green):
        result = logged(label, ['python3', ROOT / 'scripts/check-android-package-paths.py', apk])
        output = result.stdout + result.stderr
        if green:
            helper.require(result)
            if 'ANDROID APK BUILD PATHS PASS' not in output:
                raise RuntimeError('Missing package path success receipt')
        elif result.returncode == 0 or 'Android APK embeds build-host paths:' not in output:
            raise RuntimeError('Expected the compiled APK path guard to reject this dependency recipe')

    def control(label):
        signature = logged(label + '-signature', [tools / 'apksigner', 'verify', '--verbose', '--print-certs', apk])
        helper.require(signature)
        certificate = re.search(r'Signer #1 certificate SHA-256 digest: (\w+)', signature.stdout + signature.stderr)[1]
        helper.require(logged(label + '-zipalign', [tools / 'zipalign', '-c', '-P', '16', '4', apk]))
        libraries = []
        with zipfile.ZipFile(apk) as archive:
            for entry in archive.infolist():
                if not entry.filename.endswith('.so'):
                    continue
                data = archive.read(entry)
                assert entry.filename.startswith('lib/arm64-v8a/') and data[:6] == b'\x7fELF\x02\x01'
                assert struct.unpack_from('<H', data, 18)[0] == 183
                offset = struct.unpack_from('<Q', data, 32)[0]
                size, count = struct.unpack_from('<HH', data, 54)
                loads = [struct.unpack_from('<IIQQQQQQ', data, offset + i * size) for i in range(count)
                         if struct.unpack_from('<I', data, offset + i * size)[0] == 1]
                assert loads and all(p[7] >= 16384 and p[2] % 16384 == p[3] % 16384 for p in loads)
                libraries.append(entry.filename)
        assert libraries
        return dict(certificate=certificate, libraries=sorted(libraries))

    accepted = []
    try:
        build('baseline'); paths('baseline-paths', True); baseline = control('baseline-control')
        for name, old, new in cases:
            builder.write_text(text.replace(old, new) + '\n# ' + MARK + '\n')
            build(name); paths(name + '-target', False)
            assert control(name + '-control') == baseline
            builder.write_bytes(original)
            build(name + '-restored'); paths(name + '-restored-paths', True)
            assert control(name + '-restored-control') == baseline
            assert hashlib.sha256(builder.read_bytes()).digest() == hashlib.sha256(original).digest()
            accepted.append(dict(case=name, compiled_apk_path_red=True, same_apk_signature_elf_zip_green=True,
                                 restored_green=True, source_sha256_restored=True))
            (out / 'accepted-results.json').write_text(json.dumps(accepted, indent=2) + '\n')
            print('PASS package-path-' + name + ': compiled APK red / same APK signature, ELF and ZIP green / restored green / SHA256', flush=True)
    finally:
        if MARK.encode() in builder.read_bytes():
            builder.write_bytes(original)
        if builder.read_bytes() != original:
            raise RuntimeError('Package builder source restoration failed')
    print(f'ANDROID PACKAGE PATH MUTATIONS PASS: {len(accepted)} groups; receipts {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame):
        raise KeyboardInterrupt(f'signal {number}')
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
