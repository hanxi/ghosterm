#!/usr/bin/env python3
"""Build copied Android/Core sources; run public process tests on owned no-window AVD.

No user device, UI activity, SSH/model or root-worktree mutation. The native
child executable exists only in the test APK. JNI/Kotlin mutants must produce
one ordinary JUnit assertion, with same-APK independent Core configuration green.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
CLASS = 'com.hanxi.ghosterm.ExtensionProcessChannelTest'
CONTROL = 'com.hanxi.ghosterm.ConfigIdentityTest#identitySourcesUseCoreOrderingAndOwnership'
REGRESSIONS = ['com.hanxi.ghosterm.' + name for name in (
    'ExtensionChannelTest#temporaryArrayIsCopiedBeforeReturning',
    'ExtensionChannelTest#closeDropsQueuedDataAndRejectsStaleNativeId',
    'ExtensionSessionTest#consentLeaseAndRevocationRemainScoped',
    'ExtensionSessionTest#adoptedSessionsRetainQuotaUntilActualRelease')]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args, serial):
    files = [p for folder in ('android', 'core', 'terminal/android') for p in (ROOT / folder).rglob('*')
        if p.is_file() and not any(part in ('build', '.cxx', '.gradle', '.kotlin', '__pycache__') for part in p.relative_to(ROOT).parts)]
    files += [ROOT / 'scripts/test-plugin-process-channel-android.py', ROOT / 'scripts/test-plugin-channel.py',
        ROOT / 'scripts/test-plugin-codex-patch-android.py']
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in files}
    spec = importlib.util.spec_from_file_location('process_channel_junit', ROOT / 'scripts/test-plugin-channel.py')
    junit = importlib.util.module_from_spec(spec); spec.loader.exec_module(junit)
    with tempfile.TemporaryDirectory(prefix='process-channel-android-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; source.mkdir(); logs = work / 'logs'; logs.mkdir()
        ignore = shutil.ignore_patterns('build', '.cxx', '.gradle', '.kotlin', '__pycache__')
        for folder in ('android', 'core', 'terminal/android'): shutil.copytree(ROOT / folder, source / folder, ignore=ignore)
        for folder in ('build', 'vendor', 'patches', 'scripts', 'packages', 'assets', 'runtime-payload'):
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = {**os.environ, 'TMPDIR': str(source / 'tmp'),
            'JAVA_HOME': str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            'ANDROID_HOME': str(ROOT / 'build/android-toolchain/sdk'),
            'GRADLE_USER_HOME': str(ROOT / 'build/android-toolchain/gradle-home')}
        adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]

        def run(label, command, seconds=120, cwd=ROOT):
            p = subprocess.Popen(list(map(str, command)), cwd=cwd, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, errors='backslashreplace', start_new_session=True)
            try: stdout, stderr = p.communicate(timeout=seconds)
            except BaseException:
                try: os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    try: os.killpg(p.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            output = stdout + stderr; (logs / (label + '.log')).write_text(output)
            if p.returncode: raise RuntimeError(label + '\n' + output)
            return output

        def test(label, selection, expected=1, red=False):
            output = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selection,
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], 120)
            codes = re.findall(r'^INSTRUMENTATION_STATUS_CODE: (-?\d+)\s*$', output, re.M)
            if any(token in output for token in ('INSTRUMENTATION_FAILED', 'Process crashed', 'Fatal signal', 'AddressSanitizer', 'runtime error:')):
                raise RuntimeError(label + ': invalid instrumentation result\n' + output)
            if red:
                stacks = re.findall(r'^INSTRUMENTATION_STATUS: stack=([^\r\n]+)', output, re.M)
                if codes.count('1') != 1 or codes.count('-2') != 1 or len(stacks) != 1 or not junit.normal_junit_assertion(output):
                    raise RuntimeError(label + ': expected one ordinary JUnit assertion\n' + output)
            elif codes.count('1') != expected or codes.count('0') != expected or any(code not in ('0', '1') for code in codes) or not re.search(r'OK \(' + str(expected) + r' tests?\)', output):
                raise RuntimeError(label + ': exact discovery/result mismatch\n' + output)
            return output

        def installed(label, package, apk):
            run(label + '-install', [*adb, 'install', '-r', '-t', apk])
            path = run(label + '-path', [*adb, 'shell', 'pm', 'path', package]).strip().splitlines()
            if len(path) != 1 or not path[0].startswith('package:/data/app/'): raise RuntimeError('Unexpected package path')
            actual = run(label + '-sha', [*adb, 'shell', 'sha256sum', path[0][8:]]).split()[0]
            if actual != sha(apk): raise RuntimeError('Installed APK SHA mismatch')
            return actual

        api = run('api', [*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip()
        abi = run('abi', [*adb, 'shell', 'getprop', 'ro.product.cpu.abi']).strip()
        page = run('page', [*adb, 'shell', 'getconf', 'PAGE_SIZE']).strip()
        if api != args.headless_api or abi != 'arm64-v8a': raise RuntimeError('Owned device profile mismatch')
        cpp = source / 'android/bridge/src/main/cpp/extension_jni.c'
        kotlin = source / 'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ExtensionChannel.kt'
        originals = {cpp: cpp.read_bytes(), kotlin: kotlin.read_bytes()}
        cases = [
            ('jni-exit', cpp, 'i->exit_code,\n                        i->term_signal,', '0,\n                        i->term_signal,', 'rawArgumentsEnvironmentAndFinalExit'),
            ('jni-unsupported', cpp, 'jlong values[16] = {status,', 'jlong values[16] = {(status == GT_OK ? GT_OK : GT_OK),', 'missingApprovalMalformedTextAndNonProcessAreExplicit'),
            ('kotlin-diagnostic', kotlin, 'diagnostic.copyOf())', 'ByteArray(diagnostic.size))', 'rawArgumentsEnvironmentAndFinalExit'),
        ]
        # clang-format can realign the initializer; require the actual unique
        # public transport observation field, not the Executor's later table.
        text = originals[cpp].decode()
        start = text.index('jlong values[16] = '); end = text.index('(*e)->SetLongArrayRegion', start)
        table = text[start:end]
        if table.count('i->exit_code') != 1: raise RuntimeError('Ambiguous exit field')
        cases[0] = ('jni-exit', cpp, table, table.replace('i->exit_code', '0', 1), 'rawArgumentsEnvironmentAndFinalExit')
        for label, path, old, _, _ in cases:
            if originals[path].decode().count(old) != 1: raise RuntimeError('Ambiguous mutation ' + label)
        apk = source / 'android/app/build/outputs/apk/debug/app-debug.apk'
        tests = source / 'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk'
        actual_build = None

        def build(label, changed=False):
            if actual_build:
                (actual_build / 'CMakeFiles/ghosterm_android.dir/extension_jni.c.o').unlink(missing_ok=True)
            output = run(label, [source / 'android/gradlew', '--no-daemon', '-PghostermPluginLua=false',
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 600, source / 'android')
            if 'BUILD SUCCESSFUL' not in output: raise RuntimeError('Missing successful Gradle build')
            a = installed(label + '-app', 'com.hanxi.ghosterm', apk)
            b = installed(label + '-tests', 'com.hanxi.ghosterm.test', tests)
            with zipfile.ZipFile(apk) as z:
                if any('test_process_channel' in name for name in z.namelist()): raise RuntimeError('Test executable leaked into production APK')
                library = hashlib.sha256(z.read('lib/arm64-v8a/libghosterm_android.so')).hexdigest()
            with zipfile.ZipFile(tests) as z:
                helper = hashlib.sha256(z.read('lib/arm64-v8a/libghosterm_test_process_channel.so')).hexdigest()
            return dict(app=a, tests=b, jni=library, child=helper)

        try:
            baseline = build('baseline-build')
            caches = list((source / 'android/bridge/.cxx/Debug').glob('*/arm64-v8a/CMakeCache.txt'))
            if len(caches) != 1: raise RuntimeError('Expected one actual native build')
            actual_build = caches[0].parent
            test('baseline-tests', CLASS, 6); test('baseline-control', CONTROL)
            results = []
            if args.mutate:
                for label, path, old, new, method in cases:
                    before = sha(actual_build / 'CMakeFiles/ghosterm_android.dir/extension_jni.c.o')
                    try:
                        path.write_text(originals[path].decode().replace(old, new, 1) + '\n// GT_PROCESS_CHANNEL_DEVICE_MUTATION\n')
                        artifacts = build(label + '-build')
                        if artifacts['app'] == baseline['app']: raise RuntimeError('APK did not change')
                        if path == cpp and (sha(actual_build / 'CMakeFiles/ghosterm_android.dir/extension_jni.c.o') == before or artifacts['jni'] == baseline['jni']):
                            raise RuntimeError('Actual JNI object/library did not change')
                        test(label + '-red', CLASS + '#' + method, red=True); test(label + '-control', CONTROL)
                    finally: path.write_bytes(originals[path])
                    build(label + '-restore'); test(label + '-green', CLASS + '#' + method)
                    if sha(path) != hashlib.sha256(originals[path]).hexdigest(): raise RuntimeError('Restoration mismatch')
                    results.append(dict(group=label, actualArtifacts=artifacts))
                    print('PASS ' + label + ': compiled JUnit red / same APK Core green / restored / SHA256', flush=True)
            final = build('final-build'); test('final-tests', CLASS, 6); test('final-control', CONTROL)
            for index, selection in enumerate(REGRESSIONS): test('regression-' + str(index), selection)
            if any(sha(ROOT / name) != digest for name, digest in inputs.items()): raise RuntimeError('Root inputs changed')
            receipt = dict(accepted=True, api=api, abi=abi, pageSize=int(page), noWindow=True, inputs=inputs,
                cases=6, control=CONTROL, regressions=REGRESSIONS, mutations=results, actualArtifacts=final,
                scope='test-only Native child / public JNI and Kotlin / Session handoff; no handshake/Runtime/App entry/model/device matrix')
            if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
            print('PROCESS CHANNEL ANDROID PASS: API' + api + ', six cases, ' + str(len(results)) + ' mutations', flush=True)
            return receipt, apk.read_bytes(), tests.read_bytes()
        finally:
            for path, original in originals.items(): path.write_bytes(original)
            # Close only this APK on this owned emulator. Its fault child has an
            # independent alarm; AVD owner then stops the complete guest.
            run('cleanup-app', [*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
            run('cleanup-tests', [*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm.test'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headless-api', choices=('29', '36'), required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--also-api36', action='store_true', help='after API29 exits, verify the same two final APKs on a fresh owned API36')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.also_api36 and args.headless_api != '29': parser.error('--also-api36 requires API29 first')
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): parser.error('receipt must be under project tmp')
    spec = importlib.util.spec_from_file_location('process_channel_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    with helper.headless(args.headless_api) as serial: receipt, app, tests = execute(args, serial)
    if args.also_api36:
        with tempfile.TemporaryDirectory(prefix='process-channel-apks-', dir=ROOT / 'tmp') as temporary:
            work = Path(temporary)
            for name, data in (('app.apk', app), ('tests.apk', tests)): (work / name).write_bytes(data)
            with helper.headless('36') as serial:
                adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]
                def run(label, command):
                    output = helper.run(command, seconds=120); (work / (label + '.log')).write_text(output); return output
                api = run('api', [*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip()
                abi = run('abi', [*adb, 'shell', 'getprop', 'ro.product.cpu.abi']).strip()
                page = run('page', [*adb, 'shell', 'getconf', 'PAGE_SIZE']).strip()
                if api != '36' or abi != 'arm64-v8a': raise RuntimeError('Second owned AVD profile mismatch')
                for name, package, expected in (('app.apk', 'com.hanxi.ghosterm', receipt['actualArtifacts']['app']),
                        ('tests.apk', 'com.hanxi.ghosterm.test', receipt['actualArtifacts']['tests'])):
                    run(name + '-install', [*adb, 'install', '-r', '-t', work / name])
                    path = run(name + '-path', [*adb, 'shell', 'pm', 'path', package]).strip().splitlines()
                    if len(path) != 1 or not path[0].startswith('package:/data/app/'): raise RuntimeError('Unexpected installed package')
                    if run(name + '-sha', [*adb, 'shell', 'sha256sum', path[0][8:]]).split()[0] != expected: raise RuntimeError('Same APK identity mismatch')
                for index, selection in enumerate([CLASS, CONTROL, *REGRESSIONS]):
                    expected = 6 if selection == CLASS else 1
                    output = run('test-' + str(index), [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selection,
                        'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                    codes = re.findall(r'^INSTRUMENTATION_STATUS_CODE: (-?\d+)\s*$', output, re.M)
                    if codes.count('1') != expected or codes.count('0') != expected or any(code not in ('0', '1') for code in codes) or not re.search(r'OK \(' + str(expected) + r' tests?\)', output):
                        raise RuntimeError('API36 exact results mismatch\n' + output)
                if any(sha(ROOT / name) != digest for name, digest in receipt['inputs'].items()): raise RuntimeError('Root input changed before second AVD')
                receipt['sameApksAPI36'] = dict(accepted=True, api=api, abi=abi, pageSize=int(page), noWindow=True,
                    cases=6, control=CONTROL, regressions=REGRESSIONS, actualArtifacts=receipt['actualArtifacts'])
                if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
                print('PROCESS CHANNEL ANDROID PASS: same APKs / API36 / six cases + configuration + four regressions', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
