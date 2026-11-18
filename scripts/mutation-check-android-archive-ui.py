#!/usr/bin/env python3
"""Actual Android ZIP capture / draft / declarations / SAF page mutations on an explicitly selected device.

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
MARK = 'GT_ANDROID_ARCHIVE_UI_MUTATION'
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
    capture = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginArchiveCapture.kt'
    draft = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginArchiveReviewModel.kt'
    declarations = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseDeclarations.kt'
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    view = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginArchiveReviewContent.kt'
    settings = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'
    signature = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseSignature.kt'
    readers = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginSignatureDocuments.kt'
    originals = {p: p.read_bytes() for p in [capture, draft, declarations, controller, view, settings, signature, readers]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    cap = 'com.hanxi.ghosterm.PluginArchiveCaptureTest#'
    model = 'com.hanxi.ghosterm.PluginArchiveDraftTest#'
    decl = 'com.hanxi.ghosterm.PluginReleaseDeclarationsTest#'
    page = 'com.hanxi.ghosterm.PluginArchiveReviewUITest#actualSafZipAndDeclarationsRequireExplicitTrustAndReleaseOnLeaving'
    cases = [
        ('capture-limit', cap + 'actualLimitAccepts72MiBAndRejectsExtraByteAndShortInput', capture,
         'total <= PluginReleaseArchive.LIMIT - count', 'true'),
        ('capture-short', cap + 'actualLimitAccepts72MiBAndRejectsExtraByteAndShortInput', capture,
         'total >= 22', 'true'),
        ('capture-content', cap + 'nonContentUrisAreRejectedWithoutOpeningProvider', capture,
         'uri.scheme == "content" && !uri.authority.isNullOrBlank()', 'true'),
        ('capture-readonly', cap + 'providerFailureAndWritableHandlesDoNotKeepNamedCacheOrDescriptors', capture,
         'flags and OsConstants.O_ACCMODE == OsConstants.O_RDONLY', 'true'),
        ('capture-provider-error', cap + 'providerFailureAndWritableHandlesDoNotKeepNamedCacheOrDescriptors', capture,
         'input.checkError()', 'Unit'),
        ('capture-owner-close', cap + 'providerFailureAndWritableHandlesDoNotKeepNamedCacheOrDescriptors', capture,
         'closed = true; owner.close()', 'closed = true; Unit'),
        ('capture-input-close', cap + 'providerFailureAndWritableHandlesDoNotKeepNamedCacheOrDescriptors', capture,
         '.use { input ->', '.let { input ->'),
        ('capture-flags', cap + 'sharedInputStatusFlagsAreRestoredAfterCapture', capture,
         'finally { FileDescriptorFlags.status(fd, flags) }', 'finally { Unit }'),
        ('capture-slots', cap + 'archiveAndSmallMaterialsShareTwoSlotsUntilActualIoExit', capture,
         'PluginSignatureDocuments.acquireArchiveSlot()', 'true'),
        ('capture-slot-release', cap + 'archiveAndSmallMaterialsShareTwoSlotsUntilActualIoExit', capture,
         'finally { PluginSignatureDocuments.releaseArchiveSlot() }', 'finally { Unit }'),
        ('capture-cleanup', cap + 'providerFailureAndWritableHandlesDoNotKeepNamedCacheOrDescriptors', capture,
         'demand(!file.exists() || file.delete(), "ZIP 私有快照清理失败。")', 'Unit'),
        ('capture-dispatch-cancel', cap + 'cancelledDispatchOfCompletedCaptureClosesUndeliveredOwner', capture,
         'finally { result?.close() }', 'finally { Unit }'),
        ('draft-trust', model + 'requiresArchiveAllMaterialsAndExplicitPolicyConfirmation', draft,
         'useConfiguredChannel || trustedPolicy && PluginSignatureMaterial.POLICY in documents', 'useConfiguredChannel || PluginSignatureMaterial.POLICY in documents'),
        ('draft-archive', model + 'requiresArchiveAllMaterialsAndExplicitPolicyConfirmation', draft,
         'archive != null && PluginSignatureMaterial.DESCRIPTOR in documents', 'PluginSignatureMaterial.DESCRIPTOR in documents'),
        ('draft-reselect-close', model + 'archiveReselectionReleasesOldOwnerAndCancellationClearsSlot', draft,
         'if (kind == PluginArchiveMaterial.ARCHIVE) { archive?.close(); archive = null }', 'if (kind == PluginArchiveMaterial.ARCHIVE) { archive = null }'),
        ('draft-close', model + 'closeReleasesSelectedCaptureAndClearsAllProofState', draft,
         'operation?.cancel(); archive?.close(); archive = null; documents = emptyMap()', 'operation?.cancel(); archive = null; documents = emptyMap()'),
        ('draft-late-close', model + 'closingDraftCancelsReadAndClosesCaptureReturnedLate', draft,
         'archive = value else value.close()', 'archive = value else Unit'),
        ('draft-policy', model + 'materialChangesClearProofAndPolicyReselectionClearsTrust', draft,
         'if (kind == PluginArchiveMaterial.POLICY) trustedPolicy = false', 'Unit'),
        ('draft-epoch', model + 'withdrawnAndReconfirmedTrustRejectsLatePreview', draft,
         'epoch = Any(); receipt = null; issue = null', 'receipt = null; issue = null'),
        ('draft-proof', model + 'materialChangesClearProofAndPolicyReselectionClearsTrust', draft,
         'epoch = Any(); receipt = null; issue = null', 'epoch = Any(); issue = null'),
        ('draft-picker', model + 'wrongCancelledAndClosedPickerResultsCannotPopulateAnotherDraft', draft,
         'if (closed || pending !== request) return', 'if (closed) return'),
        ('draft-cancel', model + 'closingDraftCancelsReadAndClosesCaptureReturnedLate', draft,
         'operation?.cancel(); archive?.close(); archive = null; documents = emptyMap()', 'archive?.close(); archive = null; documents = emptyMap()'),
        ('draft-error', model + 'failedProviderHasVisibleFixedErrorAndNoOverlappingRequest', draft,
         'else -> "无法完成签名 ZIP 预检，请重新选择。"', 'else -> failure.message ?: "error"'),
        ('declaration-digest', decl + 'rawManifestMustMatchExactCoreDigest', declarations,
         'hash(bytes) != metadata.manifestDigest', 'false'),
        ('declaration-programs', decl + 'allPermissionsProgramsAndExecutorDeclarationsRemainComplete', declarations,
         'if (names == null) emptyList() else (0 until names.length()).map { names.opt(it) as? String ?: throw Rejected() }', 'emptyList()'),
        ('declaration-purpose', decl + 'allPermissionsProgramsAndExecutorDeclarationsRemainComplete', declarations,
         'text(item, "purpose")', '""'),
        ('declaration-executor', decl + 'allPermissionsProgramsAndExecutorDeclarationsRemainComplete', declarations,
         'val executor = root.opt("executor")?.let { (it as? JSONObject ?: throw Rejected()).toString(2) }', 'val executor: String? = null'),
        ('declaration-protocol', decl + 'allPermissionsProgramsAndExecutorDeclarationsRemainComplete', declarations,
         'metadata.sdkVersion, declared, metadata.protocols.toList()', 'metadata.sdkVersion, metadata.protocols.toList(), metadata.protocols.toList()'),
        ('declaration-strict', decl + 'duplicateAndMalformedJsonCannotOverrideDisplayedDeclarations', signature,
         'parse(bytes).obj()', 'Unit'),
        ('independent-worker', 'com.hanxi.ghosterm.PluginArchivePreviewTest#independentInspectorBindsFullDeclarationsAndKeepsParentUsable', controller,
         'return ExtensionController(releaseProfile, appContext, versionDirectory)', 'return this'),
        ('page-trust', page, view, 'onValueChange = model::trustPolicy', 'onValueChange = {}'),
        ('page-permissions', page, view, 'result.declarations.permissions.forEach', 'result.declarations.permissions.take(0).forEach'),
        ('settings-entry', page, settings, 'PluginArchiveReviewContent(extensions.controller)', 'Unit'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-archive-ui-mutations' / str(time.time_ns())
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
        expected = {'page-trust-target': 'Failed to assert the following: (is enabled)',
            'page-permissions-target': 'permission declaration missing from verified ZIP',
            'settings-entry-target': 'ZIP review entry missing from Settings'}.get(label)
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
    print('ANDROID ARCHIVE UI MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
