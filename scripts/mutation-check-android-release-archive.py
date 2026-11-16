#!/usr/bin/env python3
"""Actual Android ZIP and signed Core preview mutations on an explicitly selected device.

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
MARK = 'GT_ANDROID_RELEASE_ARCHIVE_MUTATION'
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
    archive = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseArchive.kt'
    controller = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt'
    signature = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/PluginReleaseSignature.kt'
    originals = {p: p.read_bytes() for p in [archive, controller, signature]}
    modes = {p: p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in content for content in originals.values()): raise RuntimeError('Residual mutation')
    tests = 'com.hanxi.ghosterm.PluginReleaseArchiveTest#'
    core = 'com.hanxi.ghosterm.PluginReleaseArchiveControllerTest#'
    cases = [
        ('archive-limit', tests + 'archiveLimitRejectsSparseOversizeBeforeReadingIt', archive,
         'length in 22..LIMIT', 'length >= 22'),
        ('member-count', tests + 'memberCountSingleAndTotalBudgetsPrecedeInflateAllocation', archive,
         'count in 1..1024', 'count >= 1'),
        ('artifact-digest', tests + 'artifactBytesAndDigestMustMatchBeforeAnyMember', archive,
         'hex(hash.digest()) == proof.artifactSHA256', 'true'),
        ('artifact-length', tests + 'artifactBytesAndDigestMustMatchBeforeAnyMember', archive,
         'length == proof.artifactBytes', 'true'),
        ('readonly', tests + 'onlyReadOnlyRegularDescriptorsAreAccepted', archive,
         'FileDescriptorFlags.status(fd) and OsConstants.O_ACCMODE == OsConstants.O_RDONLY', 'true'),
        ('canonical-path', tests + 'canonicalPathsRejectTraversalControlsAndNormalizationAliases', archive,
         'validPath(name)', 'true'),
        ('duplicate', tests + 'duplicateCaseFoldAndFileDirectoryConflictsAreRejected', archive,
         'names.add(name.lowercase(Locale.ROOT))', 'true'),
        ('parent-conflict', tests + 'duplicateCaseFoldAndFileDirectoryConflictsAreRejected', archive,
         'parts.take(depth).joinToString("/") !in names', 'true'),
        ('ordinary-mode', tests + 'symlinksDevicesDirectoriesAndUnknownFlagsAreRejected', archive,
         '((external ushr 16).toInt() and OsConstants.S_IFMT) in setOf(0, OsConstants.S_IFREG)', 'true'),
        ('flags', tests + 'symlinksDevicesDirectoriesAndUnknownFlagsAreRejected', archive,
         'flags and 0x800.inv() == 0', 'true'),
        ('central-extra', tests + 'centralDirectoryRequiresExactSingleDiskAndNoExtrasOrComment', archive,
         'u16(at + 30) == 0', 'true'),
        ('local-header', tests + 'localAndCentralHeadersMustAgreeExactly', archive,
         'u32(offset + 14) == entry.crc', 'true'),
        ('contiguous', tests + 'payloadRangesHaveNoPrefixGapOverlapOrTrailingBytes', archive,
         'offset == cursor', 'true'),
        ('total-budget', tests + 'memberCountSingleAndTotalBudgetsPrecedeInflateAllocation', archive,
         'total <= 67108864', 'true'),
        ('member-budget', tests + 'memberCountSingleAndTotalBudgetsPrecedeInflateAllocation', archive,
         'size <= 16777216', 'true'),
        ('crc', tests + 'storedLengthAndCrcAreVerifiedBeforeDeliveringMember', archive,
         'crc == entry.crc', 'true'),
        ('inflate-input', tests + 'deflateRequiresExactInputOutputEndAndNoDictionary', archive,
         'inflater.bytesRead == entry.compressed.toLong()', 'true'),
        ('inflate-output', tests + 'deflateRequiresExactInputOutputEndAndNoDictionary', archive,
         'inflater.bytesWritten == entry.member.bytes.toLong()', 'true'),
        ('descriptor-close', tests + 'duplicatedDescriptorsCloseWithoutConsumingBorrowedOffset', archive,
         'closed = true; owner.close()', 'closed = true; Unit'),
        ('checkpoint', tests + 'checkpointsCancelAndReleaseEvenFailedOpens', archive,
         'checkpoint()', 'Unit'),
        ('changed', tests + 'modifiedArchiveIsRejectedBeforeCallbackAndBetweenMembers', archive,
         'stamp(before) == stamp(Os.fstat(fd))', 'true'),
        ('closed', tests + 'closedArchiveCannotReadOrPublishMembers', archive,
         'ensure(!closed, Failure.CLOSED)', 'ensure(true, Failure.CLOSED)'),
        ('core-identity', core + 'signedIdentityMismatchIsRejectedAfterRealCoreCapture', controller,
         'PluginReleaseSignature.Identity(review.id, review.version, review.publisher, review.digest) != authenticated.identity', 'false'),
        ('stage-cleanup', core + 'signedStoredAndDeflatedArchivesBindRealCoreWithoutSourceOrSession', controller,
         'check(stage.deleteRecursively() || !stage.exists())', 'check(true)'),
        ('directory-identity', 'com.hanxi.ghosterm.PluginReleaseSignatureTest#signatureBindsEveryReviewedPackageIdentityField', signature,
         'authenticated.identity == identity', 'true'),
        ('descriptor-authentication', 'com.hanxi.ghosterm.PluginReleaseSignatureTest#signatureAndRawBytesAreNotCanonicalized', signature,
         'when (ReleaseSignature.verify(key.bytes, signed, payload))', 'when (0)'),
    ]
    selected = args.selected or [case[0] for case in cases]
    if len(selected) != len(set(selected)) or set(selected) - {c[0] for c in cases}: raise RuntimeError('Unknown or duplicate case')
    cases = [c for c in cases if c[0] in selected]
    for name, _, path, before, _ in cases:
        if originals[path].decode().count(before) != 1: raise RuntimeError('Missing/ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-release-archive-mutations' / str(time.time_ns())
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
    print('ANDROID RELEASE ARCHIVE MUTATIONS PASS: ' + str(len(results)) + ' groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
