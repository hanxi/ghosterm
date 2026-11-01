#!/usr/bin/env python3
"""Retained-file maintenance device mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_RETAINED_FILES_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the history test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    data=ROOT/'android/data/src/main/java/com/hanxi/ghosterm/data'
    app=ROOT/'android/app/src/main/java/com/hanxi/ghosterm'
    service=data/'LocalRetainedFiles.kt';budget=data/'LocalPublicationBudget.kt'
    screen=app/'RetainedFilesScreen.kt';model=app/'FileTasksViewModel.kt'
    native=ROOT/'android/bridge/src/main/cpp/publication_jni.c'
    originals={p:p.read_bytes() for p in (service,budget,native,screen,model)}
    modes={p:p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in body for body in originals.values()): raise RuntimeError('Residual retained file mutation')
    cases = [
        ('stale-list', 'LocalRetainedFilesTest#staleEpochAndForeignOwnerAreRejectedBeforeOpening', ((service, 'ref.owner==ownerId && ref.epoch==epoch', 'ref.owner==ownerId', 1),(service, 'epoch++;records.clear()', 'epoch++', 1),)),
        ('changed-clear', 'LocalRetainedFilesTest#modifiedFileCannotUseOldConfirmation', ((service, 'check(fingerprint(named)==selected.file && fingerprint(Os.fstat(current.file.fileDescriptor))==selected.file)', 'check(true)', 1),)),
        ('changed-preview', 'LocalRetainedFilesTest#changedFileBetweenListingAndReviewIsRejected', ((service, 'check(fingerprint(Os.fstat(file.fileDescriptor))==selected.file)', 'check(true)', 2),)),
        ('active-publisher', 'LocalRetainedFilesTest#activeReservationBlocksMaintenanceAndIsNeverDeleted', ((budget, 'check(active.values.all { it.isEmpty() })', 'check(true)', 1),)),
        ('list-budget', 'LocalRetainedFilesTest#boundedListingRetainsVerifiedRowsAndReportsIncomplete', ((service, 'check(++entries<=entryLimit)', 'check(true)', 1),)),
        ('incomplete', 'LocalRetainedFilesTest#boundedListingRetainsVerifiedRowsAndReportsIncomplete', ((service, 'failureReason==null,failureReason', 'true,failureReason', 1),)),
        ('binary-preview', 'LocalRetainedFilesTest#binaryAndTruncatedTextReviewsRemainReadonly', ((service, 'CodingErrorAction.REPORT', 'CodingErrorAction.REPLACE', 2),)),
        ('preview-budget', 'LocalRetainedFilesTest#binaryAndTruncatedTextReviewsRemainReadonly', ((service, 'minOf(selected.item.size,4096)', 'minOf(selected.item.size,8192)', 1),)),
        ('release-fds', 'LocalRetainedFilesTest#dismissReleasesAllDescriptorsAndPreservesContents', ((service, 'for(fd in listOf(file,directory,owner,root))', 'for(fd in emptyList<ParcelFileDescriptor>())', 1),)),
        ('readonly', 'LocalRetainedFilesTest#nativeRetainedReaderIsReadonlyCloexecAndNeverFollowsSymlinks', ((native, 'openat(directory,value,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)', 'openat(directory,value,O_RDWR|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)', 1),)),
        ('nofollow', 'LocalRetainedFilesTest#nativeRetainedReaderIsReadonlyCloexecAndNeverFollowsSymlinks', ((native, 'openat(directory,value,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)', 'openat(directory,value,O_RDONLY|O_NONBLOCK|O_CLOEXEC)', 1),)),
        ('cloexec', 'LocalRetainedFilesTest#nativeRetainedReaderIsReadonlyCloexecAndNeverFollowsSymlinks', ((native, 'openat(directory,value,O_RDONLY|O_NONBLOCK|O_NOFOLLOW|O_CLOEXEC)', 'openat(directory,value,O_RDONLY|O_NONBLOCK|O_NOFOLLOW)', 1),)),
        ('clear-action', 'LocalRetainedFilesTest#freshReviewAndConfirmationClearOnlySelectedFile', ((service, 'val code=LocalPublicationFile.remove(freshDirectory.fd,current.file.fd,selected.candidate)', 'val code=0', 1),(service, 'check(Os.fstat(current.file.fileDescriptor).st_nlink==0L)', 'check(true)', 1),)),
        ('confirm-routing', 'RetainedFilesProductTest#tapPreviewsAndOnlySeparateConfirmationDeletesWithoutRewritingHistory', ((screen, 'onClick=model::clearRetained', 'onClick=model::dismissRetained', 1),)),
        ('keep-routing', 'RetainedFilesProductTest#tapPreviewsAndOnlySeparateConfirmationDeletesWithoutRewritingHistory', ((screen, 'onClick={ confirming=false }', 'onClick=model::clearRetained', 1),)),
        ('visible-failure', 'RetainedFilesProductTest#changedFileIsPreservedAndFailureRemainsVisibleAfterAutomaticRefresh', ((model, 'loadRetained(clearError=false)', 'loadRetained(clearError=true)', 1),)),
        ('empty-label', 'RetainedFilesProductTest#unknownStructureIsVisibleAndNeverReportedAsEmpty', ((screen, 'if(snapshot?.complete==true && snapshot.items.isEmpty())', 'if(snapshot!=null && snapshot.items.isEmpty())', 1),)),
        ('regular-file', 'LocalRetainedFilesTest#nativeRetainedReaderIsReadonlyCloexecAndNeverFollowsSymlinks', ((native, 'else if(fd>=0 && !S_ISREG(st.st_mode))', 'else if(0)', 1),)),
        ('borrowed-parent', 'LocalRetainedFilesTest#nativeRetainedReaderIsReadonlyCloexecAndNeverFollowsSymlinks', ((native, 'if(fd<0)failure(env,error);\n    return fd;\n}\nJNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_openCandidate', 'if(fd<0)failure(env,error);\n    else close(directory);\n    return fd;\n}\nJNIEXPORT jint JNICALL Java_com_hanxi_ghosterm_bridge_LocalPublicationFile_openCandidate', 1),)),
        ('fresh-root', 'LocalRetainedFilesTest#rootReplacementCannotRedirectCleanup', ((service, 'LocalPublicationFile.directory(root,"").use { freshRoot ->', 'ParcelFileDescriptor.dup(current.root.fileDescriptor).use { freshRoot ->', 1),)),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-retained-files-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('RETAINED FILES RECEIPTS', work, flush=True)

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
                raise RuntimeError('Retained file source restore SHA256 mismatch: ' + path.name)

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
                write_source(path, (source + '\n// ' + MARK + '\n').encode())
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
    print(f'ANDROID RETAINED FILES MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
