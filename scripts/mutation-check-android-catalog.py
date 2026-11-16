#!/usr/bin/env python3
"""Android saved-version observation / selection-record transactions / shared directory leases on an explicitly selected device.

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
MARK = 'GT_ANDROID_CATALOG_MUTATION'
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
    catalog = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseCatalog.kt'
    native = ROOT / 'android/bridge/src/main/cpp/install_jni.c'
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstalledVersionsModel.kt'
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstalledVersionsContent.kt'
    settings = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'
    originals = {p: p.read_bytes() for p in [catalog, native, model, controller, view, settings]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    store = 'com.hanxi.ghosterm.PluginReleaseCatalogTest#'
    lease = 'com.hanxi.ghosterm.PluginVersionLeaseTest#'
    draft = 'com.hanxi.ghosterm.PluginInstalledVersionsModelTest#'
    owner = 'com.hanxi.ghosterm.PluginCatalogControllerTest#independentObserverKeepsParentSnapshotAndReportsBusyInsteadOfEmpty'
    page = 'com.hanxi.ghosterm.PluginCatalogUITest#actualSettingsReadsSavedVersionsAndUnknownSelectionWithoutLoadingOrDeletingAnything'
    read = '        private fun readFile(' + originals[catalog].decode().split('        private fun readFile(', 1)[1]
    unbounded = read.replace(' || before.st_size !in 0L..limit.toLong()', '').replace('count < 0 || result.size() > limit - count', 'count < 0')
    cases = [
        ('native-reference', lease + 'sharedReferenceSurvivesOriginalOwnerUntilLastBorrowedFdCloses', native,
         'if (!error && ((value.st_mode & 0222) || flock(descriptor, LOCK_SH | LOCK_NB) < 0))', 'if (0 && !error && ((value.st_mode & 0222) || flock(descriptor, LOCK_SH | LOCK_NB) < 0))'),
        ('native-shared', lease + 'independentSharedReadersCoexistAndExclusiveOwnerWaitsForBoth', native,
         'flock(descriptor, LOCK_SH | LOCK_NB)', 'flock(descriptor, LOCK_EX | LOCK_NB)'),
        ('native-frozen-reference', lease + 'writableDirectoryRegularFileAndBadFdReturnErrnoWithoutConsumingBorrowedFd', native,
         '((value.st_mode & 0222) || flock(descriptor, LOCK_SH | LOCK_NB) < 0)', '(0 || flock(descriptor, LOCK_SH | LOCK_NB) < 0)'),
        ('version-pin', store + 'versionReferenceAndBorrowSurviveVersionAndCatalogOwnerClosure', catalog,
         'if (PluginInstallFiles.pin(descriptor.fd) != 0)', 'if (false)'),
        ('version-borrow', store + 'versionReferenceAndBorrowSurviveVersionAndCatalogOwnerClosure', catalog,
         'ParcelFileDescriptor.dup(descriptor.fileDescriptor)', 'descriptor'),
        ('root-object', store + 'replacedRootCannotReceiveWriteOrLoseUnknownData', catalog,
         'demand(privateDirectory(named) && objectIdentity(named) == identity, Reason.CHANGED)', 'demand(true)'),
        ('root-sync', store + 'rootEntrySyncFailureCannotWriteSelection', catalog,
         'rootSync(ancestor.fileDescriptor)', 'Unit'),
        ('version-object', store + 'versionAndPayloadReplacementsCannotBorrowOpenedIdentities', catalog,
         'demand(named.identity == version.identity, Reason.CHANGED)', 'demand(true)'),
        ('payload-object', store + 'versionAndPayloadReplacementsCannotBorrowOpenedIdentities', catalog,
         'demand(objectIdentity(Os.fstat(named.fileDescriptor)) == objectIdentity(Os.fstat(payload.fileDescriptor)), Reason.CHANGED)', 'demand(true)'),
        ('frozen-version', store + 'symlinkVersionAndWritableDirectoryAreNotVersions', catalog,
         'privateDirectory(v) && v.st_mode and 146 == 0', 'privateDirectory(v)'),
        ('metadata-digest', store + 'descriptorDigestAndAllReadonlyEvidenceAreRequired', catalog,
         'sha(raw) == name && ', ''),
        ('metadata-signature', store + 'descriptorDigestAndAllReadonlyEvidenceAreRequired', catalog,
         'version.read("release.sig", 64).size == 64', 'version.read("release.sig", 64).size <= 64'),
        ('metadata-policy-evidence', store + 'descriptorDigestAndAllReadonlyEvidenceAreRequired', catalog,
         'version.read("policy.json", 131072)', 'Unit'),
        ('metadata-mode', store + 'descriptorDigestAndAllReadonlyEvidenceAreRequired', catalog,
         'before.st_mode and 191 != 0', 'false'),
        ('metadata-limits', store + 'descriptorDigestAndAllReadonlyEvidenceAreRequired', catalog, read, unbounded),
        ('inventory-limit', store + 'choiceCapacityAndInventoryCapacityAreBounded', catalog,
         'demand(names.size <= 128, Reason.LIMIT)', 'demand(true)'),
        ('initial-cas', store + 'absentAndOldRevisionsCannotOverwriteANewerSelectionOrAba', catalog,
         'demand(previous.revision == expected, Reason.CONFLICT)', 'demand(true)'),
        ('commit-cas', store + 'changedRecordAtCommitIsPreservedAndOwnTemporaryIsRemoved', catalog,
         'demand(selection().revision == expected, Reason.CONFLICT)', 'demand(true)'),
        ('write-limit', store + 'choiceCapacityAndInventoryCapacityAreBounded', catalog,
         'demand(proposed.size <= 64, Reason.LIMIT)', 'demand(true)'),
        ('approval-generation', store + 'updatesPreserveOtherChoicesAndAlwaysRotateApprovalGenerationAndRevision', catalog,
         'Choice(releaseID, enabled, UUID.randomUUID().toString())', 'Choice(releaseID, enabled, previous.choices[plugin]?.generation ?: UUID.randomUUID().toString())'),
        ('temporary-stamp', store + 'replacedOrChangedTemporaryIsNotPublishedOrDeleted', catalog,
         'demand(stamp(Os.lstat(path(temporary))) == before && stamp(Os.fstat(input.fileDescriptor)) == before, Reason.CHANGED)', 'demand(true)'),
        ('temporary-cleanup-proof', store + 'replacedOrChangedTemporaryIsNotPublishedOrDeleted', catalog,
         'if (input != null) demand(stamp(Os.fstat(input.fileDescriptor)) == candidateStamp && stamp(Os.lstat(path(temporary))) == candidateStamp, Reason.CHANGED)', 'Unit'),
        ('temporary-cleanup', store + 'failuresBeforeRenameCleanOwnedTemporaryAndLeavePreviousRecord', catalog,
         'demand(LocalPublicationFile.remove(fd.fd, (input ?: output).fd, temporary) == 0, Reason.IO)', 'Unit'),
        ('publication-receipt', store + 'parentSyncAndReceiptFailuresKeepPublishedSelection', catalog,
         'published = true', 'published = false'),
        ('publication-durability', store + 'parentSyncAndReceiptFailuresKeepPublishedSelection', catalog,
         'checkpoint("after-rename"); validate(); parentSync(fd.fileDescriptor) }.isSuccess', 'checkpoint("after-rename"); validate(); parentSync(fd.fileDescriptor) }.let { true }'),
        ('record-unknown', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'selected.getOrNull()', 'selected.getOrElse { Selection(null, emptyMap()) }'),
        ('record-json', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'val value = PluginReleaseSignature.declarationObject(raw)', 'val value = JSONObject(String(raw, Charsets.UTF_8))'),
        ('record-keys', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'keys(value) == setOf("schemaVersion", "nonce", "choices")', 'true'),
        ('record-schema', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'value.opt("schemaVersion") is Number && value.opt("schemaVersion").toString() == "1"', 'true'),
        ('record-nonce', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'uuid(value.opt("nonce") as? String)', 'true'),
        ('record-choice-keys', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'keys(choice) == setOf("releaseID", "enabled", "generation")', 'true'),
        ('record-choice-generation', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'digest(release) && uuid(generation)', 'digest(release)'),
        ('record-choice-limit', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'demand(entries.length() <= 64, Reason.LIMIT)', 'demand(true)'),
        ('record-choice-boolean', store + 'corruptSelectionRemainsUnknownAndCannotBeSilentlyReplaced', catalog,
         'val enabled = choice.opt("enabled") as? Boolean ?: throw Rejected(Reason.INVALID)', 'val enabled = choice.optBoolean("enabled")'),
        ('draft-overlap', draft + 'explicitRefreshClearsPreviousObservationAndRejectsOverlappingRequest', model,
         'if (closed || busy) return', 'if (closed) return'),
        ('draft-stale', draft + 'explicitRefreshClearsPreviousObservationAndRejectsOverlappingRequest', model,
         'busy = true; observation = null; issue = null', 'busy = true; issue = null'),
        ('draft-late', draft + 'closeCancelsRequestAndRejectsResultReturnedAfterCancellation', model,
         'if (!closed && generation === selected) observation = value', 'observation = value'),
        ('draft-cancel', draft + 'closeCancelsRequestAndRejectsResultReturnedAfterCancellation', model,
         'request?.cancel()', 'Unit'),
        ('draft-error', draft + 'failureIsVisibleUnknownAndNeverAnEmptySuccessfulInventory', model,
         'else "无法读取已保存版本；当前状态查不出来。"', 'else failure.message'),
        ('observer-busy', owner, controller,
         'PluginReleaseCatalog.open(directory)?.use { it.observe() }', 'runCatching { PluginReleaseCatalog.open(directory)?.use { it.observe() } }.getOrNull()'),
        ('observer-worker', owner, controller,
         'return ExtensionController(releaseProfile, appContext, versionDirectory)', 'return this'),
        ('page-entry', page, settings, 'PluginInstalledVersionsContent(extensions.controller, extensions::versionSelectionChanged, extensions::versionRemovalChanged)', 'Unit'),
        ('page-unverified', page, view,
         '仅显示已保存的声明，尚未用当前可信策略核验；不授权或执行插件。', '已验证发布者。'),
        ('page-unknown', page, view,
         'selected != null && selected.choices.isEmpty()', 'selected == null || selected.choices.isEmpty()'),
        ('page-refresh', page, view, 'onClick = { selectionModel.cancel(); removalModel.refresh(); maintenanceModel.refresh(); unclassifiedModel.refresh(); model.refresh() }', 'onClick = { selectionModel.cancel(); removalModel.refresh(); maintenanceModel.refresh(); unclassifiedModel.refresh() }'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-catalog-mutations' / str(time.time_ns())
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
        expected = {'page-entry-target': 'saved versions entry missing',
            'page-unverified-target': '尚未',
            'page-unknown-target': 'plugin-versions-no-selection',
            'page-refresh-target': 'plugin-versions-choice-',
            'observer-worker-target': 'observation closed the active parent controller',
            'observer-busy-target': 'locked inventory was treated as empty',
            'initial-cas-target': 'stale selection created a candidate'}.get(label)
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
    print('ANDROID CATALOG MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
