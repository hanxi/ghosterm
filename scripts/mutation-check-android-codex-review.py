#!/usr/bin/env python3
"""Compile Android review mutations, then require a single device assertion red and same APK Core green."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import signal
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = b'GT_ANDROID_CODEX_REVIEW_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
SOURCE = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
CASES = (
    ('readonly', 'ConversationControls.kt', 'readOnly = true', 'readOnly = false',
     'CodexChangeReviewUiTest#fullDiffIsSelectableReadOnlyAndLastFileIsReachable'),
    ('identity', 'CodexChangeJson.kt',
     'thread == reference.threadID && turn == reference.turnID && item == reference.itemID', 'true',
     'CodexChangeJsonTest#documentIdentityIsExactAndMalformedEncodingIsRejected'),
    ('schema', 'CodexChangeJson.kt',
     'require(reader.peek() == JsonToken.NUMBER && reader.nextString() == "1")', 'reader.nextString()',
     'CodexChangeJsonTest#schemaRejectsDuplicatesUnknownFieldsAndCoercedVersionOrDiff'),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        parser.error('Set ANDROID_SERIAL to the dedicated test emulator/device')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    sdk = ROOT / 'build/android-toolchain/sdk'
    jdk = ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'
    os.environ.update(TMPDIR=str(ROOT / 'tmp'), JAVA_HOME=str(jdk), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    os.environ['JAVA_TOOL_OPTIONS'] = os.environ.get('JAVA_TOOL_OPTIONS', '') + ' -Djava.io.tmpdir=' + str(ROOT / 'tmp')
    adb = [str(sdk / 'platform-tools/adb'), '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    originals = {SOURCE / case[1]: (SOURCE / case[1]).read_bytes() for case in CASES}
    if any(MARK in value for value in originals.values()):
        raise RuntimeError('Residual Codex review mutation')
    for name, file, old, _, _ in CASES:
        if originals[SOURCE / file].count(old.encode()) != 1:
            raise RuntimeError(f'{name}: source anchor missing or ambiguous')

    def restore():
        for path, value in originals.items():
            if path.read_bytes() != value:
                if MARK not in path.read_bytes():
                    raise RuntimeError(f'Concurrent source change; preserving {path}')
                path.write_bytes(value)
            if hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(value).digest():
                raise RuntimeError('Restored source SHA256 mismatch')

    with tempfile.TemporaryDirectory(prefix='android-codex-review-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)

        def run(label, command, seconds=120):
            result = helper.run(command, seconds)
            output = result.stdout + result.stderr
            (work / (label + '.log')).write_text(output)
            helper.require(result)
            return output

        def build(label):
            run(label + '-build', [ROOT / 'android/gradlew', '-p', ROOT / 'android', '-PghostermPluginJs=true',
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 900)
            for name in ('debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk'):
                run(label + '-install-' + Path(name).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / name])

        def test(label, target, green):
            output = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                'com.hanxi.ghosterm.' + target, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
            if green:
                valid = 'OK (1 test)' in output and 'FAILURES' not in output
            else:
                valid = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                    marker in output for marker in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
            if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not valid or 'initializationError' in output:
                raise RuntimeError(f'{label}: expected compiled single assertion {"green" if green else "red"}\n{output}')

        try:
            build('baseline')
            for name, file, old, new, target in CASES:
                if args.case and name not in args.case:
                    continue
                test(name + '-baseline', target, True)
                path = SOURCE / file
                path.write_bytes(originals[path].replace(old.encode(), new.encode()) + b'\n// ' + MARK + b'\n')
                build(name + '-mutant')  # Compilation failure is never an assertion red.
                test(name + '-red', target, False)
                test(name + '-control', 'NativeSmokeTest#corePreservesUtf8AndLineEndings', True)
                restore()
                build(name + '-restored')
                test(name + '-restored', target, True)
                print(f'PASS {name}: compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
        finally:
            restore()
            helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print('ANDROID CODEX REVIEW DEVICE MUTATIONS PASS', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
