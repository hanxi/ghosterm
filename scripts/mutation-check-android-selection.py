#!/usr/bin/env python3
"""Android signed version selection, disable/rollback, admission and confirmation races on an explicitly selected device.

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
MARK = 'GT_ANDROID_SELECTION_MUTATION'
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
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionSelectionModel.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionSelectionContent.kt'
    packages = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionPackagesViewModel.kt'
    originals = {p: p.read_bytes() for p in [controller, model, view, packages]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    backend = 'com.hanxi.ghosterm.PluginVersionSelectionControllerTest#'
    draft = 'com.hanxi.ghosterm.PluginVersionSelectionModelTest#'
    live = 'live#sameControllerRegistryReplacementPreservesSessionSnapshotAndRuntimeLease'
    page = 'com.hanxi.ghosterm.PluginVersionSelectionUITest#actualSettingsTrustsCurrentPolicyReviewsDifferencesAndSeparatelyConfirmsSelectionDisableAndRollback'
    cases = [
        ('page-source-reset', page, packages,
         'entries = entries.map { if (it.review.id == result.pluginID) it.copy(decision = "尚未批准") else it }', 'entries = entries.map { it }'),
        ('session-generation-admission', backend + 'anotherControllerSameVersionSelectionMakesOldApprovalStaleBeforeArgumentsOrSessionEffects', controller,
         'check(record.source == Source.ALLOWED) { "插件来源尚未获本进程批准" }\n        val admission = managedStatus(record, owner, choices, approval = true)',
         'check(record.source == Source.ALLOWED) { "插件来源尚未获本进程批准" }\n        val admission = ExtensionRegistry.Status.OK'),
        ('page-commit-main', page, model,
         'val id = preview!!.id; val generation = epoch\n        busy = true; committing = true; issue = null\n        operation = scope.launch(Dispatchers.Main.immediate)',
         'val id = preview!!.id; val generation = epoch\n        busy = true; committing = true; issue = null\n        operation = scope.launch'),
        ('session-version-pin', live, controller, 'it.pin(); it.borrow()', 'it.borrow()'),
        ('runtime-version-transfer', live, controller,
         'runtimeVersionLease = sessionVersionLease; sessionVersionLease = null; sessionApprovedSnapshot = false',
         'sessionVersionLease?.close(); sessionVersionLease = null; sessionApprovedSnapshot = false'),
        ('runtime-version-release', live, controller, 'runtimeVersionLease?.close(); runtimeVersionLease = null', 'runtimeVersionLease = null'),
        ('session-approved-snapshot', live, controller, 'check(sessionApprovedSnapshot && record.review.commands.any', 'check(record.source == Source.ALLOWED && record.review.commands.any'),
        ('session-exact-old-package', live, controller, 'val sameRegistration = registeredDigests[pluginID] == packageDigest', 'val sameRegistration = true'),
        ('preview-pin', backend + 'previewPinsOnlyCandidateAndRequiresSeparateAcknowledgementAndSourceApproval', controller, 'candidate.pin()', 'Unit'),
        ('preview-cancel-release', backend + 'previewCancelAndControllerCloseReleaseKnownTokenLease', controller,
         'if (pendingVersion?.value?.id == id) { pendingVersion?.close(); pendingVersion = null }', 'if (pendingVersion?.value?.id == id) { pendingVersion = null }'),
        ('selection-ack', backend + 'previewPinsOnlyCandidateAndRequiresSeparateAcknowledgementAndSourceApproval', controller,
         'check(acknowledged) { "请先审阅完整声明并确认本次切换" }', 'check(true) { "请先审阅完整声明并确认本次切换" }'),
        ('disable-ack', backend + 'disableRequiresCurrentRevisionAndAckThenBlocksAllNewAdmissionUntilReselection', controller,
         'check(acknowledged) { "请确认禁用后续新实例" }', 'check(true) { "请确认禁用后续新实例" }'),
        ('current-policy', backend + 'currentPolicyRevocationIsUsedInsteadOfStoredInstallationPolicy', controller,
         'PluginReleaseSignature.authenticate(raw, version.read("release.sig", 64), policy)', 'PluginReleaseSignature.authenticate(raw, version.read("release.sig", 64), version.read("policy.json", 131072))'),
        ('core-identity', backend + 'authenticatedIdentityMustMatchActualCorePackageNotDeclaredSlotMetadata', controller,
         'if (identity != authenticated.identity)', 'if (false)'),
        ('installed-development-bypass', backend + 'installedDevelopmentCopyCannotBypassMissingSelectionOrDisabledChoice', controller,
         'if (record.review.id !in managedIDs) return ExtensionRegistry.Status.OK', 'if (true) return ExtensionRegistry.Status.OK'),
        ('missing-root-unknown', backend + 'missingRootAndCorruptChoiceNeverRestoreUnmanagedDevelopmentAdmission', controller,
         'if (owner == null) return ExtensionRegistry.Status.STALE', 'if (owner == null) return ExtensionRegistry.Status.DENIED'),
        ('disabled-state', backend + 'disableRequiresCurrentRevisionAndAckThenBlocksAllNewAdmissionUntilReselection', controller,
         'if (!choice.enabled) return ExtensionRegistry.Status.DENIED', 'if (false) return ExtensionRegistry.Status.DENIED'),
        ('selection-generation', backend + 'anotherControllerSameVersionSelectionMakesOldApprovalStaleBeforeArgumentsOrSessionEffects', controller,
         'record.selectionGeneration != choice.generation || ', ''),
        ('source-approval-generation', backend + 'previewPinsOnlyCandidateAndRequiresSeparateAcknowledgementAndSourceApproval', controller,
         'record.approvalGeneration = choices[id]?.generation', 'record.approvalGeneration = null'),
        ('source-reset', backend + 'upgradeAndRollbackUseDifferentDigestsFullPermissionDiffAndFreshSourceApprovals', controller,
         'it.source = Source.UNKNOWN; it.approvalGeneration = null', 'it.approvalGeneration = null'),
        ('registry-exact-replacement', backend + 'upgradeAndRollbackUseDifferentDigestsFullPermissionDiffAndFreshSourceApprovals', controller,
         'if (record.managedReleaseID != null) registeredDigests[id] else ', ''),
        ('previous-unknown', backend + 'missingPreviousVersionRemainsUnknownAndDoesNotClaimEmptyPermissions', controller,
         '} catch (_: Exception) { previous = null; state = PreviousVersionState.UNKNOWN }', '} catch (_: Exception) { previous = null; state = PreviousVersionState.NONE }'),
        ('cancel-before-publication', backend + 'cancellationWhileBeforeRenameCheckpointIsBlockedLeavesOldChoiceAndNoTemporary', controller,
         '                            checkpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }, parentSync = parentSync)',
         '                            checkpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase) }, parentSync = parentSync)'),
        ('cancel-before-disable-publication', backend + 'cancellationWhileBeforeRenameCheckpointIsBlockedLeavesOldChoiceAndNoTemporary', controller,
         '\n                    checkpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase); caller.ensureActive(); checkOpen() }, parentSync = parentSync)',
         '\n                    checkpoint = { phase -> caller.ensureActive(); checkOpen(); checkpoint(phase) }, parentSync = parentSync)'),
        ('policy-trust', draft + 'policyTrustAndSeparatePreviewAcknowledgementAreRequiredBeforeCommit', model,
         'readyForReview && policy != null && trusted', 'readyForReview && policy != null'),
        ('preview-ack', draft + 'policyTrustAndSeparatePreviewAcknowledgementAreRequiredBeforeCommit', model,
         'preview != null && acknowledged && (trusted || preview?.candidate?.channelStatementSHA256 != null)', 'preview != null && (trusted || preview?.candidate?.channelStatementSHA256 != null)'),
        ('disable-preview-ack', draft + 'disableHasItsOwnAcknowledgementAndCancelsCandidateWithoutRequiringPolicy', model,
         'disableTarget != null && acknowledged', 'disableTarget != null'),
        ('trust-withdrawal', draft + 'trustWithdrawalAndNewPolicyInvalidatePreviewAndReleaseKnownRequest', model,
         'invalidate(); trusted = value', 'trusted = value'),
        ('preview-late', draft + 'closingInFlightPreviewCancelsOwnerTokenBeforeLateReturnAndRejectsLateUi', model,
         'if (!closed && generation === epoch) preview = value else cancelRequest(value.id)', 'preview = value'),
        ('preview-cancel-token', draft + 'closingInFlightPreviewCancelsOwnerTokenBeforeLateReturnAndRejectsLateUi', model,
         'requestID?.let(cancelRequest)', 'Unit'),
        ('preview-cancel-work', draft + 'closingInFlightPreviewCancelsOwnerTokenBeforeLateReturnAndRejectsLateUi', model,
         'operation?.cancel()', 'Unit'),
        ('commit-freeze', draft + 'commitFreezesMaterialAcknowledgementAndOtherActionsUntilPublishedReceipt', model,
         'if (closed || committing || value && policy == null)', 'if (closed || value && policy == null)'),
        ('commit-late', draft + 'closedPageIgnoresCommittedResultReturnedAfterCancellation', model,
         'if (!closed && generation === epoch) { result = value; preview = null; requestID = null; changed(value) }', 'run { result = value; preview = null; requestID = null; changed(value) }'),
        ('commit-error-unknown', draft + 'unknownCommitReceiptDoesNotGrantUiApprovalOrOfferUnconfirmedRetry', model,
         '"未取得完整活动选择回执；请刷新列表核对当前选择。"', 'failure.message ?: "未知"'),
        ('page-candidate', page, view, 'VersionDeclarations(preview.candidate, "candidate")', 'Unit'),
        ('page-previous-unknown', page, view, '旧版已用本次策略核验；此前', '旧版权限为空；此前'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-selection-mutations' / str(time.time_ns())
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
        expected = {'page-commit-main-target': 'activity selection failed',
            'page-candidate-target': 'plugin-version-candidate-declarations',
            'page-previous-unknown-target': '旧版已用本次策略核验'}.get(label)
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
    print('ANDROID SELECTION MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
