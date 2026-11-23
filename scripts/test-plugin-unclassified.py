#!/usr/bin/env python3
"""macOS unknown-directory review, explicit manual deletion and recovery.

Owned private fixtures only. Unknown origin stays unknown, including after a
fresh manual deletion decision; existing ownership records are never adopted.
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
MODULE = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseRemoval.swift'
UNKNOWN = 'apple/GhostermApp/Sources/GhostermAppModel/PluginUnclassifiedDirectories.swift'
UI = 'apple/GhostermApp/Sources/GhostermApp/PluginInstalledVersionsView.swift'
SUITE = 'GhostermAppTests.PluginUnclassifiedDirectoryTests/'
POINTS = ['before-intent', 'after-intent', 'after-detach', 'after-unlink', 'before-clear']
RECIPES = [
    ('native-acknowledgement', MODULE, 'guard header.unknownName == nil || acknowledgedUnknown else', 'guard true else', 'testNativeRemovalAlsoRequiresSeparateAcknowledgement'),
    ('listing-identity', UNKNOWN, 'try token(parent: root, name: name, value: value) == expected else', 'PluginReleaseCatalog.digest(expected) else', 'testReplacedListingCannotAuthorizeAnotherObject'),
    ('enabled-preview', MODULE, 'guard !selection.choices.values.contains(where: { $0.releaseID == unknownName && $0.enabled }) else', 'guard true else', 'testEnabledUnreadableVersionCannotBypassDisableViaUnknownEntry'),
    ('enabled-recovery', MODULE, 'guard !choice.choices.values.contains(where: { $0.releaseID == name && $0.enabled }) else', 'guard true else', 'testManualRecoveryRetainsDirectoryIfSelectionWasEnabled'),
    ('journal-name-binding', MODULE, 'header.releaseID == PluginReleaseCatalog.sha(Data(name.utf8)) else', 'PluginReleaseCatalog.digest(header.releaseID) else', 'testManualJournalCannotRedirectOriginalName'),
    ('reserved-names', UNKNOWN, '![".", "..", PluginReleaseCatalog.selectionName, PluginReleaseCatalog.removalName, PluginInstallJournal.directoryName].contains(name)', '![".", ".."].contains(name)', 'testReservedNamesAndPathTraversalCannotBeReviewed'),
    ('page-preview', UI, 'await draft.previewUnknown(entry, controller: controller)', '_ = entry', 'GhostermAppTests.PluginUnclassifiedDirectoryUITests/testManualAcknowledgementResetsOnCancelAndControlsDeletion'),
    ('page-acknowledgement', UI, '(!confirmed && preview.unknownOrigin && !draft.acknowledgedUnknown)', '(false)', 'GhostermAppTests.PluginUnclassifiedDirectoryUITests/testManualAcknowledgementResetsOnCancelAndControlsDeletion'),
]


def compile_native(package, work, label, baseline=False):
    selected = 'PluginUnclassifiedDirectoryTests|PluginUnclassifiedDirectoryUITests|PluginInstallJournalTests|PluginInstallResidueUITests|PluginReleaseRemovalTests|HostListTests.testReadingDoesNotMutateConfig' if baseline else 'HostListTests.testReadingDoesNotMutateConfig'
    child = subprocess.Popen(['swift', 'test', '--package-path', str(package), '--filter', selected], cwd=ROOT,
        env=base.environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try: output, _ = child.communicate(timeout=300)
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try: child.communicate(timeout=5)
            except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)
    (work / (label + '.log')).write_bytes(output)
    count = 60 if baseline else 1
    assert child.returncode == 0 and f'Executed {count} test' + ('s' if count > 1 else '') + ', with 0 failures' in output.decode(), output.decode()[-9000:]
    assert 'skipped' not in output.decode().lower()
    return hashlib.sha256(output).hexdigest()


def crash_case(driver, work, point):
    fixture = base.Fixture(work, 'unknown-' + point)
    base.thaw(fixture.root / fixture.release); shutil.rmtree(fixture.root / fixture.release)
    name = 'ordinary unknown folder'
    directory = fixture.root / name; (directory / 'nested').mkdir(parents=True, mode=0o700)
    (directory / 'nested/file.txt').write_bytes(b'retained')
    with driver.child(base.PROBE, fixture, action='unknown-begin', point=point, unknownName=name) as child:
        prefix, proof = driver.ready(child, fixture, point, 'unknown-begin')
        child.send_signal(signal.SIGKILL); suffix, _ = child.communicate(timeout=10)
        assert child.returncode == -signal.SIGKILL
        output = prefix + suffix
        (work / (point + '-death.log')).write_bytes(output)
        driver.commands.append(dict(label=point, kind='injected-process-death', status=child.returncode,
            checkpoint=proof, logSHA256=hashlib.sha256(output).hexdigest()))
    before = base.snapshot(fixture.root)
    pending = point != 'before-intent'
    driver.run(point + '-inspect', fixture, action='inspect', pending=pending, versions=0,
        journalSHA=base.sha(fixture.root / base.JOURNAL) if pending else None, survivors=fixture.members(),
        removalID=hashlib.sha256(name.encode()).hexdigest(), unknownName=name)
    assert base.snapshot(fixture.root) == before, 'fresh Controller observation/close must not replay the manual decision'
    assert not (fixture.root / 'install-journal-v1').exists(), 'manual decision cannot invent installation provenance'
    if pending:
        header = json.loads((fixture.root / base.JOURNAL).read_bytes().splitlines()[0])
        assert header['schema'] == 3 and header['unknownName'] == name and 'stage' not in header
        driver.run(point + '-resume', fixture, action='resume', unknownName=name,
            removed=0 if point == 'before-clear' else 2 if point == 'after-unlink' else 3,
            absent=3 if point == 'before-clear' else 1 if point == 'after-unlink' else 0)
        assert not (fixture.root / header['tombstone']).exists()
    else:
        driver.run(point + '-clean', fixture, action='unknown-clean', unknownName=name, removed=3, absent=0)
    assert not directory.exists() and not (fixture.root / base.JOURNAL).exists()
    assert not (fixture.root / 'install-journal-v1').exists()
    print('PASS actual unknown-directory death ' + point, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true'); parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    base.TMP.mkdir(exist_ok=True); out = args.out.resolve(); assert base.TMP in out.parents
    assert 'GHOSTERM_WITH_PLUGIN_LUA:BOOL=OFF' in (ROOT / 'core/build/CMakeCache.txt').read_text()
    inputs = {str(p.relative_to(ROOT)): base.sha(p) for directory in ['apple', 'core/include', 'core/src', 'core/tests', 'packages', 'scripts']
        for p in (ROOT / directory).rglob('*') if p.is_file() and not any(part in p.parts for part in ['.build', '.swiftpm', '__pycache__'])}
    tools = [Path(subprocess.check_output(['/usr/bin/xcrun', '--find', 'xctest'], env=base.environment(), text=True).strip()),
             ROOT / 'core/build/libghosterm_core.a', ROOT / 'build/host-native/lib/libssh2.a', ROOT / 'build/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a']
    binaries = {str(path): base.sha(path) for path in tools}
    for label, filename, old, _, _ in RECIPES: assert (ROOT / filename).read_text().count(old) == 1, label
    with base.private_work('unknown-') as work:
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
        receipt = dict(schemaVersion=1, scope='macOS unknown-directory observation / fresh explicit manual deletion / schema 3 recovery; no origin adoption, runtime or publisher authentication',
            baselineTests=60, checkpoints=POINTS, mutations=groups, commands=driver.commands, driverInterruptions=interruptions,
            inputs=inputs, binaries=binaries, baselineLogSHA256=baseline, xctestSHA256=base.sha(bundle / 'Contents/MacOS/GhostermAppTests'))
    assert all(base.sha(ROOT / path) == digest for path, digest in inputs.items())
    assert all(base.sha(Path(path)) == digest for path, digest in binaries.items())
    out.parent.mkdir(parents=True, exist_ok=True); out.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'UNCLASSIFIED DIRECTORIES PASS: 60 tests, {len(POINTS)} actual death points, {len(groups)} groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame): raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]: signal.signal(number, interrupted)
    main()
