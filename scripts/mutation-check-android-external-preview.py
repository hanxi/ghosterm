#!/usr/bin/env python3
"""A4 external preview mutations: compiled target assertion red, same-APK Core control, restoration and SHA256."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_EXTERNAL_PREVIEW_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the preview test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    cache = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExternalPreviewCache.kt'
    provider = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExternalPreviewProvider.kt'
    manifest = ROOT / 'android/app/src/main/AndroidManifest.xml'
    vm = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/FilesViewModel.kt'
    screen = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/FilesScreen.kt'
    repository = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/FileBrowserRepository.kt'
    originals = {path:path.read_bytes() for path in (cache,provider,manifest,vm,screen,repository)}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual external preview mutation')
    cases = [
        ('known-size-admission', 'ExternalPreviewCacheTest#unknownAndKnownOversizeCannotBypassStreamingLimit', ((cache, 'check(expected==null || expected<=fileLimit.toULong())', 'check(true)', 1),)),
        ('capacity', 'ExternalPreviewCacheTest#wholeCacheCapacityIncludesUnknownSizeReservationsAndReleasedFailures', ((cache, 'used+writing.size*fileLimit<=totalLimit-fileLimit', 'used+writing.size*fileLimit<=Long.MAX_VALUE-fileLimit', 1),)),
        ('expiry-cleanup', 'ExternalPreviewCacheTest#expiredCopiesWaitForConsumerCloseBeforeReclamation', ((cache, 'time-file.lastModified()>=retentionMillis', 'time-file.lastModified()>=Long.MAX_VALUE', 1),)),
        ('reader-pin', 'ExternalPreviewCacheTest#expiredCopiesWaitForConsumerCloseBeforeReclamation', ((cache, 'readers[token]==null &&', 'true &&', 1),)),
        ('write-mode', 'ExternalPreviewCacheTest#writesTraversalAndExpiredAddressesAreRejected', ((cache, 'if(mode!="r")', 'if(false)', 1),)),
        ('uri-authority', 'ExternalPreviewCacheTest#writesTraversalAndExpiredAddressesAreRejected', ((cache, 'uri.authority!=authority', 'false', 1),)),
        ('raw-byte-integrity', 'ExternalPreviewCacheTest#rawBytesAndDisplayNameArePreservedWithoutExposingTheSource', ((cache, 'output.write(buffer,0,n)', 'output.write(ByteArray(n))', 1),)),
        ('immutable-published-copy', 'ExternalPreviewCacheTest#rawBytesAndDisplayNameArePreservedWithoutExposingTheSource', ((cache, 'Os.chmod(part.path,256)', 'Os.chmod(part.path,384)', 1),)),
        ('cleanup-ownership', 'ExternalPreviewCacheTest#invalidRootsAndCleanupFailuresAreVisibleInsteadOfAdmitted', ((cache, 'TOKEN.matches(token) && OsConstants.S_ISREG(Os.lstat(it.path).st_mode)', 'TOKEN.matches(token)', 1),)),
        ('intent-read-grant', 'ExternalPreviewCacheTest#actionViewContainsOnlyTemporaryReadGrantAndClipData', ((screen, 'addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION)', 'addFlags(android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION or android.content.Intent.FLAG_GRANT_WRITE_URI_PERMISSION)', 1),)),
        ('provider-not-exported', 'ExternalPreviewCacheTest#actualOtherUidNeedsTheIntentGrantAndCannotWrite', ((manifest, 'android:exported="false" android:grantUriPermissions="true"', 'android:exported="true" android:grantUriPermissions="true"', 1),)),
        ('provider-mode-routing', 'ExternalPreviewCacheTest#realProviderUsesTheSameReadOnlyCacheAndOpenableMetadata', ((provider, 'cache.open(uri,mode)', 'cache.open(uri,"r")', 1),)),
        ('launch-consumed-once', 'ExternalPreviewProductTest#explicitExternalRequestIsConsumedExactlyOnce', ((vm, 'externalReady=null\n        try { launch(ready) }', 'Unit\n        try { launch(ready) }', 1),)),
        ('stale-ready-launch', 'ExternalPreviewProductTest#readyCopyCannotLaunchAfterPreviewCloses', ((vm, 'if(!visible || externalReady!==ready || preview==null) return', 'if(false) return', 1),)),
        ('button-wiring', 'ExternalPreviewProductTest#binaryPreviewOffersExplicitExternalOpenAndNeverStartsOnTap', ((screen, 'onClick=model::openExternal', 'onClick={}', 1),)),
        ('remote-parent-membership', 'ExternalPreviewCacheTest#repositoryRejectsRemoteEntryOutsideDirectoryWithoutConnection', ((repository, 'check(entry.id.startsWith(prefix) && entry.id.removePrefix(prefix).let {\n', 'check(true || entry.id.startsWith(prefix) && entry.id.removePrefix(prefix).let {\n', 1),)),
        ('actual-worker-cancellation', 'ExternalPreviewCacheTest#cancellationSignalsTheActualReaderBeforePageCapacityReturns', ((cache, 'waiting.invokeOnCancellation { worker.cancel() }', 'waiting.invokeOnCancellation { Unit }', 1),)),
        ('unlinked-reader-capacity', 'ExternalPreviewCacheTest#unlinkedButOpenCopiesRemainInTheCapacityBudget', ((cache, 'readerBytes.filterKeys { it !in named }', 'emptyMap<String,Long>()', 1),)),
        ('proxy-seek-offset', 'ExternalPreviewCacheTest#consumerCanSeekWithoutReceivingTheBackingCacheDescriptor', ((cache, 'needed-count,offset+count)', 'needed-count,count.toLong())', 1),)),
        ('background-presenter-cancellation', 'ExternalPreviewProductTest#actualBackgroundStopCancelsPreparationAndRequiresAnotherExplicitOpen', ((screen, 'if(event==Lifecycle.Event.ON_STOP) model.externalPresenterStopped()', 'if(false) model.externalPresenterStopped()', 1),)),
        ('unlink-failure', 'ExternalPreviewCacheTest#cleanupUnlinkFailurePreservesBytesAndBlocksNewAdmission', ((cache, 'check(deleteCopy(file))', 'check(true.also { deleteCopy(file) })', 1),)),
        ('default-file-limit', 'ExternalPreviewCacheTest#defaultFileLimitRejectsSeventeenMiBBeforeOpeningTheSource', ((cache, 'const val FILE_LIMIT = 16L * 1024 * 1024', 'const val FILE_LIMIT = 32L * 1024 * 1024', 1),)),
        ('default-retention', 'ExternalPreviewCacheTest#defaultRetentionKeepsTwentyThreeHoursButExpiresAfterTwentyFive', ((cache, 'const val RETENTION_MILLIS = 24L * 60 * 60 * 1000', 'const val RETENTION_MILLIS = 1L * 60 * 60 * 1000', 1),)),
        ('empty-copy-slots', 'ExternalPreviewCacheTest#emptyFilesCannotBypassTheSixteenCopySlotLimit', ((cache, '+missingPins.size<16 &&', '+missingPins.size<17 &&', 1),)),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-external-preview-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('EXTERNAL PREVIEW RECEIPTS', work, flush=True)

    def write_source(path, data):
        temporary = work / ('source-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(data)
        temporary.chmod(modes[path])
        os.replace(temporary, path)

    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                write_source(path, data)
            if path.read_bytes() != data:
                raise RuntimeError('External preview source restore SHA256 mismatch: ' + path.name)

    def run_logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        run_logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            run_logged(label + '-install-' + Path(name).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name])

    def test(label, target, green):
        output = run_logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + target, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        if green:
            good = 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            good = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                kind in output for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
            good = good and 'Fixture:' not in output
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(cases[number - 1][1] for number in selected)):
            test('baseline-' + str(index), target, True)
        for number in selected:
            name, target, edits = cases[number - 1]
            variants = {}
            for path, old, new, count in edits:
                variants[path] = variants.get(path, originals[path].decode()).replace(old, new)
            for path, source in variants.items():
                write_source(path, (source + ('\n<!-- ' + MARK + ' -->\n' if path.suffix == '.xml' else '\n// ' + MARK + '\n')).encode())
            label = f'C{number:02d}-{name}'
            build(label)
            test(label + '-target', target, False)
            test(label + '-control', 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            restore()
            build(label + '-restored')
            test(label + '-restored', target, True)
            results.append(dict(case=number, name=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(label + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
        helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print(f'ANDROID EXTERNAL PREVIEW MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
