#!/usr/bin/env python3
"""A7 file preference consumer mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_FILE_PREFERENCES_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the preference test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    model, runtime, tasks = (base / name for name in ('FilesViewModel.kt', 'FileWorkRuntime.kt', 'FileTasksViewModel.kt'))
    ordering = ROOT / 'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileListingPreferences.kt'
    originals = {path: path.read_bytes() for path in (model, runtime, tasks, ordering)}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual file preference mutation')
    cases = []
    def case(name, test, path, old, new, count=1):
        cases.append((name, 'FilePreferencesTest#'+test, ((path, old, new, count),)))
    order = 'sizeOrderingPreservesUnsignedUnknownAndDirectorySemantics'
    live = 'committedFiltersAndSortingKeepTheCopiedOriginAndAvoidDirectoryReads'
    clear = 'capturedClearPolicyWorksAfterTheBrowserIsDestroyed'
    case('hidden-filter', live, ordering, 'showHidden || !it.name.startsWith(".")', 'true')
    case('unsigned-size', order, ordering, 'second.compareTo(first)', 'second.toLong().compareTo(first.toLong())')
    cases.append(('directory-size-is-unknown', 'FilePreferencesTest#'+order, (
        (ordering, 'a.size.takeIf { a.kind == FileKind.FILE }', 'a.size', 1),
        (ordering, 'b.size.takeIf { b.kind == FileKind.FILE }', 'b.size', 1))))
    case('folders-first', order, ordering, 'if (foldersFirst)', 'if (false)')
    cases.append(('unknown-size-last', 'FilePreferencesTest#'+order, (
        (ordering, 'first == null -> 1', 'first == null -> -1', 1),
        (ordering, 'second == null -> -1', 'second == null -> 1', 1))))
    case('ascending-name-tiebreak', order, ordering, 'a.name.lowercase(Locale.ROOT).compareTo(b.name.lowercase(Locale.ROOT))',
        'b.name.lowercase(Locale.ROOT).compareTo(a.name.lowercase(Locale.ROOT))')
    case('live-sort-consumer', live, model, 'listing = state.committed', 'listing = state.committed.copy(sortBy = FileSort.NAME)')
    case('hidden-selection-pruned', live, model, 'selected = selected.intersect(visibleEntries.map { it.id }.toSet())', 'Unit', 2)
    case('failed-toggle-keeps-committed', 'failedHiddenToggleKeepsTheConfirmedFilterAndExposesTheError', model,
        'if (result is PreferenceUpdate.Failed) preferenceError = result.message',
        'if (result is PreferenceUpdate.Failed) { listing = listing.copy(showHidden = desired); preferenceError = result.message }')
    case('new-request-conflict-default', 'newRequestsCaptureConflictPolicyWithoutChangingQueuedRequests', tasks,
        'FilePasteRequest.capture(snapshot,destination,label,preferences.conflict)', 'FilePasteRequest.capture(snapshot,destination,label)')
    case('capture-clear-policy', clear, tasks, 'preferences.clipboardAfterPaste == ClipboardAfterPaste.CLEAR', 'false')
    case('completion-uses-captured-policy', clear, runtime, 'task.clearClipboardAfterSuccess',
        'preferences.clipboardAfterPaste == com.hanxi.ghosterm.domain.ClipboardAfterPaste.CLEAR')
    case('old-request-cannot-clear-new-source', 'completingAnOldRequestCannotClearANewerClipboard', runtime,
        'clipboard.snapshot?.id == result.clipboardId', 'true')
    case('skipped-result-cannot-clear', 'allSkippedAndCancelledRequestsKeepTheirSourceList', runtime, 'result.mayClearClipboard', 'true')
    case('application-completion-route', clear, tasks, 'task.result?.let { onCompleted(task, it) }', 'Unit')
    case('capture-keep-policy', 'capturedKeepPolicySurvivesLaterPreferenceChanges', tasks,
        'preferences.clipboardAfterPaste == ClipboardAfterPaste.CLEAR', 'true')
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'P{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-file-preferences-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('FILE PREFERENCE RECEIPTS', work, flush=True)

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
                raise RuntimeError('Settings source restore SHA256 mismatch: ' + path.name)

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
            label = f'P{number:02d}-{name}'
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
    print(f'ANDROID FILE PREFERENCES MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
