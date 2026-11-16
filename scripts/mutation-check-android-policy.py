#!/usr/bin/env python3
"""Android current policy review and explicit atomic subset-disable guards on an explicitly selected device.

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
MARK = 'GT_ANDROID_POLICY_MUTATION'
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
    review = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginPolicyReview.kt'
    catalog = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseCatalog.kt'
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginPolicyReviewModel.kt'
    selection = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginVersionSelectionModel.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginPolicyReviewContent.kt'
    page_view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginInstalledVersionsContent.kt'
    originals = {p: p.read_bytes() for p in [controller, review, catalog, model, selection, view, page_view]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    data = 'com.hanxi.ghosterm.PluginPolicyReviewTest#'
    core = 'com.hanxi.ghosterm.PluginPolicyReviewControllerTest#'
    draft = 'com.hanxi.ghosterm.PluginPolicyReviewModelTest#'
    page = 'com.hanxi.ghosterm.PluginPolicyReviewUITest#actualSettingsTrustsPolicyShowsThreeStatesAndDisablesOnlyAFreshExplicitSubset'
    cases = [
        ('policy-copy', data+'acceptedRejectedAndUnknownRowsStayDistinctAndDisabledChoicesAreNotReviewed', review,
         'val trusted = policy.copyOf()', 'val trusted = policy'),
        ('empty-policy', data+'policyIsStrictlyValidatedBeforeEvenAnEmptyEnabledList', review,
         'val channel = PluginReleaseSignature.policyChannel(trusted)', 'val channel = "fixture"'),
        ('unknown-not-accepted', core+'tamperedPayloadCannotBorrowOldSignatureAndReadFailureStaysUnknown', review,
         'Entry(pluginID, choice.releaseID, State.UNKNOWN,', 'Entry(pluginID, choice.releaseID, State.ACCEPTED,'),
        ('current-policy', core+'revokedPolicyExplicitDisablePersistsAcrossControllersAndDoesNotDeleteOrGrantAnything', review,
         'PluginReleaseSignature.authenticate(raw, version.read("release.sig", 64), trusted)', 'PluginReleaseSignature.authenticate(raw, version.read("release.sig", 64), version.read("policy.json", 131072))'),
        ('choice-identity', data+'acceptedRejectedAndUnknownRowsStayDistinctAndDisabledChoicesAreNotReviewed', review,
         ' || identity.id != pluginID', ''),
        ('core-capture', core+'tamperedPayloadCannotBorrowOldSignatureAndReadFailureStaysUnknown', review,
         'val identity = inspect(payload.fd)', 'val identity = authenticated.identity'),
        ('core-bind', core+'validSignatureWithWrongCoreIdentityIsRejectedRatherThanAcceptedOrUnknown', review,
         'identity != authenticated.identity || ', ''),
        ('root-identity', data+'policyRawBytesAndStrongRootIdentityAreRecheckedBeforeDisable', review,
         'root != null && root.sameObject(rootStamp(owner))', 'true'),
        ('policy-hash', data+'policyRawBytesAndStrongRootIdentityAreRecheckedBeforeDisable', review,
         'PluginReleaseCatalog.sha(policy) == value.policySHA256', 'true'),
        ('accepted-subset', data+'emptyDuplicateAcceptedAndUnlistedSubsetsCannotWriteAnything', review,
         'it.pluginID == id && it.state != State.ACCEPTED', 'it.pluginID == id'),
        ('batch-generation', data+'explicitSubsetPublishesOnceRotatesOnlySelectedGenerationAndNeverEnablesOrDeletes', catalog,
         'choice.copy(enabled = false, generation = UUID.randomUUID().toString())', 'choice.copy(enabled = false)'),
        ('actual-disable', 'live#sameControllerPolicyReviewDisablePreservesRuntimeAndLease', catalog,
         'choice.copy(enabled = false, generation = UUID.randomUUID().toString())', 'choice.copy(enabled = true, generation = UUID.randomUUID().toString())'),
        ('batch-final-cas', data+'finalCatalogCASDoesNotOverwriteAnotherChangeDuringCommit', catalog,
         'demand(selection().revision == expected, Reason.CONFLICT)', 'demand(true)'),
        ('ticket-ack', core+'knownTicketNeedsAcknowledgementAndRejectsWrongIDCancellationAndReplay', controller,
         'check(acknowledged) { "请确认仅禁用已勾选的活动选择" }', 'check(true) { "请确认仅禁用已勾选的活动选择" }'),
        ('ticket-id', core+'knownTicketNeedsAcknowledgementAndRejectsWrongIDCancellationAndReplay', controller,
         'it.value.id == id && policyRequest == id', 'true'),
        ('ticket-late', core+'cancellationDuringIndependentCoreInspectionCannotInstallLateTicketOrLeakReferences', controller,
         'check(policyRequest == id) { "策略复核已取消或过期" }', 'check(true) { "策略复核已取消或过期" }'),
        ('model-trust', draft+'trustIsRequiredAndOnlyExplicitUnacceptedSubsetCanBeCommitted', model,
         'policy != null && trusted', 'policy != null'),
        ('model-empty', draft+'trustIsRequiredAndOnlyExplicitUnacceptedSubsetCanBeCommitted', model,
         '&& selectedIDs.isNotEmpty()', ''),
        ('model-accepted', draft+'trustIsRequiredAndOnlyExplicitUnacceptedSubsetCanBeCommitted', model,
         'it.pluginID == id && it.state != PluginPolicyReview.State.ACCEPTED', 'it.pluginID == id'),
        ('model-reset', draft+'cancelTrustAndPolicyChangesClearEverySelectionAndKnownTicket', model,
         'review = null; selectedIDs = emptySet(); issue = null; result = null', 'review = null; issue = null; result = null'),
        ('model-late', draft+'closeCancelsKnownRequestAndRejectsLateInspectionResult', model,
         'if (!closed && generation === epoch && trusted) review = value else cancelRequest(value.id)', 'if (true) review = value else cancelRequest(value.id)'),
        ('picker-sync', draft+'actualPolicyPickerAndTrustModelSynchronouslyInvalidateAuditBeforeAnyLateRead', selection,
         'invalidate(); policy = null; trusted = false; policyChanged(null, false)', 'invalidate(); policy = null; trusted = false'),
        ('model-freeze', draft+'commitFreezesPolicySubsetAndCancellationAndPreservesVisibleUnsyncedReceipt', model,
         'main(); if (closed || committing) return', 'main(); if (closed) return'),
        ('model-retry', draft+'lostReceiptClearsTicketAndCannotBlindlyApplyAgain', model,
         'cancelRequest(id); requestID = null; review = null; selectedIDs = emptySet(); issue = message(failure, true)', 'cancelRequest(id); requestID = null; selectedIDs = emptySet(); issue = message(failure, true)'),
        ('page-entry', page, page_view, 'PluginPolicyReviewContent(policyModel,', 'if (false) PluginPolicyReviewContent(policyModel,'),
        ('page-subset', page, view, 'enabled = model.canApply && canAct', 'enabled = canAct'),
        ('page-accepted', page, view, 'if (entry.state != PluginPolicyReview.State.ACCEPTED) Row', 'if (true) Row'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-policy-mutations' / str(time.time_ns())
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
        expected = {'page-entry-target':'plugin-policy-review-open', 'page-subset-target':'plugin-policy-review-apply', 'page-accepted-target':'plugin-policy-select-org.ghosterm.signature'}.get(label)
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
    print('ANDROID POLICY MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
