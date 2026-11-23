#!/usr/bin/env python3
"""Android explicit version removal, persisted intent and reference guards on an explicitly selected device.

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
MARK = 'GT_ANDROID_REMOVAL_MUTATION'
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
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionRemovalModel.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionRemovalContent.kt'
    disk = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseRemoval.kt'
    native = ROOT / 'android/bridge/src/main/cpp/install_jni.c'
    packages = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionPackagesViewModel.kt'
    originals = {p: p.read_bytes() for p in [controller, model, view, disk, packages, native]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    backend = 'com.hanxi.ghosterm.PluginReleaseRemovalTest#'
    api = 'com.hanxi.ghosterm.PluginVersionRemovalControllerTest#'
    draft = 'com.hanxi.ghosterm.PluginVersionRemovalModelTest#'
    page = 'com.hanxi.ghosterm.PluginVersionRemovalUITest#actualSettingsRequiresDisableInventoryAndSeparateDeleteConfirmationThenExplicitlyResumesSavedIntent'
    live = 'live#developmentSessionSelectingSameManagedDigestPinsVersionUntilActualRuntimeReleaseAndRemoval'
    cases = [
        ('exclusive-version', backend + 'activeSelectionAndSharedRuntimeReferencesPreventPreparation', disk,
         'try {\n                val error = PluginInstallFiles.lock(version.descriptor.fd)', 'try {\n                val error = 0'),
        ('persistent-identity', backend + 'changedPersistentIdentityInSavedIntentCannotAuthorizeAnyUnlink', disk,
         ' && identityKind == other.identityKind && born == other.born && bornNS == other.bornNS', ''),
        ('intent-sync-before-detach', backend + 'parentSyncFailurePreservesOriginalAndIntentForExplicitRetry', disk,
         'sync(root.fileDescriptor); checkpoint(if (detached)', 'checkpoint(if (detached)'),
        ('confirmation-ack', api + 'previewNeedsNoPolicyOrSourceGrantButDeletionRequiresItsExactTokenAndAcknowledgement', controller,
         'check(acknowledged) { "请核对完整文件清单并单独确认删除" }', 'check(true) { "请核对完整文件清单并单独确认删除" }'),
        ('exact-preview-token', api + 'previewNeedsNoPolicyOrSourceGrantButDeletionRequiresItsExactTokenAndAcknowledgement', controller,
         'pendingRemoval?.takeIf { it.first == id }?.second', 'pendingRemoval?.second'),
        ('cancel-releases-reference', api + 'cancellationCloseAndOverlappingSelectionCannotLeakOrReplacePreviewReferences', controller,
         'if (pendingRemoval?.first == id) { pendingRemoval?.second?.close(); pendingRemoval = null }', 'if (pendingRemoval?.first == id) { pendingRemoval = null }'),
        ('forget-exact-cached-package', api + 'disabledVersionRemovalForgetsOnlyItsOwnDigestAndNeverRestoresDevelopmentFallback', controller,
         'if (!result.complete || result.stage != null || result.unclassified != null) return result', 'if (true) return result'),
        ('resume-ack', api + 'pendingRecordIsReadOnlyAcrossControllerRestartAndRequiresSeparateResumeAcknowledgement', controller,
         'check(acknowledged) { "请核对保留的删除意图并确认继续" }', 'check(true) { "请核对保留的删除意图并确认继续" }'),
        ('withdraw-ack', api + 'cancellationBeforeIntentWritesNothingAndAfterIntentPreservesActualOutcome', controller,
         'check(acknowledged) { "请确认撤销尚未开始移除的删除意图" }', 'check(true) { "请确认撤销尚未开始移除的删除意图" }'),
        ('pre-intent-cancellation', api + 'cancellationBeforeIntentWritesNothingAndAfterIntentPreservesActualOutcome', controller,
         'confirmationCheckpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }',
         'confirmationCheckpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase) }'),
        ('preview-ack', draft + 'observationNeverMutatesAndPreviewNeedsItsOwnFreshAcknowledgement', model,
         'preview != null && acknowledged', 'preview != null'),
        ('saved-ack', draft + 'savedIntentRequiresExplicitAcknowledgedResumeOrWithdrawalAndIsNeverAutomaticallyReplayed', model,
         'saved != null && acknowledged', 'saved != null'),
        ('refresh-no-replay', draft + 'savedIntentRequiresExplicitAcknowledgedResumeOrWithdrawalAndIsNeverAutomaticallyReplayed', model,
         'try { val value = observe(); if', 'try { val value = observe()?.also { resume(it.id) }; if'),
        ('unknown-is-not-empty', draft + 'corruptObservationDoesNotBecomeNoPendingIntentOrAllowPreview', model,
         'catch (failure: Exception) { if (!closed && generation === epoch) issue = message(failure) }',
         'catch (failure: Exception) { if (!closed && generation === epoch) { saved = null; loaded = true } }'),
        ('known-cancel-token', draft + 'refreshAndCancelReleasePreviewAndResetAcknowledgementWithoutWriting', model,
         'requestID?.let(cancelPreview);', 'requestID?.let { Unit };'),
        ('late-preview', draft + 'closeCancelsKnownTokenBeforeLatePreviewAndRejectsLateResult', model,
         'if (!closed && generation === epoch) preview = value else cancelPreview(value.id)', 'if (true) preview = value else cancelPreview(value.id)'),
        ('actual-effect-counts', draft + 'commitFreezesActionsAndPreservesPartialEffectReceiptAndSavedIntent', model,
         'result = value; withdrawalDurability', 'result = value?.copy(removed = 0, observedAbsent = 0); withdrawalDurability'),
        ('page-ack', page, view,
         'onClick = model::confirm, enabled = model.canConfirm', 'onClick = model::confirm, enabled = true'),
        ('page-inventory', page, view,
         'items(value.members, key', 'items(value.members.take(0), key'),
        ('page-forget-metadata', page, packages,
         'entries = entries.filterNot { (it.review.id to it.review.digest) in removed }', 'entries = entries.map { it }'),
        ('adopted-development-lease', live, controller,
         'if (run.sessionPackageKey == key && run.sessionVersionLease == null) sessionReferences[run] = pending.version.borrow()', 'Unit'),
        ('native-no-overwrite', 'com.hanxi.ghosterm.PluginRemovalFilesTest#detachNeverOverwritesEmptyOrPopulatedTombstone', native,
         'selected.st_mode != named.st_mode || selected.st_dev != root.st_dev)) {\n        error = EINVAL;\n    }\n    if (!error && syscall(__NR_renameat2, parent, source, parent, target, RENAME_NOREPLACE)', 'selected.st_mode != named.st_mode || selected.st_dev != root.st_dev)) {\n        error = EINVAL;\n    }\n    if (!error && syscall(__NR_renameat2, parent, source, parent, target, 0)'),
        ('native-frozen-version', 'com.hanxi.ghosterm.PluginRemovalFilesTest#detachedSourceReplacementAndWritableDirectoryAreRejected', native,
         '\n    JNIEnv *env, jobject self, jint parent, jint descriptor, jbyteArray from, jbyteArray to) {\n    (void)self;\n    char source[65] = {0}, target[47] = {0};\n    int length = from ? (*env)->GetArrayLength(env, from) : 0;\n    int preparing = length == 45;\n    if (!from || !to || (!preparing && length != 64) || (*env)->GetArrayLength(env, to) != 46) {\n        return EINVAL;\n    }\n    (*env)->GetByteArrayRegion(env, from, 0, length, (jbyte *)source);\n    (*env)->GetByteArrayRegion(env, to, 0, 46, (jbyte *)target);\n    if ((*env)->ExceptionCheck(env) || memcmp(target, ".removing-", 10)) {\n        return EINVAL;\n    }\n    if (preparing && memcmp(source, ".prepare-", 9)) {\n        return EINVAL;\n    }\n    for (int i = preparing ? 9 : 0; i < length; i++) {\n        int separator = preparing && (i == 17 || i == 22 || i == 27 || i == 32);\n        if (separator ? source[i] != \'-\'\n                      : !((source[i] >= \'0\' && source[i] <= \'9\') ||\n                          (source[i] >= \'a\' && source[i] <= \'f\'))) {\n            return EINVAL;\n        }\n    }\n    for (int i = 10; i < 46; i++) {\n        int separator = i == 18 || i == 23 || i == 28 || i == 33;\n        if (separator ? target[i] != \'-\'\n                      : !((target[i] >= \'0\' && target[i] <= \'9\') ||\n                          (target[i] >= \'a\' && target[i] <= \'f\'))) {\n            return EINVAL;\n        }\n    }\n    struct stat root, selected, named;\n    int error = private_directory(parent, &root);\n    if (!error) {\n        error = private_directory(descriptor, &selected);\n    }\n    if (!error && !preparing && (selected.st_mode & 0222)) {\n        error = EINVAL;\n    }\n', '\n    JNIEnv *env, jobject self, jint parent, jint descriptor, jbyteArray from, jbyteArray to) {\n    (void)self;\n    char source[65] = {0}, target[47] = {0};\n    int length = from ? (*env)->GetArrayLength(env, from) : 0;\n    int preparing = length == 45;\n    if (!from || !to || (!preparing && length != 64) || (*env)->GetArrayLength(env, to) != 46) {\n        return EINVAL;\n    }\n    (*env)->GetByteArrayRegion(env, from, 0, length, (jbyte *)source);\n    (*env)->GetByteArrayRegion(env, to, 0, 46, (jbyte *)target);\n    if ((*env)->ExceptionCheck(env) || memcmp(target, ".removing-", 10)) {\n        return EINVAL;\n    }\n    if (preparing && memcmp(source, ".prepare-", 9)) {\n        return EINVAL;\n    }\n    for (int i = preparing ? 9 : 0; i < length; i++) {\n        int separator = preparing && (i == 17 || i == 22 || i == 27 || i == 32);\n        if (separator ? source[i] != \'-\'\n                      : !((source[i] >= \'0\' && source[i] <= \'9\') ||\n                          (source[i] >= \'a\' && source[i] <= \'f\'))) {\n            return EINVAL;\n        }\n    }\n    for (int i = 10; i < 46; i++) {\n        int separator = i == 18 || i == 23 || i == 28 || i == 33;\n        if (separator ? target[i] != \'-\'\n                      : !((target[i] >= \'0\' && target[i] <= \'9\') ||\n                          (target[i] >= \'a\' && target[i] <= \'f\'))) {\n            return EINVAL;\n        }\n    }\n    struct stat root, selected, named;\n    int error = private_directory(parent, &root);\n    if (!error) {\n        error = private_directory(descriptor, &selected);\n    }\n    if (0) {\n        error = EINVAL;\n    }\n'),
        ('adopted-development-runtime-lease', 'live#developmentRuntimeSelectingSameManagedDigestPinsVersionUntilActualRuntimeReleaseAndRemoval', controller,
         'if (run.runtimePackageKey == key && run.runtimeVersionLease == null) runtimeReferences[run] = pending.version.borrow()', 'Unit'),
        ('absent-receipt-revision', backend + 'fullyAbsentTreeCanClearIntentAfterAnUnrelatedExplicitDisableChangesRevision', disk,
         'validate(owner); verifyIntent(); disabled(owner, !detached && version != null)\n            sync(root.fileDescriptor)', 'validate(owner); verifyIntent(); disabled(owner, !detached)\n            sync(root.fileDescriptor)'),
        ('member-sync-effect-count', backend + 'memberSyncFailureCountsAlreadyUnlinkedFilesAndDirectoriesBeforeReturning', disk,
         'demand(LocalPublicationFile.remove(parent.fd, input.fd, name) == 0, Reason.IO)\n                            removed++',
         'demand(LocalPublicationFile.remove(parent.fd, input.fd, name) == 0, Reason.IO)'),
        ('directory-sync-effect-count', backend + 'memberSyncFailureCountsAlreadyUnlinkedFilesAndDirectoriesBeforeReturning', disk,
         'demand(LocalPublicationFile.rmdir(parent.fd, child.fd, name) == 0, Reason.IO)\n                            removed++',
         'demand(LocalPublicationFile.rmdir(parent.fd, child.fd, name) == 0, Reason.IO)'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-removal-mutations' / str(time.time_ns())
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
        expected = {'page-ack-target': 'plugin-removal-confirm',
            'page-inventory-target': 'plugin-removal-member-release.sig'}.get(label)
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
    print('ANDROID REMOVAL MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
