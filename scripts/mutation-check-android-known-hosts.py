#!/usr/bin/env python3
"""Known-host source records over real JNI: compiled red, same APK control, restore."""
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_KNOWN_HOSTS_MUTATION'
spec = importlib.util.spec_from_file_location('native_mutations', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
run, require = helper.run, helper.require


def main():
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to a test device')
    adb = ['adb', '-s', serial]
    require(run([*adb, 'get-state']))
    native = ROOT / 'android/bridge/src/main/cpp/known_hosts_jni.c'
    binding = ROOT / 'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/KnownHostsBridge.kt'
    originals = {p: p.read_bytes() for p in (native, binding)}
    if any(MARK.encode() in text for text in originals.values()): raise RuntimeError('residual mutation marker')
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    logs = ROOT / 'tmp/android-known-hosts-mutations'
    logs.mkdir(parents=True, exist_ok=True)

    def build(label):
        result = run(['bash', ROOT / 'scripts/build-android.sh'], seconds=900)
        (logs / (label + '-build.log')).write_text(result.stdout + result.stderr)
        require(result)
        for apk in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / apk]))

    def test(label, method, green):
        result = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                      'com.hanxi.ghosterm.' + method,
                      'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output = result.stdout + result.stderr
        (logs / (label + '.log')).write_text(output)
        passed = result.returncode == 0 and 'OK (1 test)' in output
        assertion = any(s in output for s in ('AssertionError', 'org.junit.ComparisonFailure'))
        if passed != green or (not green and (not assertion or 'FAILURES' not in output)):
            raise RuntimeError(output)

    cases = [
        ('marker', native, 'end, entry.marker, entry.hosts', 'end, "", entry.hosts',
         'sourceRecordsKeepMarkersMalformedRowsAndExactByteRanges'),
        ('range', native, 'entry.byte_end);', 'entry.byte_end - 1);',
         'sourceRecordsKeepMarkersMalformedRowsAndExactByteRanges'),
        ('recognition', binding, 'it[7] == "1", it[8] == "1"', 'true, it[8] == "1"',
         'sourceRecordsKeepMarkersMalformedRowsAndExactByteRanges'),
        ('utf8', binding, 'CodingErrorAction.REPORT', 'CodingErrorAction.REPLACE',
         'invalidAndOverBudgetInputNeverBecomeAnEmptyList'),
        ('committed', binding, 'fields[1] == "1"', 'false',
         'removalKeepsExactBytesAndRejectsStaleRevision'),
        ('conflict', binding, 'fields[2] == "1"', 'false',
         'removalKeepsExactBytesAndRejectsStaleRevision'),
        ('status', native, 'sizeof code, "%d", status', 'sizeof code, "%d", GT_OK',
         'removalCannotEraseReadonlyRulesOrHideIoFailures'),
        ('remove-index', native, '(size_t)index, &update', '(size_t)index + 1, &update',
         'removalKeepsExactBytesAndRejectsStaleRevision'),
    ]
    control = 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    try:
        build('baseline')
        for label, path, before, after, method in cases:
            target = 'KnownHostsBridgeTest#' + method
            test(label + '-baseline', target, True)
            source = originals[path].decode()
            if source.count(before) != (2 if label == 'utf8' else 1): raise RuntimeError('ambiguous mutation anchor: ' + label)
            path.write_text(source.replace(before, after) + '\n// ' + MARK + '\n')
            build(label)
            test(label, target, False)
            test(label + '-control', control, True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[path]: raise RuntimeError('restore SHA256')
            build(label + '-restored')
            test(label + '-restored', target, True)
            print(f'PASS {label}: compiled target red / same APK control green / restored green / SHA256', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()):
            raise RuntimeError('repository source changed during mutations')
    print(f'ANDROID KNOWN HOSTS MUTATIONS PASS: {len(cases)} groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
