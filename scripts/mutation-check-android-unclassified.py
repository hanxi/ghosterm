#!/usr/bin/env python3
"""Android unknown-directory full review, extra consent and format-three intent guards on an explicitly selected device.

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
MARK = 'GT_ANDROID_UNCLASSIFIED_MUTATION'
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
    unknown = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginUnclassifiedDirectories.kt'
    disk = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseRemoval.kt'
    journal = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstallJournal.kt'
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionRemovalModel.kt'
    read_model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginUnclassifiedModel.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionRemovalContent.kt'
    page_view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstalledVersionsContent.kt'
    native = ROOT / 'android/bridge/src/main/cpp/install_jni.c'
    originals = {p: p.read_bytes() for p in [controller, unknown, disk, journal, model, read_model, view, page_view, native]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    store = 'com.hanxi.ghosterm.PluginUnclassifiedDirectoriesTest#'
    draft = 'com.hanxi.ghosterm.PluginUnclassifiedModelTest#'
    page = 'com.hanxi.ghosterm.PluginUnclassifiedUITest#actualSettingsRequiresFullInventoryAndSeparateUnknownAcknowledgementAfterEveryPreview'
    native_unknown = originals[native].decode().split('Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_detachUnclassifiedDirectory(', 1)[1]
    cases = [
        ('reserved-names', store+'reservedNamesAreRejectedByListingReferenceAndNativeMove', unknown,
         'listOf(".install-", ".intent-", ".selection-", ".removal-record-").none(name::startsWith)', 'true'),
        ('observation-token', store+'observationTokenBindsParentNameAndFullDirectoryStamp', unknown,
         'return PluginReleaseCatalog.sha((parent + name + "\\u0000" + value.json().toString()).toByteArray(Charsets.UTF_8))', 'return "a".repeat(64)'),
        ('directory-stamp', store+'observationTokenBindsParentNameAndFullDirectoryStamp', unknown,
         ' + value.json().toString()', ' + ""'),
        ('extra-native-ack', store+'fullMutableTreeRequiresExtraNativeAcknowledgementAndDoesNotGrantOrigin', disk,
         'demand(header.unclassified == null || acknowledgedUnclassified, Reason.INVALID)\n        confirmationCheckpoint?',
         'demand(true)\n        confirmationCheckpoint?'),
        ('extra-resume-ack', store+'formatThreeResumeNeedsExtraAckAndPreservesNewOrChangedMembers', disk,
         'demand(header.unclassified == null || acknowledgedUnclassified, Reason.INVALID)\n                demand(header.operation',
         'demand(true)\n                demand(header.operation'),
        ('enabled-preview', store+'enabledBrokenVersionCannotBypassDisableThroughUnknownEntry', disk,
         'if (owner.selection().choices.values.any { it.enabled && it.releaseID == reference.name }) throw Rejected(Reason.ENABLED)', 'Unit'),
        ('enabled-resume', store+'detachedUnknownStillHonorsNewEnabledSelectionBeforeAnyUnlink', disk,
         'if (selected.choices.values.any { it.enabled && it.releaseID == (header.unclassified?.name ?: releaseID) })',
         'if (header.unclassified == null && selected.choices.values.any { it.enabled && it.releaseID == releaseID })'),
        ('intent-name-binding', store+'beforeDetachIntentCanBeWithdrawnAndNameTamperingIsNeverFollowed', disk,
         'demand(unclassified == null || (unclassified.nameSHA256 == release && PluginUnclassifiedDirectories.token(header.root, unclassified.name, header.version) == unclassified.token), Reason.INVALID)', 'demand(true)'),
        ('known-stages', store+'listingExcludesKnownStagesAndMetadataButPreservesFilesAndLinks', unknown,
         'known += PluginInstallJournal.classifiedNames(root)', 'Unit'),
        ('unknown-selection', store+'corruptSelectionOrOwnershipCannotTurnIntoDeletionAuthority', unknown,
         'owner.selection()\n', 'Unit\n'),
        ('unknown-ownership', store+'corruptSelectionOrOwnershipCannotTurnIntoDeletionAuthority', unknown,
         'known += PluginInstallJournal.classifiedNames(root)', 'known += runCatching { PluginInstallJournal.classifiedNames(root) }.getOrDefault(emptySet())'),
        ('native-no-overwrite', store+'nativeUnknownMoveDoesNotOverwriteOrFollowReplacementOrConsumeBorrowedFD', native,
         native_unknown, native_unknown.replace('parent, target_name, RENAME_NOREPLACE)', 'parent, target_name, 0)')),
        ('native-source-object', store+'nativeUnknownMoveDoesNotOverwriteOrFollowReplacementOrConsumeBorrowedFD', native,
         'candidate.st_ino != original.st_ino || ', ''),
        ('native-no-follow', store+'nativeUnknownMoveDoesNotOverwriteOrFollowReplacementOrConsumeBorrowedFD', native,
         'source_name, &original, AT_SYMLINK_NOFOLLOW)', 'source_name, &original, 0)'),
        ('native-reserved', store+'reservedNamesAreRejectedByListingReferenceAndNativeMove', native,
         '!strncmp(source_name, ".selection-", 11)', '0'),
        ('model-extra-ack', draft+'unknownPreviewRequiresBothAcknowledgementsAndCancelResetsTheExtraOne', model,
         '&& (preview?.unclassified == null || acknowledgedUnclassified)', ''),
        ('model-resume-ack', draft+'unknownSavedIntentRequiresFreshExtraAcknowledgementForExplicitResume', model,
         '&& (saved?.unclassified == null || acknowledgedUnclassified)', ''),
        ('model-reset-ack', draft+'unknownPreviewRequiresBothAcknowledgementsAndCancelResetsTheExtraOne', model,
         'preview = null; acknowledged = false; acknowledgedUnclassified = false', 'preview = null; acknowledged = false'),
        ('read-unknown', draft+'unknownListingFailureAndLateResultNeverBecomeEmptyOrAnAction', read_model,
         'issue = "未知目录查不出来；未当作空列表或授予删除权限。"', 'entries = emptyList()'),
        ('read-late', draft+'unknownListingFailureAndLateResultNeverBecomeEmptyOrAnAction', read_model,
         'if (!closed && generation === epoch) entries = value', 'if (true) entries = value'),
        ('page-entry', page, page_view, 'PluginUnclassifiedContent(unclassifiedModel,', 'if (false) PluginUnclassifiedContent(unclassifiedModel,'),
        ('page-extra-ack', page, view, 'if (value.unclassified != null) {', 'if (false) {'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-unclassified-mutations' / str(time.time_ns())
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
        expected = {'page-entry-target':'plugin-unclassified-count', 'page-extra-ack-target':'plugin-unclassified-acknowledge'}.get(label)
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
    print('ANDROID UNCLASSIFIED MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
