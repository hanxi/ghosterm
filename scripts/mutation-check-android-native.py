#!/usr/bin/env python3
"""Run actual device JNI regressions with targeted mutations and independent controls."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_NATIVE_MUTATION'
TEST_CLASS = 'com.hanxi.ghosterm.NativeSmokeTest'


def run(command, seconds=300):
    # All bounded commands use the repository's portable timeout wrapper.
    shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require android-native; shift; limit="$1"; shift; gt_timeout "$limit" "$@"'
    return subprocess.run(['bash', '-c', shell, 'android-native', str(ROOT), str(seconds), *map(str, command)],
                          cwd=ROOT, capture_output=True, text=True)


def require(result):
    if result.returncode:
        raise RuntimeError(result.stdout + result.stderr)


def main():
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the intended emulator/device')
    adb = ['adb', '-s', serial]
    require(run([*adb, 'get-state']))
    core = ROOT / 'android/bridge/src/main/cpp/core_jni.c'
    vt = ROOT / 'terminal/android/native/vt_jni.c'
    original = {p: p.read_bytes() for p in (core, vt)}
    hashes = {p: hashlib.sha256(data).digest() for p, data in original.items()}
    if any(MARK.encode() in data for data in original.values()):
        raise RuntimeError('existing mutation marker; refusing to run')
    work = ROOT / 'tmp/android-native-mutations'
    work.mkdir(exist_ok=True)
    def build_install():
        result = run(['bash', ROOT / 'scripts/build-android.sh'], seconds=900)
        (work / 'build.log').write_text(result.stdout + result.stderr)
        require(result)  # Compilation errors are NOT a red test.
        for apk in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / apk]))
    def test(method, green):
        result = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                      TEST_CLASS + '#' + method,
                      'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output = result.stdout + result.stderr
        (work / (method + '.log')).write_text(output)
        passed = result.returncode == 0 and 'OK (1 test)' in output
        if passed != green or (not green and ('AssertionError' not in output or 'FAILURES' not in output)):
            raise RuntimeError(output)
    tests = ['corePreservesUtf8AndLineEndings', 'configurationRejectsNul', 'vtExecutesCursorMovement']
    build_install()
    for method in tests: test(method, True)
    cases = [
        (core, 'jbyteArray result = bytes(env, serialized, strlen(serialized));',
         'if (*serialized) serialized[0] = \'!\';\n    jbyteArray result = bytes(env, serialized, strlen(serialized));',
         tests[0], tests[2]),
        (core, 'if (memchr(text, \'\\0\', (size_t)count))', 'if (0 && memchr(text, \'\\0\', (size_t)count))',
         tests[1], tests[2]),
        (vt, 'ghostty_terminal_vt_write(terminal, (const uint8_t *)data, (size_t)count);',
         'ghostty_terminal_vt_write(terminal, (const uint8_t *)data, (size_t)count * 0);',
         tests[2], tests[0]),
    ]
    try:
        for index, (path, before, after, target, control) in enumerate(cases, 1):
            source = original[path].decode()
            if source.count(before) != 1:
                raise RuntimeError('mutation anchor missing or ambiguous')
            path.write_text(source.replace(before, after + ' /* ' + MARK + ' */'))
            build_install()
            test(target, False)
            test(control, True)  # Same mutated APK, unrelated native component.
            path.write_bytes(original[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[path]:
                raise RuntimeError('restore SHA256 mismatch')
            build_install()
            test(target, True)
            print(f'N{index}: target red / control green / restored green / SHA256 identical', flush=True)
    finally:
        for path, content in original.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(content)
        if any(hashlib.sha256(p.read_bytes()).digest() != value for p, value in hashes.items()):
            raise RuntimeError('source changed during mutations')
    print('ANDROID NATIVE MUTATIONS PASS: 3 groups')


if __name__ == '__main__':
    def interrupt(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupt)
    signal.signal(signal.SIGTERM, interrupt)
    main()
