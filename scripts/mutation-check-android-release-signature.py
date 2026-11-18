#!/usr/bin/env python3
"""Actual Android policy/JNI/Controller mutations on an explicitly selected device.

Requires one normal assertion red, same-APK independent Core green, restored
scenario green, and unchanged SHA256. Does not open any host UI window.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_RELEASE_SIGNATURE_MUTATION'
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to the explicitly intended test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    policy = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseSignature.kt'
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    jni = ROOT / 'android/bridge/src/main/cpp/release_jni.c'
    originals = {p: p.read_bytes() for p in [policy, controller, jni]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    prefix = 'com.hanxi.ghosterm.PluginReleaseSignatureTest#'
    cases = [
        ('revoked', prefix + 'policyScopesUnknownKeysAndRevocationsRemainDistinct', policy,
         'ensure(keyID !in revoked, Failure.REVOKED_KEY)', 'ensure(true, Failure.REVOKED_KEY)'),
        ('key-id', prefix + 'policyRejectsNoncanonicalAndMismatchedPublicKeys', policy,
         ' && hash(raw) == id', ''),
        ('signature', prefix + 'signatureAndRawBytesAreNotCanonicalized', policy,
         'when (ReleaseSignature.verify(key.bytes, signed, payload))', 'when (0)'),
        ('identity', prefix + 'signatureBindsEveryReviewedPackageIdentityField', policy,
         'ensure(authenticated.identity == identity, Failure.IDENTITY)',
         'ensure(true, Failure.IDENTITY)'),
        ('duplicate', prefix + 'duplicateEscapedNormalizedAndPrototypeKeysAreRejected', policy,
         'normalized.add(Normalizer.normalize(key, Normalizer.Form.NFC))', 'true'),
        ('json-lexemes', prefix + 'strictJsonRejectsBOMControlsMalformedUtf8AndSurrogates', policy,
         'checkLexemes(source)', 'Unit'),
        ('numeric-rounding', prefix + 'exactNumericLexemesMatchIntegerSemantics', policy,
         'val negative = raw.startsWith', 'if (maximum == 1L) return raw.toDoubleOrNull()?.toLong()?.takeIf { it in 0..1 }\n            val negative = raw.startsWith'),
        ('status', prefix + 'signatureAndRawBytesAreNotCanonicalized', policy,
         '4 -> reject(Failure.SIGNATURE)', '4 -> reject(Failure.CRYPTO_UNKNOWN)'),
        ('jni-verify', prefix + 'signatureAndRawBytesAreNotCanonicalized', jni,
         'status = gt_ext_release_verify(\n                key, sizeof(key), signed_bytes, sizeof(signed_bytes), bytes, (size_t)size);',
         'status = gt_ext_release_verify(\n                key, sizeof(key), signed_bytes, sizeof(signed_bytes), bytes, (size_t)size); status = GT_OK;'),
        ('jni-size', prefix + 'coreAndJniEnforceArgumentBounds', jni,
         'if (key_size != 32 || signature_size != 64 || size < 1 ||',
         'if (key_size < 32 || signature_size < 64 || size < 1 ||'),
        ('core-binding', 'com.hanxi.ghosterm.PluginReleaseControllerTest#signatureBindsCoreSnapshotWithoutSourceOrRuntimeGrant', controller,
         'metadata.publisher, metadata.packageDigest))',
         'metadata.publisher, JSONObject(String(bytes)).getString("packageSHA256")))'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-release-signature-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    print('RECEIPTS ' + str(work), flush=True)
    for path, data in originals.items(): (work / path.name).write_bytes(data)
    def write(path, data):
        temporary = work / ('source-' + str(time.time_ns()))
        temporary.write_bytes(data); temporary.chmod(modes[path]); os.replace(temporary, path)
    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): write(path, data)
            if path.read_bytes() != data: raise RuntimeError('Restore SHA mismatch: ' + str(path))
    def logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result); return output
    def build(label):
        # This also compiles the actual Core package/Controller fixture test.
        logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true',
            ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 900)
        for relative in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
            logged(label + '-install-' + Path(relative).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / relative])
    def test(label, method, green):
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', method,
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        good = ('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(t in output for t in ['java.lang.AssertionError', 'org.junit.ComparisonFailure']) and
            'Process crashed' not in output and 'INSTRUMENTATION_FAILED' not in output)
        if not good or 'INSTRUMENTATION_STATUS: numtests=1' not in output: raise RuntimeError('Not accepted assertion result:\n' + output)
    control = 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings'
    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(c[1] for c in cases)): test('baseline-' + str(index), target, True)
        for name, target, path, before, after in cases:
            write(path, (originals[path].decode().replace(before, after) + '\n// ' + MARK + '\n').encode())
            build(name); test(name + '-target', target, False); test(name + '-control', control, True)
            restore(); build(name + '-restored'); test(name + '-restored', target, True)
            results.append(dict(case=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(name + ': compiled normal assertion RED / Core GREEN / restored GREEN / SHA256', flush=True)
    finally:
        restore()
    for path, data in originals.items(): print(str(path.relative_to(ROOT)) + ' SHA256 ' + hashlib.sha256(data).hexdigest(), flush=True)
    print('ANDROID RELEASE SIGNATURE MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
