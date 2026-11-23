#!/usr/bin/env python3
"""Android version capability and full-declaration comparison guards on an explicitly selected device.

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
MARK = 'GT_ANDROID_VERSION_CHANGES_MUTATION'
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
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    declarations = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseDeclarations.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionSelectionContent.kt'
    originals = {p: p.read_bytes() for p in [controller, declarations, view]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    data = 'com.hanxi.ghosterm.PluginVersionChangesTest#'
    decoded = 'com.hanxi.ghosterm.PluginReleaseDeclarationsTest#'
    page = 'com.hanxi.ghosterm.PluginVersionChangesUITest#actualSettingsShowsCapabilityUpgradeUnknownAndRollbackWithoutGuessingEmptyDifferencesOrGrantingSource'
    cases = [
        ('capability-added', data+'verifiedSelectionShowsBothCapabilityDirectionsAndKeepsDeclarationOrder', controller,
         'candidate.review.requiredCapabilities.filter { it !in previous?.review?.requiredCapabilities.orEmpty() }', 'candidate.review.requiredCapabilities'),
        ('capability-removed', data+'verifiedSelectionShowsBothCapabilityDirectionsAndKeepsDeclarationOrder', controller,
         'previous?.review?.requiredCapabilities.orEmpty().filter { it !in candidate.review.requiredCapabilities }', 'previous?.review?.requiredCapabilities.orEmpty()'),
        ('capability-added-unknown', data+'unknownPreviousNeverClaimsEmptyOrCompleteCapabilityAndPermissionDifferences', controller,
         'val addedCapabilities get() = if (previousState == PreviousVersionState.UNKNOWN) null else ', 'val addedCapabilities get() = '),
        ('capability-removed-unknown', data+'unknownPreviousNeverClaimsEmptyOrCompleteCapabilityAndPermissionDifferences', controller,
         'val removedCapabilities get() = if (previousState == PreviousVersionState.UNKNOWN) null else ', 'val removedCapabilities get() = '),
        ('capability-disabled', data+'disabledPreviousStillParticipatesInFullComparisonAndSameDeclarationsStayEmpty', controller,
         'val addedCapabilities get() = ', 'val addedCapabilities get() = if (previousEnabled == false) emptyList() else '),
        ('permission-added-fields', data+'purposeScopeProgramAndSideChangesEachRemainVisibleAsWholePermissions', controller,
         'it !in previous?.declarations?.permissions.orEmpty()', 'previous?.declarations?.permissions.orEmpty().none { old -> old.side == it.side && old.capability == it.capability }'),
        ('permission-removed-fields', data+'purposeScopeProgramAndSideChangesEachRemainVisibleAsWholePermissions', controller,
         'it !in candidate.declarations.permissions', 'candidate.declarations.permissions.none { next -> next.side == it.side && next.capability == it.capability }'),
        ('manifest-binding', decoded+'rawManifestMustMatchExactCoreDigest', declarations,
         'bytes.size > 65536 || hash(bytes) != metadata.manifestDigest', 'bytes.size > 65536'),
        ('declaration-programs', decoded+'allPermissionsProgramsAndExecutorDeclarationsRemainComplete', declarations,
         'if (names == null) emptyList() else (0 until names.length()).map { names.opt(it) as? String ?: throw Rejected() }', 'emptyList()'),
        ('page-capability-known', page, view,
         'Text("必需能力声明新增：', 'if (false) Text("必需能力声明新增：'),
        ('page-capability-unknown', page, view,
         'else Text("旧能力声明查不出来，无法计算完整差异。",', 'else if (false) Text("旧能力声明查不出来，无法计算完整差异。",'),
        ('page-permission-unknown', page, view,
         'if (preview.addedPermissions == null) Text', 'if (false) Text'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-version-changes-mutations' / str(time.time_ns())
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
        logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginLua=true', '-PghostermLuaArchive=' + str(Path(os.environ['GHOSTERM_LUA_ARCHIVE']).resolve(strict=True)),
            ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 900)
        for relative in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
            logged(label + '-install-' + Path(relative).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / relative])
    def test(label, method, green):
        if method.startswith('live#'):
            command = [sys.executable, '-B', ROOT / 'scripts/test-plugin-channel.py', '--rpc',
                '--android-client-runtime', '--android', serial, '--case', 'allow2', '--android-test',
                'com.hanxi.ghosterm.PluginVersionSelectionChannelTest#' + method.split('#', 1)[1]]
            if not green: command += ['--expect-platform-assertion', 'android']
            output = logged(label, command, 300)
            receipt = Path(output.rsplit('PUBLIC CHANNEL PASS; logs: ', 1)[1].strip())
            result = json.loads((receipt / 'result.json').read_text())
            raw = (receipt / 'android.log').read_text()
            if ('INSTRUMENTATION_STATUS: numtests=1' not in raw or
                    result['expected_platform_assertion'] != (None if green else 'android') or
                    (green and 'OK (1 test)' not in raw)):
                raise RuntimeError('Not accepted actual Session/Runtime assertion:\n' + raw)
            return
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', method,
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        good = ('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(t in output for t in ['java.lang.AssertionError', 'org.junit.ComparisonFailure']) and
            'Process crashed' not in output and 'INSTRUMENTATION_FAILED' not in output)
        expected = {'page-capability-known-target':'plugin-version-capability-diff', 'page-capability-unknown-target':'plugin-version-capability-unknown', 'page-permission-unknown-target':'plugin-version-permission-unknown'}.get(label)
        if not green and expected is not None: good = good and expected in output
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
    print('ANDROID VERSION CHANGES MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
