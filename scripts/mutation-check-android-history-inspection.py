#!/usr/bin/env python3
"""History inspection device mutations on a real device, with independent Core controls."""
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
MARK = 'GT_ANDROID_HISTORY_INSPECTION_MUTATION'
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
    manifest = ROOT / 'android/data/src/main/java/com/hanxi/ghosterm/data/FileHistoryInspector.kt'
    originals = {manifest: manifest.read_bytes()}
    modes = {path: path.stat().st_mode & 0o777 for path in originals}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual history inspection mutation')
    cases = [
        ('read-error', 'FileHistoryInspectionTest#unreadableDirectoryRemainsUnknownInsteadOfAbsent', ((manifest, 'catch(error: Exception) { unknown(error.message ?: "无法读取当前位置") }', 'catch(error: Exception) { FileHistoryObservation(item.source.relativePath,candidate,FileHistoryPresence.ABSENT,location) }', 1),)),
        ('saf-id', 'FileHistoryInspectionTest#safWithoutReturnedIdNeverGuessesByDisplayName', ((manifest, 'parent.endpoint.treeUri!=null && id==null', 'false', 1),)),
        ('exact-id', 'FileHistoryInspectionTest#recordedIdDoesNotFallBackToSameNameReplacement', ((manifest, 'if(id!=null) it.id==id else it.name==name', 'if(id!=null) it.id==id || it.name==item.targetName else it.name==name', 1),)),
        ('candidate-parent', 'FileHistoryInspectionTest#candidateCannotEscapeRecordedParent', ((manifest, 'require(name.isNotEmpty() && name!="." && name!=".." && \'/\' !in name && \'\\u0000\' !in name)', 'require(true)', 1),)),
        ('directory-id', 'FileHistoryInspectionTest#changedDirectoryIdentityIsUnknown', ((manifest, 'require(directory.location==parent)', 'require(true)', 1),)),
        ('endpoint-id', 'FileHistoryInspectionTest#endpointMismatchIsRejectedBeforeReading', ((manifest, 'require(parent.endpoint==record.intent.destination.endpoint)', 'require(true)', 1),)),
        ('cancel', 'FileHistoryInspectionTest#cancellationPropagatesWithoutPublishingPartialResult', ((manifest, 'catch(cancel: CancellationException) { throw cancel }', 'catch(cancel: CancellationException) { return unknown("cancelled") }', 2),)),
        ('directory-budget', 'FileHistoryInspectionTest#directoryBudgetStopsFurtherReads', ((manifest, 'directories.size<DIRECTORY_LIMIT', 'directories.size<=DIRECTORY_LIMIT', 1),)),
    ]
    cases += [
        ('entry-budget', 'FileHistoryInspectionTest#perDirectoryEntryBudgetRejectsIncompleteView', ((manifest, 'require(directory.entries.size<=ENTRY_LIMIT)', 'require(true)', 1),)),
        ('total-budget', 'FileHistoryInspectionTest#aggregateEntryBudgetStopsAdditionalReads', ((manifest, 'check(entriesRead<16384)', 'check(true)', 1),)),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        for path, old, new, count in cases[number - 1][2]:
            if originals[path].decode().count(old) != count:
                raise RuntimeError(f'C{number}: source anchor missing or ambiguous')
    work = ROOT / 'tmp/android-history-inspection-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('HISTORY INSPECTION RECEIPTS', work, flush=True)

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
                raise RuntimeError('History inspection source restore SHA256 mismatch: ' + path.name)

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
    print(f'ANDROID HISTORY INSPECTION MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
