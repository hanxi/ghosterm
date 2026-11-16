#!/usr/bin/env python3
"""Owned macOS installation reservations, crash observations and explicit cleanup.

Requires default Core / GhosttyKit. Uses copied native sources and unsigned
declaration fixtures; no package import, execution or publisher authentication.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('removal_crash', ROOT / 'scripts/test-plugin-removal-crash.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
JOURNAL = 'apple/GhostermApp/Sources/GhostermAppModel/PluginInstallJournal.swift'
UI = 'apple/GhostermApp/Sources/GhostermApp/PluginInstalledVersionsView.swift'
SUITE = 'GhostermAppTests.PluginInstallJournalTests/'
POINTS = ['after-reservation', 'after-directory-creation', 'after-directory-registration', 'after-file-create',
          'after-file-write', 'before-publication-rename', 'after-publication-rename', 'after-publication-sync', 'during-staging-removal']
RECIPES = [
    ('directory-identity', JOURNAL, 'guard actual == expected else', 'guard actual.owner == expected.owner else', 'testReplacedDirectoryIsPreserved'),
    ('unconfirmed', JOURNAL, 'guard let expected = record.directory else { return Entry(reference: reference, state: .unconfirmed, target: record.target) }',
     'let expected = record.directory ?? actual', 'testUnconfirmedSameNameDirectoryIsNotAdopted'),
    ('published', JOURNAL, '(try? identity(target)) == expected {', '(try? identity(target)) == expected && false {', 'testAlreadyPublishedRecordNeverDeletesCanonicalVersion'),
    ('record-cas', JOURNAL, 'guard try read(reference.id).1 == reference else', 'guard reference.name.hasPrefix(".prepare-") else', 'testRecordCASRejectsChangedPreview'),
    ('name-binding', JOURNAL, 'record.name == ".prepare-" + id.lowercased(), record.parent == parentIdentity,',
     'record.name.hasPrefix(".prepare-"), record.parent == parentIdentity,', 'testRecordCannotRedirectToAnotherDirectoryName'),
    ('root-binding', JOURNAL, 'record.parent == parentIdentity,\n              record.target == nil',
     'record.parent.owner == parentIdentity.owner,\n              record.target == nil', 'testLedgerDoesNotMoveAuthorityToReplacedRoot'),
    ('record-limit', JOURNAL, 'existing.entries.count + existing.issues.count < 64', 'existing.entries.count + existing.issues.count < 65', 'testReservationLimitCountsMetadataWithoutDirectories'),
    ('page-preview', UI, 'await draft.previewResidue(entry, controller: controller)', '_ = entry',
     'GhostermAppTests.PluginInstallResidueUITests/testExplicitPreviewCancelAndCleanupButtons'),
]


def compile_native(package, work, label, baseline=False):
    selected = 'PluginInstallJournalTests|PluginInstallResidueUITests|PluginReleaseRemovalTests|HostListTests.testReadingDoesNotMutateConfig' if baseline else 'HostListTests.testReadingDoesNotMutateConfig'
    child = subprocess.Popen(['swift', 'test', '--package-path', str(package), '--filter', selected], cwd=ROOT,
        env=base.environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try: output, _ = child.communicate(timeout=300)
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try: child.communicate(timeout=5)
            except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)
    (work / (label + '.log')).write_bytes(output)
    count = 41 if baseline else 1
    assert child.returncode == 0 and f'Executed {count} test' + ('s' if count > 1 else '') + ', with 0 failures' in output.decode(), output.decode()[-9000:]
    assert 'skipped' not in output.decode().lower()
    return hashlib.sha256(output).hexdigest()


def crash_case(driver, work, point):
    fixture = base.Fixture(work, point)
    base.thaw(fixture.root / fixture.release); shutil.rmtree(fixture.root / fixture.release)
    action = 'stage-gc-begin' if point == 'during-staging-removal' else 'stage-begin'
    boundary = 'after-unlink' if point == 'during-staging-removal' else point
    with driver.child(base.PROBE, fixture, action=action, point=boundary, descriptor=fixture.descriptor.decode()) as child:
        prefix, proof = driver.ready(child, fixture, boundary, action)
        child.send_signal(signal.SIGKILL); suffix, _ = child.communicate(timeout=10)
        assert child.returncode == -signal.SIGKILL
        output = prefix + suffix
        (work / (point + '-death.log')).write_bytes(output)
        driver.commands.append(dict(label=point, kind='injected-process-death', status=child.returncode,
            checkpoint=proof, logSHA256=hashlib.sha256(output).hexdigest()))
    records = list((fixture.root / 'install-journal-v1').glob('*.json')); assert len(records) == 1
    record = json.loads(records[0].read_bytes())
    if point == 'during-staging-removal':
        before = base.snapshot(fixture.root)
        driver.run(point + '-inspect', fixture, action='inspect', pending=True, versions=0,
            journalSHA=base.sha(fixture.root / base.JOURNAL), survivors=fixture.members(), removalID=base.sha(records[0]))
        assert base.snapshot(fixture.root) == before
        driver.run(point + '-resume', fixture, action='resume', removed=2, absent=1)
        assert not records[0].exists() and not (fixture.root / base.JOURNAL).exists()
        print('PASS actual staging death ' + point, flush=True); return
    state = ('absent' if point == 'after-reservation' else 'unconfirmed' if point == 'after-directory-creation' else
             'published' if point in ['after-publication-rename', 'after-publication-sync'] else 'abandoned')
    before = base.snapshot(fixture.root)
    driver.run(point + '-inspect', fixture, action='stage-inspect', residueState=state, survivors=fixture.members())
    assert base.snapshot(fixture.root) == before, 'residue observation must not resume installation or delete data'
    if state == 'unconfirmed':
        driver.run(point + '-denied', fixture, action='stage-preview-denied', expectedError='stage.changed', survivors=fixture.members())
        assert base.snapshot(fixture.root) == before
    elif state in ['absent', 'published']:
        survivors = {path: digest for path, digest in fixture.members().items() if path != str(records[0].relative_to(fixture.root))}
        driver.run(point + '-retire', fixture, action='stage-retire', survivors=survivors)
        assert not records[0].exists()
        for path, digest in survivors.items(): assert base.sha(fixture.root / path) == digest
        if state == 'published': assert (fixture.root / fixture.release).is_dir()
    else:
        directory = fixture.root / record['name']
        count = len(base.snapshot(directory))
        driver.run(point + '-clean', fixture, action='stage-clean', removed=count, absent=0)
        assert not directory.exists() and not records[0].exists()
        assert not (fixture.root / fixture.release).exists(), 'staging cleanup must not publish a version'
    print('PASS actual staging death ' + point, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true'); parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    base.TMP.mkdir(exist_ok=True); out = args.out.resolve(); assert base.TMP in out.parents
    assert 'GHOSTERM_WITH_PLUGIN_JS:BOOL=OFF' in (ROOT / 'core/build/CMakeCache.txt').read_text()
    inputs = {str(p.relative_to(ROOT)): base.sha(p) for directory in ['apple', 'core/include', 'core/src', 'core/tests', 'packages', 'scripts']
        for p in (ROOT / directory).rglob('*') if p.is_file() and not any(part in p.parts for part in ['.build', '.swiftpm', '__pycache__'])}
    tools = [Path(subprocess.check_output(['/usr/bin/xcrun', '--find', 'xctest'], env=base.environment(), text=True).strip()),
             ROOT / 'core/build/libghosterm_core.a', ROOT / 'build/host-native/lib/libssh2.a', ROOT / 'build/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a']
    binaries = {str(path): base.sha(path) for path in tools}
    for label, filename, old, _, _ in RECIPES: assert (ROOT / filename).read_text().count(old) == 1, label
    with base.private_work('ij-') as work:
        source = work / 'source'
        for name in ['GhostermApp', 'GhostermCore', 'GhostermTerminal']:
            shutil.copytree(ROOT / 'apple' / name, source / 'apple' / name, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir()
        for name in ['include', 'build']: (source / 'core' / name).symlink_to(ROOT / 'core' / name, target_is_directory=True)
        for name in ['build', 'patches']: (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir(); package = source / 'apple/GhostermApp'
        baseline = compile_native(package, work, 'baseline', True)
        bundle = next((package / '.build').rglob('GhostermAppTests.xctest'))
        driver = base.Driver(bundle, tools[0], work)
        for point in POINTS: crash_case(driver, work, point)
        interruptions = base.check_interruptions(bundle, work)
        groups = []
        for label, filename, old, new, selected in RECIPES if args.mutate else []:
            original = (ROOT / filename).read_bytes(); copied = source / filename; stem = Path(filename).stem
            objects = {path: base.sha(path) for path in (package / '.build').rglob('*') if path.name in [stem + '.swift.o', stem + '.o']}; assert objects
            copied.write_bytes(original.replace(old.encode(), new.encode()))
            selected = selected if '/' in selected else SUITE + selected
            try:
                compile_native(package, work, label + '-compile')
                assert any(base.sha(path) != digest for path, digest in objects.items())
                driver.run(label + '-red', selected=selected, red=True)
                driver.run(label + '-control', selected=base.CONTROL)
            finally: copied.write_bytes(original)
            compile_native(package, work, label + '-restore-compile')
            driver.run(label + '-restored', selected=selected)
            assert base.sha(copied) == inputs[filename]; groups.append(label)
        receipt = dict(schemaVersion=1, scope='macOS durable installation reservations / identity and explicit staging maintenance; declaration-only owned SIGKILL and native UI, no runtime or publisher authentication',
            baselineTests=41, checkpoints=POINTS, mutations=groups, commands=driver.commands, driverInterruptions=interruptions,
            inputs=inputs, binaries=binaries, baselineLogSHA256=baseline, xctestSHA256=base.sha(bundle / 'Contents/MacOS/GhostermAppTests'))
    assert all(base.sha(ROOT / path) == digest for path, digest in inputs.items())
    assert all(base.sha(Path(path)) == digest for path, digest in binaries.items())
    out.parent.mkdir(parents=True, exist_ok=True); out.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'INSTALL JOURNAL PASS: 41 tests, {len(POINTS)} actual death points, {len(groups)} groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame): raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]: signal.signal(number, interrupted)
    main()
