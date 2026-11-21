#!/usr/bin/env python3
"""Owned Native journal crashes and copied Swift normal-assertion mutations.

No user App/support directory, process discovery, credentials or foreground UI.
Only direct subprocesses are signalled, before wait/reap releases their PID.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
JOURNAL = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionLocalJournal.swift'
TEMPORARY = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionLocalTemporaryDirectory.swift'
CONTROL = 'ExtensionControllerTests.testClientHostProfileOnlyAdvertisesIntegratedCapabilities'


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--core-build', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    core = args.core_build.resolve(strict=True)
    cache = core / 'CMakeCache.txt'
    if not cache.is_file(): cache = core.parent / 'CMakeCache.txt'
    if not core.is_relative_to(ROOT / 'tmp') or not cache.is_file() or 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON' not in cache.read_text():
        parser.error('explicit JS-enabled copied Core build under project tmp required')
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): parser.error('receipt must be in project tmp')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('apple/GhostermApp/Sources', 'apple/GhostermApp/Tests',
        'apple/GhostermCore/Sources', 'apple/GhostermTerminal/Sources', 'core/src', 'core/include')
        for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('scripts/test-plugin-local-journal.py', 'scripts/fixtures/plugin-local-journal.swift',
                 'scripts/fixtures/plugin-local-idle.c', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_LOCAL_JOURNAL_MUTATION' in (ROOT / name).read_bytes() for name in inputs if name.startswith('apple/')):
        raise RuntimeError('Refuse leftover Native journal mutation marker')
    with tempfile.TemporaryDirectory(prefix='lj-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; logs = work / 'logs'; logs.mkdir()
        for folder in ('apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal'):
            shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include')
        (source / 'core/build').symlink_to(core, target_is_directory=True)
        for folder in ('build', 'patches'): (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp'), 'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer',
            'GHOSTERM_AUTOMATION_OFFSCREEN': '1', 'GHOSTERM_CORE_TEST_PLUGIN_JS': '1',
            'GHOSTERM_PROCESS_CHANNEL_CHILD': str(ROOT / 'core/build/test_process_channel_child'),
            'GHOSTERM_LOCAL_IDLE_CHILD': str(work / 'idle')}
        commands = []

        def run(label, command, red=False, reject=False, seconds=300):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require local-journal; shift; exec "$gt_timeout_bin" "$@"', 'local-journal', ROOT, seconds, *command]
            p = subprocess.Popen(list(map(str, bounded)), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, start_new_session=True)
            try: out, err = p.communicate(timeout=seconds)
            except BaseException:
                if p.poll() is None:
                    try: os.killpg(p.pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    if p.poll() is None:
                        try: os.killpg(p.pid, signal.SIGKILL)
                        except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            text = out + err; (logs / (label + '.log')).write_text(text)
            commands.append(dict(label=label, returncode=p.returncode))
            if red:
                assertions = re.findall(r'^.*: error: .* : XCTAssertTrue failed[^\n]*', text, re.M)
                if p.returncode != 1 or len(assertions) != 1 or 'Executed 1 test, with 1 failure (0 unexpected)' not in text or any(t in text for t in ('unexpected signal', 'AddressSanitizer', 'runtime error:', 'fatalError')):
                    raise RuntimeError(label + ': expected one compiled normal assertion red\n' + text)
            elif reject:
                if p.returncode != 2 or not err.startswith('REJECTED:'): raise RuntimeError(label + ': expected explicit rejection\n' + text)
            elif p.returncode:
                raise RuntimeError(label + '\n' + text)
            return text

        run('compile-probe', ['xcrun', '--sdk', 'macosx', 'swiftc', '-swift-version', '5', '-parse-as-library',
            ROOT / JOURNAL, ROOT / TEMPORARY, ROOT / 'scripts/fixtures/plugin-local-journal.swift', '-o', work / 'probe'])
        run('compile-idle', ['xcrun', '--sdk', 'macosx', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', ROOT / 'scripts/fixtures/plugin-local-idle.c', '-o', work / 'idle'])
        crashes = []
        for mode in ('intent', 'namespace'):
            for number in (signal.SIGTERM, signal.SIGKILL):
                fixture = work / (mode + '-' + str(number)); fixture.mkdir()
                base = fixture / 'base'; base.mkdir(mode=0o700); ledger = fixture / 'ledger'; ready = fixture / 'ready'
                p = subprocess.Popen([str(work / 'probe'), mode, str(ledger), str(base), str(ready)], env=env,
                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
                try:
                    end = time.monotonic() + 2
                    while not ready.exists() and time.monotonic() < end and p.poll() is None: time.sleep(.01)
                    if not ready.exists(): raise RuntimeError('Native owner setup did not reach its recorded phase')
                    before = json.loads(ready.read_text()); assert len(before) == 1 and before[0]['held']
                    os.kill(p.pid, number); p.communicate(timeout=2)
                    if p.returncode != -number: raise RuntimeError('Owner did not die by the requested signal')
                    current = json.loads(run(mode + '-' + str(number) + '-reopen', [work / 'probe', 'inspect', ledger]))
                    expected = 'missing' if mode == 'intent' else 'matching'
                    if len(current) != 1 or current[0]['held'] or current[0]['location'] != expected or current[0]['reservedBytes'] != 268435456 or any(current[0][key] != before[0][key] for key in ('id', 'snapshot', 'directory', 'reservedBytes')):
                        raise RuntimeError('New Native process lost or adopted a reservation')
                    entry = current[0]
                    run(mode + '-' + str(number) + '-no-consent', [work / 'probe', 'release', ledger, entry['id'], entry['snapshot'], 'no'], reject=True)
                    if mode == 'namespace':
                        run(mode + '-' + str(number) + '-present', [work / 'probe', 'release', ledger, entry['id'], entry['snapshot'], 'yes'], reject=True)
                        directory = Path(entry['directory']); keep = directory / 'keep'
                        if keep.read_bytes() != b'OWNED': raise RuntimeError('Inspection modified the owned fixture')
                        # Only the harness removes its own positively identified fixture.
                        keep.unlink(); directory.rmdir()
                    run(mode + '-' + str(number) + '-release', [work / 'probe', 'release', ledger, entry['id'], entry['snapshot'], 'yes'])
                    if json.loads(run(mode + '-' + str(number) + '-empty', [work / 'probe', 'inspect', ledger])) != []:
                        raise RuntimeError('Explicit absent-directory release did not retire the reservation')
                    crashes.append(dict(phase=mode, signal=int(number), newProcessInspection=expected, retainedUntilExplicitRelease=True))
                finally:
                    if p.poll() is None: p.terminate(); p.communicate(timeout=10)
        print('PASS four actual Native owner deaths / fresh-process inspection / explicit missing-only release', flush=True)

        package = source / 'apple/GhostermApp'
        selection = 'LocalJournalTests|LocalTemporaryDirectoryTests|LocalExtensionTests|ExecutorExecutionTests'
        def test(label, selected=selection, count=43, red=False):
            text = run(label, ['swift', 'test', '--package-path', package, '--filter', selected], red=red)
            if not red and (f'Executed {count} test' not in text or 'with 0 failures' not in text or 'skipped' in text):
                raise RuntimeError(label + ': exact discovery/no-skip mismatch\n' + text)
        test('baseline')
        cases = [
            ('quota', JOURNAL, 'UInt64(current.count) < Self.budget / Self.reservation', 'true', 'testBudgetSurvivesOwnerDropAndRejectsBeforeMkdir'),
            ('missing', JOURNAL, 'location(record) == .missing', 'true', 'testRecordedNamespaceIsOnlyInspectedAfterOwnerDrop'),
            ('confirmation', JOURNAL, 'guard confirmed, let', 'guard true, let', 'testUnboundReservationPersistsAndNeedsExplicitConfirmation'),
            ('snapshot', JOURNAL, 'current == snapshot', 'true', 'testChangedSnapshotRequiresAnotherDecision'),
            ('parent', JOURNAL, 'Identity(value) == record.parent', 'true', 'testChangedBaseCannotReleaseAnApparentlyMissingDirectory'),
            ('lease', JOURNAL, 'guard flock(file.fd, LOCK_EX | LOCK_NB) == 0 else', 'guard true else', 'testHeldLeaseCannotBeReleasedThroughAnotherManager'),
            ('incomplete', JOURNAL, 'name == "format" || name == "lock"', 'name == "format" || name == "lock" || name == "pending"', 'testUnknownAndIncompleteMetadataBlockAdmission'),
            ('retirement', TEMPORARY, 'try reservation?.finish(); reservation = nil', 'reservation = nil', 'testSuccessfulCleanupRetiresItsReservation'),
            ('record-content', JOURNAL, 'digest(file.data) == lease.snapshot', 'true', 'testInPlaceRecordChangeRetainsOwnerUntilRestored'),
            ('admission', TEMPORARY, 'reservation = try journal.reserve(parent: parent, base: base, id: id)', 'reservation = nil', 'testControllerCannotStartWhenRecoveredBudgetIsFull'),
            ('native-confirmation', 'apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift', 'releaseMissingLocalReservation(entry, confirmed: confirmed)', 'releaseMissingLocalReservation(entry, confirmed: true)', 'testAppModelReopensAndRequiresExplicitRelease'),
            ('native-snapshot', 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift', 'snapshot: entry.snapshot, confirmed: confirmed', 'snapshot: (try self.journal().inspect().first?.snapshot) ?? entry.snapshot, confirmed: confirmed', 'testAppModelStaleDecisionDoesNotReleaseChangedMetadata'),
        ]
        groups = []
        for label, filename, old, new, target in cases if args.mutate else []:
            path = source / filename; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError('Ambiguous mutation anchor: ' + label)
            objects = package / '.build'
            before = {str(p): sha(p) for p in objects.rglob(path.stem + '.o')}
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_LOCAL_JOURNAL_MUTATION\n')
                test(label + '-red', 'LocalJournalTests.' + target, 1, True)
                if not any(sha(p) != before.get(str(p)) for p in objects.rglob(path.stem + '.o')): raise RuntimeError('Mutant object did not change')
                test(label + '-control', CONTROL, 1)
            finally: path.write_bytes(original)
            test(label + '-restore', 'LocalJournalTests.' + target, 1)
            if sha(path) != hashlib.sha256(original).hexdigest(): raise RuntimeError('Source did not restore')
            groups.append(label); print('PASS ' + label + ': compiled single assertion red / same library config green / restored / SHA256', flush=True)
        test('final')
        if any(sha(ROOT / name) != digest for name, digest in inputs.items()): raise RuntimeError('Root inputs changed')
        binaries = {str(core / name): sha(core / name) for name in ('libghosterm_core.a', 'libghosterm_quickjs.a')}
        binaries.update({p.name: sha(p) for p in (work / 'probe', work / 'idle')})
        receipt = dict(accepted=True, inputs=inputs, commands=commands, crashes=crashes, cases=42, mutations=groups,
            binaries=binaries, scope='macOS arm64 trusted Native journal reservation / read-only inspection; no resumed process, old-directory deletion, OS quota, Android or release')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print('LOCAL JOURNAL PASS: four deaths / 42 Native tests / ' + str(len(groups)) + ' mutations', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
