#!/usr/bin/env python3
"""Actual Android signature document/draft/SAF page mutations on an explicitly selected device.

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
MARK = 'GT_ANDROID_SIGNATURE_UI_MUTATION'
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
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginSignatureDocuments.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginSignatureReviewContent.kt'
    settings = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'
    originals = {p: p.read_bytes() for p in [model, view, settings]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    draft = 'com.hanxi.ghosterm.PluginSignatureDraftTest#'
    docs = 'com.hanxi.ghosterm.PluginSignatureDocumentsTest#'
    page = 'com.hanxi.ghosterm.PluginSignatureReviewUITest#realSafMaterialsRequirePolicyConfirmationAndDoNotApproveSource'
    cases = [
        ('explicit-confirmation', draft + 'allMaterialsAndExplicitConfirmationAreRequired', model,
         'pending == null && trustedPolicy && documents.size == 3', 'pending == null && documents.size == 3'),
        ('proof-invalidation', draft + 'descriptorAndSignatureEditsClearProofWhileKeepingSamePolicyDecision', model,
         'epoch = Any(); receipt = null; issue = null', 'epoch = Any(); issue = null'),
        ('policy-reselection', draft + 'policyReselectionClearsProofAndRequiresNewConfirmation', model,
         'if (kind == PluginSignatureMaterial.POLICY) trustedPolicy = false', 'if (kind == PluginSignatureMaterial.POLICY) Unit'),
        ('late-proof', draft + 'withdrawnAndReconfirmedPolicyCannotAcceptLateVerification', model,
         'generation === epoch && trustedPolicy', 'trustedPolicy'),
        ('picker-token', draft + 'requestTokensRejectWrongDraftCancelledAndClosedCallbacks', model,
         'if (closed || pending !== request) return', 'if (closed) return'),
        ('closed-draft', draft + 'requestTokensRejectWrongDraftCancelledAndClosedCallbacks', model,
         'main(); closed = true; invalidate();', 'main(); invalidate();'),
        ('cancel-read', draft + 'closeCancelsReadAndDropsCapturedMaterials', model,
         'documents = emptyMap(); operation?.cancel()', 'documents = emptyMap()'),
        ('failure-visible', draft + 'failuresRemainVisibleAndBusyRequestsCannotOverlap', model,
         'if (!closed && generation === epoch) issue = if (failure is PluginReleaseSignature.Rejected)',
         'if (false) issue = if (failure is PluginReleaseSignature.Rejected)'),
        ('policy-before-selection', draft + 'allMaterialsAndExplicitConfirmationAreRequired', model,
         'if (closed || value && PluginSignatureMaterial.POLICY !in documents) return', 'if (closed) return'),
        ('actual-byte-limit', docs + 'eachRoleEnforcesActualEofLimitsAndEmptySignature', model,
         'demand(count <= kind.limit)', 'demand(true)'),
        ('signature-length', docs + 'eachRoleEnforcesActualEofLimitsAndEmptySignature', model,
         'kind != PluginSignatureMaterial.SIGNATURE || count == 64', 'true'),
        ('descriptor-release', docs + 'actualBytesIgnoreMetadataAndReleaseFileDescriptors', model,
         '.use { owner ->', '.let { owner ->'),
        ('content-only', docs + 'providerStreamTimeoutAndNonContentUriFailExplicitly', model,
         'uri.scheme == "content" && !uri.authority.isNullOrBlank()', 'true'),
        ('read-slots', docs + 'outstandingReadsKeepTheirSlotsUntilActualRelease', model,
         'demand(readers.tryAcquire())', 'demand(true)'),
        ('provider-error', docs + 'providerReportedErrorsRejectTheCapturedPrefix', model,
         'owner.checkError()', 'Unit'),
        ('readonly-fd', docs + 'readWriteProviderHandlesAreRejected', model,
         'flags and OsConstants.O_ACCMODE == OsConstants.O_RDONLY', 'true'),
        ('page-confirmation', page, view,
         'onValueChange = model::trustPolicy', 'onValueChange = {}'),
        ('settings-page', page, settings,
         'PluginSignatureReviewContent(extensions.controller, entry.review)', 'Unit'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-signature-ui-mutations' / str(time.time_ns())
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
    print('ANDROID SIGNATURE UI MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
