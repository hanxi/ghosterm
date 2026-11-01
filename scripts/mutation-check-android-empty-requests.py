#!/usr/bin/env python3
"""Empty-request maintenance device mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_EMPTY_REQUESTS_MUTATION'
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
    native=ROOT/'android/bridge/src/main/cpp/publication_jni.c';executor=data/'FileCopyExecutor.kt'
    originals={p:p.read_bytes() for p in (service,budget,native,screen,model,executor)}
    modes={p:p.stat().st_mode & 0o777 for p in originals}
    if any(MARK.encode() in body for body in originals.values()): raise RuntimeError('Residual retained file mutation')
    cases = [
        ('empty-list', 'LocalEmptyRequestsTest#confirmedBatchRemovesOnlyEmptyRequestsAndNeverFiles', ((service, 'if(empty)emptyRecords+=', 'if(false)emptyRecords+=', 1),)),
        ('nonempty-list', 'LocalEmptyRequestsTest#confirmedBatchRemovesOnlyEmptyRequestsAndNeverFiles', ((service, 'if(empty)emptyRecords+=', 'if(true)emptyRecords+=', 1),)),
        ('directory-identity', 'LocalEmptyRequestsTest#replacedOwnerIsNotAcceptedByOldReview', ((service, 'check(stamp(owner)==selected.stamp)', 'check(true)', 1),(service, 'check(stamp(child)==expected)', 'check(true)', 1),)),
        ('fresh-root', 'LocalEmptyRequestsTest#replacedRootCannotRedirectBatch', ((service, 'LocalPublicationFile.directory(root,"").use { freshRoot ->\n                    check(directory(freshRoot)==directory(current.root))', 'ParcelFileDescriptor.dup(current.root.fileDescriptor).use { freshRoot ->\n                    check(directory(freshRoot)==directory(current.root))', 1),)),
        ('missing-result', 'LocalEmptyRequestsTest#missingRequestIsNotCountedAsRemovedByThisBatch', ((service, 'if(named==null) { missing++;continue }', 'if(named==null) { removed++;continue }', 1),)),
        ('review-owner', 'LocalEmptyRequestsTest#refreshedAndForeignReviewsCannotAuthorizeCleanup', ((service, 'preparedEmpty?.takeIf { it.review===review }', 'preparedEmpty', 1),)),
        ('cancel-release', 'LocalEmptyRequestsTest#cancellationPreservesDirectoriesAndReleasesReviewRoot', ((service, 'if(preparedEmpty?.review===review)closeHeld()', 'Unit', 1),)),
        ('source-lease', 'LocalEmptyRequestsTest#sourceTaskLeaseBlocksMaintenanceBeforeTargetReservation', ((budget, 'check(tasks.isEmpty())', 'check(true)', 1),)),
        ('executor-register', 'LocalEmptyRequestsTest#actualExecutorRegistersBeforeSourceWorkAndReleasesAfterSuccessOrFailure', ((executor, 'maintenanceLease=LocalPublicationBudget.SHARED.beginTask()', 'Unit', 1),)),
        ('executor-release', 'LocalEmptyRequestsTest#actualExecutorRegistersBeforeSourceWorkAndReleasesAfterSuccessOrFailure', ((executor, 'maintenanceLease?.close()', 'Unit', 1),)),
        ('scan-budget', 'LocalEmptyRequestsTest#partialScanCanMaintainOnlyItsVerifiedEmptyRequests', ((service, 'check(++tasks<=taskLimit)', 'check(true)', 1),)),
        ('native-rmdir', 'LocalEmptyRequestsTest#nativeRmdirRemovesOnlyPinnedEmptyDirectoryAndKeepsBorrowedFds', ((native, 'else if(unlinkat(parent,value,AT_REMOVEDIR)<0)error=errno;', 'else if(unlinkat(parent,value,0)<0)error=errno;', 1),)),
        ('native-identity', 'LocalEmptyRequestsTest#nativeRmdirRejectsNonemptyFilesLinksMismatchAndTraversal', ((native, 'else if(!S_ISDIR(selected.st_mode) || !S_ISDIR(named.st_mode) ||\n            selected.st_dev!=named.st_dev || selected.st_ino!=named.st_ino)', 'else if(0)', 1),)),
        ('borrowed-parent', 'LocalEmptyRequestsTest#nativeRmdirRemovesOnlyPinnedEmptyDirectoryAndKeepsBorrowedFds', ((native, 'else if(unlinkat(parent,value,AT_REMOVEDIR)<0)error=errno;\n    free(value);return error;', 'else if(unlinkat(parent,value,AT_REMOVEDIR)<0)error=errno;\n    if(!error)close(parent);\n    free(value);return error;', 1),)),
        ('native-action', 'LocalEmptyRequestsTest#nativeRmdirRemovesOnlyPinnedEmptyDirectoryAndKeepsBorrowedFds', ((native, 'else if(unlinkat(parent,value,AT_REMOVEDIR)<0)error=errno;', 'else error=0;', 1),)),
        ('confirm-routing', 'RetainedFilesProductTest#emptyDirectoryCleanupNeedsConfirmationAndPreservesHistoryAndFiles', ((screen, 'onClick=model::clearEmptyRetained', 'onClick=model::dismissRetained', 1),)),
        ('cancel-routing', 'RetainedFilesProductTest#emptyDirectoryCleanupNeedsConfirmationAndPreservesHistoryAndFiles', ((screen, 'dismissButton={ TextButton(onClick=model::dismissRetained,enabled=!model.retainedLoading,modifier=Modifier.testTag("retained-empty-cancel"))', 'dismissButton={ TextButton(onClick=model::clearEmptyRetained,enabled=!model.retainedLoading,modifier=Modifier.testTag("retained-empty-cancel"))', 1),)),
        ('visible-error', 'RetainedFilesProductTest#changedEmptyDirectoryShowsFailureAfterRefreshAndKeepsNewFile', ((model, 'emptyRetainedReview=null;retainedLoading=false;retainedJob=null\n                    loadRetained(clearError=false)', 'emptyRetainedReview=null;retainedLoading=false;retainedJob=null\n                    loadRetained(clearError=true)', 1),)),
        ('stale-list', 'LocalEmptyRequestsTest#refreshedAndForeignReviewsCannotAuthorizeCleanup', ((service, 'records.clear();emptyRecords.clear()', 'records.clear()', 1),)),
        ('preflight-all', 'LocalEmptyRequestsTest#allChildrenAreCheckedBeforeRemovingAnyDirectory', ((service, 'check(stamp(child)==expected)', 'check(true)', 1),(service, 'check(!entries.iterator().hasNext())', 'check(true)', 1),)),
        ('page-release', 'RetainedFilesProductTest#clearingPageModelReleasesEmptyReviewWithoutDeletingDirectories', ((model, 'emptyRetainedReview?.let { if(retainedOwner.isInitialized())work.releaseEmptyRetained(retainedOwner.value,it) }', 'Unit', 1),)),
    ]
    cases += [
        ('handoff', 'LocalEmptyRequestsTest#cancellationAtReturnBoundaryReleasesRootViaHandoff', ((service, ';onPrepared(it)', '', 1),)),
        ('late-release', 'LocalEmptyRequestsTest#lateOldReviewReleaseCannotCloseNewReview', ((service, 'if(preparedEmpty?.review===review)closeHeld()', 'if(preparedEmpty!=null)closeHeld()', 1),)),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-empty-requests-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('EMPTY REQUESTS RECEIPTS', work, flush=True)

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
    print(f'ANDROID EMPTY REQUESTS MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
