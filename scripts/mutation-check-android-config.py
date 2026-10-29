#!/usr/bin/env python3
"""Android config transactions and UI: mutated APK red, independent test green, SHA restore."""
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_CONFIG_MUTATION'
spec = importlib.util.spec_from_file_location('native_mutations', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
run, require = helper.run, helper.require


def main():
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to a test device')
    adb = ['adb', '-s', serial]
    require(run([*adb, 'get-state']))
    repo = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/ConfigRepository.kt'
    domain = ROOT / 'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/Hosts.kt'
    screen = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    native = ROOT / 'android/bridge/src/main/cpp/config_jni.c'
    originals = {p: p.read_bytes() for p in (repo, domain, screen, native)}
    if any(MARK.encode() in text for text in originals.values()): raise RuntimeError('mutation marker present')
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    work = ROOT / 'tmp/android-config-mutations'; work.mkdir(exist_ok=True)
    def build():
        result = run(['bash', ROOT / 'scripts/build-android.sh'], seconds=900)
        (work / 'build.log').write_text(result.stdout + result.stderr)
        require(result)
        for apk in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            require(run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / apk]))
    def test(method, green):
        name = ('HostScreenTest' if method == 'addEditSearchAndDiscardUseRealConfiguration' else 'ConfigRepositoryTest') + '#' + method
        result = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                      'com.hanxi.ghosterm.' + name,
                      'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        output = result.stdout + result.stderr
        (work / (method + '.log')).write_text(output)
        passed = result.returncode == 0 and 'OK (1 test)' in output
        if passed != green or (not green and (not any(kind in output for kind in ('AssertionError', 'org.junit.ComparisonFailure')) or 'FAILURES' not in output)):
            raise RuntimeError(output)
    control = 'firstSaveAndReloadDeriveHostsFromConfig'
    cases = [
        (repo, 'throw ConfigAccess("备份失败，原配置未修改", error)', 'Unit', 'backupFailureLeavesOriginal', control),
        (repo, 'if (revision != previous.revision) throw ConfigConflict()', 'if (false && revision != previous.revision) throw ConfigConflict()', 'equalSizeExternalEditIsRejected', control),
        (repo, 'if (raw().second != revision) throw ConfigConflict()', 'if (false && raw().second != revision) throw ConfigConflict()', 'externalEditDuringBackupIsRejected', control),
        (repo, 'ops.replace(temp, target)', 'Files.deleteIfExists(target); ops.replace(temp, target)', 'replaceFailureNeverDeletesOriginal', control),
        (domain, 'if (previous == null || user != previous.user)', 'if (true)', 'inheritedValuesAreNotWrittenBack', control),
        (repo, 'name.isNotBlank() && name.none { it in "*?![]" || it.isWhitespace() }', 'name.isNotBlank()', 'sparseEditAndBackupPreserveUnchangedBytes', control),
        (native, '.default_user = ""', '.default_user = "android_uid"', 'unspecifiedRemoteUserRemainsEmpty', control),
        (screen, 'model.edit(host)', 'model.edit()', 'addEditSearchAndDiscardUseRealConfiguration', control),
        (repo, 'if (lines.replace(previous.text, "") != lines.replace(candidate.toString(Charsets.UTF_8), ""))',
         'if (false && lines.replace(previous.text, "") != lines.replace(candidate.toString(Charsets.UTF_8), ""))',
         'complexDuplicateEditCannotRemoveOtherDirectives', control),
        (repo, 'throw ConfigAccess("备份失败，原配置未修改", error)',
         'Files.deleteIfExists(backup); throw ConfigAccess("备份失败，原配置未修改", error)',
         'backupNameCollisionDoesNotDeleteExistingFile', control),
        (repo, 'if (Regex("-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----").containsMatchIn(bytes.toString(Charsets.UTF_8)))',
         'if (false && Regex("-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----").containsMatchIn(bytes.toString(Charsets.UTF_8)))',
         'privateKeyContainerIsNeverSavedAsConfiguration', control),
    ]
    build()
    for name in dict.fromkeys([case[3] for case in cases] + [control]): test(name, True)
    try:
        for index, (path, before, after, target, independent) in enumerate(cases, 1):
            source = originals[path].decode()
            if source.count(before) != 1: raise RuntimeError('missing or ambiguous mutation anchor: ' + before)
            path.write_text(source.replace(before, after + ' /* ' + MARK + ' */'))
            build()
            test(target, False)
            test(independent, True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[path]: raise RuntimeError('restore SHA mismatch')
            build(); test(target, True)
            print(f'C{index}: target red / independent green / restored green / SHA256 identical', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()): raise RuntimeError('source changed during mutation run')
    print('ANDROID CONFIG MUTATIONS PASS: 11 groups')


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
