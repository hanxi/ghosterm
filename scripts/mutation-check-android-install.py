#!/usr/bin/env python3
"""Android signed installation transaction / native no-replace / separate confirmation mutations on an explicitly selected device.

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
MARK = 'GT_ANDROID_INSTALL_MUTATION'
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
    stage = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseStage.kt'
    native = ROOT / 'android/bridge/src/main/cpp/install_jni.c'
    draft = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginArchiveReviewModel.kt'
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginArchiveReviewContent.kt'
    capture = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginArchiveCapture.kt'
    originals = {p: p.read_bytes() for p in [stage, native, draft, controller, view, capture]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    fs = 'com.hanxi.ghosterm.PluginInstallFilesTest#'
    st = 'com.hanxi.ghosterm.PluginReleaseStageTest#'
    model = 'com.hanxi.ghosterm.PluginInstallDraftTest#'
    owner = 'com.hanxi.ghosterm.PluginInstallControllerTest#'
    page = 'com.hanxi.ghosterm.PluginInstallUITest#explicitInstallationPublishesReadonlyVersionWithoutApprovingOrActivating'
    publish = originals[native].decode().split('JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_publishDirectory(', 1)[1].split('JNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_PluginInstallFiles_detachDirectory(', 1)[0]
    record = originals[stage].decode().split('    private fun verifyRecord() {', 1)[1].split('    private fun retireRecord()', 1)[0]
    cases = [
        ('capture-dispatch-owner', 'com.hanxi.ghosterm.PluginArchiveCaptureTest#cancelledDispatchOfCompletedCaptureClosesUndeliveredOwner', capture,
         'finally { result?.close() }', 'finally { Unit }'),
        ('stage-root-sync', st + 'rootEntrySyncMustSucceedBeforeReservationIncludingRetryOfExistingRoot', stage,
         'rootSync(ancestor.fileDescriptor)', 'Unit'),
        ('native-no-replace', fs + 'realDirectoryRenameIsAtomicAndNeverOverwritesExistingEmptySlot', native,
         publish, publish.replace('parent, target, RENAME_NOREPLACE)', 'parent, target, 0)')),
        ('native-frozen', fs + 'writableDirectoryAndRegularFileDescriptorsAreRejected', native,
         '!error && (selected.st_mode & 0222)', '!error && 0'),
        ('native-source-object', fs + 'sourceReplacementAndSymlinkCannotRedirectBorrowedDirectory', native,
         publish, publish.replace('selected.st_ino != named.st_ino || ', '')),
        ('native-no-follow', fs + 'sourceReplacementAndSymlinkCannotRedirectBorrowedDirectory', native,
         publish, publish.replace('source, &named, AT_SYMLINK_NOFOLLOW)', 'source, &named, 0)')),
        ('native-lock', fs + 'locksAreBoundToIndependentOpenDescriptionsAndReleaseOnClose', native,
         '!error && flock(descriptor, LOCK_EX | LOCK_NB) < 0', '!error && 0'),
        ('native-directory', fs + 'writableDirectoryAndRegularFileDescriptorsAreRejected', native,
         '!S_ISDIR(value->st_mode) || ', ''),
        ('native-private', fs + 'invalidComponentsDoNotReachFilesystemAndNeverConsumeCallerFds', native,
         ' || (value->st_mode & 0077)', ''),
        ('stage-reservation', st + 'durableReservationPrecedesDirectoryCreationAndBindsIdentity', stage,
         'writeRecord(null)', 'Unit'),
        ('stage-record-object', st + 'durableReservationPrecedesDirectoryCreationAndBindsIdentity', stage,
         'writeRecord(persistentDirectory)', 'Unit'),
        ('stage-root-object', st + 'replacementRootAndStageCannotRedirectPublicationOrCleanup', stage,
         'demand(privateDirectory(named) && identity(named) == parentIdentity)', 'demand(true)'),
        ('stage-record-proof', st + 'changedIntentPreventsDeletionOfAnyStage', stage,
         '    private fun verifyRecord() {' + record, '    private fun verifyRecord() { }\n'),
        ('stage-stamp', st + 'changedFileAndUnknownEntryArePreservedAndNeverPublished', stage,
         'files[path] == stamp(value)', 'true'),
        ('stage-final-snapshot', st + 'changedMemberDuringFinalAuthorizationCannotPublish', stage,
         '            tree(freeze = false, frozen = true)', '            Unit'),
        ('stage-complete', st + 'missingKnownFileCannotPublishEvenWhenAllRemainingEntriesAreSafe', stage,
         'demand(fileCount == files.size && seen == directories.keys)', 'demand(true)'),
        ('stage-freeze', st + 'publishesCompleteReadonlyTreeAndRetiresIntent', stage,
         'Os.fchmod(current.fileDescriptor, 320)', 'Os.fchmod(current.fileDescriptor, 448)'),
        ('stage-file-mode', st + 'publishesCompleteReadonlyTreeAndRetiresIntent', stage,
         'if (executable) 320 else 256', 'if (executable) 448 else 384'),
        ('stage-frozen-child', st + 'writableNestedDirectoryAfterFreezeIsRejected', stage,
         'if (frozen) demand(Os.fstat(current.fileDescriptor).st_mode and 146 == 0)', 'if (false) demand(Os.fstat(current.fileDescriptor).st_mode and 146 == 0)'),
        ('stage-capacity', st + 'unknownRootEntriesCountAgainstCapacity', stage,
         'names.size < 64', 'true'),
        ('stage-cleanup', st + 'ordinaryCloseDiscardsOnlyOwnedStageAndJournal', stage,
         '!published && directoryIdentity != null', 'false'),
        ('stage-published-owner', st + 'parentSyncFailureAfterRenameKeepsPublishedTreeAndIntent', stage,
         'published = true', 'published = false'),
        ('stage-durability', st + 'parentSyncFailureAfterRenameKeepsPublishedTreeAndIntent', stage,
         'validateDirectory(releaseID)\n        }.isSuccess', 'validateDirectory(releaseID)\n        }.let { true }'),
        ('stage-durable-version', st + 'replacedPublishedVersionRetainsInstallIntent', stage,
         '            validateDirectory(releaseID)', '            Unit'),
        ('stage-retire', st + 'publishesCompleteReadonlyTreeAndRetiresIntent', stage,
         'if (durable) try { retireRecord() }', 'if (false) try { retireRecord() }'),
        ('controller-proof', owner + 'differentAuthenticatedProofCannotReplaceConfirmedMaterialSnapshot', controller,
         'check(authenticated.proof == expected.proof)', 'check(true)'),
        ('controller-declarations', owner + 'changedPreviewAndDeclaredPermissionsCannotBeInstalled', controller,
         'check(fresh == expected)', 'check(true)'),
        ('controller-cancellation', owner + 'prePublicationCancellationWaitsForOwnedCleanup', controller,
         'val raw = descriptor.copyOf(); val signed = signature.copyOf(); val policy = trustedPolicy.copyOf()\n        val caller = currentCoroutineContext()', 'val raw = descriptor.copyOf(); val signed = signature.copyOf(); val policy = trustedPolicy.copyOf()\n        val caller = kotlin.coroutines.EmptyCoroutineContext'),
        ('controller-descriptor', owner + 'installsReadonlySignedVersionAndReopensWithFreshExplicitPolicy', controller,
         'stage.write("release.json", raw)', 'Unit'),
        ('controller-signature', owner + 'installsReadonlySignedVersionAndReopensWithFreshExplicitPolicy', controller,
         'stage.write("release.sig", signed)', 'Unit'),
        ('controller-policy', owner + 'installsReadonlySignedVersionAndReopensWithFreshExplicitPolicy', controller,
         'stage.write("policy.json", policy)', 'Unit'),
        ('draft-acknowledgement', model + 'installationRequiresVerifiedSnapshotAndSeparateExplicitAcknowledgement', draft,
         '&& installAcknowledged && installation == null', '&& installation == null'),
        ('draft-invalidate', model + 'inputAndPolicyChangesInvalidateInstallationAcknowledgement', draft,
         'installAcknowledged = false; installation = null', 'installation = null'),
        ('draft-freeze-policy', model + 'installationFreezesPolicyAndRejectsAnotherPickerUntilReceipt', draft,
         'if (installing) return', 'if (false) return'),
        ('draft-freeze-ack', model + 'installationFreezesPolicyAndRejectsAnotherPickerUntilReceipt', draft,
         'closed || busy || receipt == null || (!useConfiguredChannel && !trustedPolicy) || installation != null', 'closed || receipt == null || (!useConfiguredChannel && !trustedPolicy) || installation != null'),
        ('draft-fresh-retry', model + 'unknownReceiptIsVisibleAndRequiresFreshAcknowledgementForRetry', draft,
         'finally { installAcknowledged = false; busy = false; installing = false }', 'finally { busy = false; installing = false }'),
        ('draft-completion', model + 'installationFreezesPolicyAndRejectsAnotherPickerUntilReceipt', draft,
         'finally { installAcknowledged = false; busy = false; installing = false }', 'finally { installAcknowledged = false; busy = false }'),
        ('draft-unknown-error', model + 'unknownReceiptIsVisibleAndRequiresFreshAcknowledgementForRetry', draft,
         'else -> "未取得完整安装回执；请核对是否有版本、暂存或归属记录保留。"', 'else -> failure.message'),
        ('draft-durability', model + 'publishedButUnconfirmedDurabilityIsKeptAsPublishedResult', draft,
         'if (!closed && generation === epoch) installation = value', 'if (!closed && generation === epoch && value.publication.durabilityConfirmed) installation = value'),
        ('page-confirmation', page, view, 'onValueChange = model::acknowledgeInstall', 'onValueChange = {}'),
        ('page-install', page, view, 'onClick = model::installArchive', 'onClick = {}'),
        ('page-receipt', page, view, 'model.installation?.let { installed ->', 'model.installation?.takeIf { false }?.let { installed ->'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-install-mutations' / str(time.time_ns())
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
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', method,
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        good = ('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(t in output for t in ['java.lang.AssertionError', 'org.junit.ComparisonFailure']) and
            'Process crashed' not in output and 'INSTRUMENTATION_FAILED' not in output)
        expected = {'capture-dispatch-owner-target': 'cancelled result dispatch retained a capture fd',
            'page-confirmation-target': 'Failed to assert the following: (is enabled)',
            'page-install-target': 'installation produced no visible receipt',
            'page-receipt-target': 'installation produced no visible receipt',
            'controller-proof-target': 'mismatched proof reached persistent reservation',
            'controller-declarations-target': 'changed preview was accepted',
            'controller-cancellation-target': 'cancelled install left owned state',
            'stage-durable-version-target': 'A replaced version cannot report durable installation',
            'stage-final-snapshot-target': 'A member changed during final authorization must not publish',
            'controller-descriptor-target': 'signed installation metadata missing: release.json',
            'controller-signature-target': 'signed installation metadata missing: release.sig',
            'controller-policy-target': 'signed installation metadata missing: policy.json'}.get(label)
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
    print('ANDROID INSTALL MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
