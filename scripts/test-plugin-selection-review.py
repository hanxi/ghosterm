#!/usr/bin/env python3
"""macOS reviewed selection snapshots, declaration binding and version references.

Owned signed declarations with injected metadata; actual Core/native UI/live SSH
are verified separately by test-plugin-native-signature.py --selection-review.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('removal_crash', ROOT / 'scripts/test-plugin-removal-crash.py')
base = importlib.util.module_from_spec(spec); spec.loader.exec_module(base)
MODULE = 'apple/GhostermApp/Sources/GhostermAppModel/PluginSelectionReview.swift'
DECLARATIONS = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseDeclarations.swift'
CONTROLLER = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'
SUITE = 'GhostermAppTests.PluginSelectionReviewTests/'
RECIPES = [
    ('candidate-binding', MODULE, 'let proof = try authenticated.bind(.init(id: review.id, version: review.version, publisher: review.publisher, packageSHA256: review.digest))', 'let proof = authenticated.proof', 'testCandidateMetadataCannotBorrowSignedVersion'),
    ('root-binding', MODULE, 'guard rootIdentity.sameObject(try Self.rootStamp(owner)) else', 'guard true else', 'testRootReplacementCannotReuseIdenticalSelectionBytes'),
    ('candidate-object', MODULE, 'guard try Self.stamp(named.fd).sameObject(Self.stamp(candidate.fd)) else', 'guard named.fd >= 0 else', 'testReplacedCandidateCannotReuseIdenticalSelectionAndDescriptor'),
    ('candidate-reference', MODULE, 'let opened = try owner.openVersion(releaseID); try opened.pin(); candidate = opened', 'let opened = try owner.openVersion(releaseID); candidate = opened', 'testCandidateReferenceBlocksRemovalUntilReviewIsReleased'),
    ('unknown-previous', MODULE, 'previous = nil; state = .unknown', 'previous = nil; state = .none', 'testMissingPreviousIsUnknownRatherThanAnEmptyDifference'),
    ('manifest-binding', DECLARATIONS, 'guard PluginReleaseCatalog.sha(bytes) == manifestDigest else', 'guard PluginReleaseCatalog.digest(manifestDigest) else', 'GhostermAppTests.PluginReleaseDeclarationsTests/testChangedManifestCannotBorrowAnOldCoreDigest'),
    ('declaration-programs', DECLARATIONS, 'let programs = try object["programs"].map { try $0.array().map { try $0.string() } } ?? []', 'let programs: [String] = []', 'GhostermAppTests.PluginReleaseDeclarationsTests/testDisplayFieldsUseExactlyTheManifestCoreAttested'),
    ('declaration-diff', CONTROLLER, 'return candidate.permissions.filter { !(previous?.permissions ?? []).contains($0) }', 'return candidate.permissions.filter { new in !(previous?.permissions ?? []).contains { $0.side == new.side && $0.capability == new.capability } }', 'testPermissionPurposeProgramAndCapabilityChangesRemainVisible'),
]


def compile_native(package, work, label, baseline=False):
    selected = 'PluginSelectionReviewTests|PluginReleaseDeclarationsTests|HostListTests.testReadingDoesNotMutateConfig' if baseline else 'HostListTests.testReadingDoesNotMutateConfig'
    child = subprocess.Popen(['swift', 'test', '--package-path', str(package), '--filter', selected], cwd=ROOT,
        env=base.environment(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
    try: output, _ = child.communicate(timeout=300)
    finally:
        if child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            try: child.communicate(timeout=5)
            except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate(timeout=5)
    (work / (label + '.log')).write_bytes(output)
    if child.returncode != 0:
        diagnostics = [line for line in output.decode(errors='replace').splitlines() if re.search(r':(?:[0-9]+:[0-9]+:)? error:', line)]
        print('\n'.join(diagnostics[:20]), flush=True)
    count = 19 if baseline else 1
    assert child.returncode == 0 and f'Executed {count} test' + ('s' if count > 1 else '') + ', with 0 failures' in output.decode(), output.decode()[-9000:]
    assert 'skipped' not in output.decode().lower()
    return hashlib.sha256(output).hexdigest()




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
    with base.private_work('selection-') as work:
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
            print('PASS ' + label + ' mutation / control / restore', flush=True)
        receipt = dict(schemaVersion=1, scope='macOS reviewed selection / declaration digest and diff / actual fd reference and identity; owned signed declarations and injected metadata, no Core-content or UI claim from this suite',
            baselineTests=19, mutations=groups, commands=driver.commands, driverInterruptions=interruptions,
            inputs=inputs, binaries=binaries, baselineLogSHA256=baseline, xctestSHA256=base.sha(bundle / 'Contents/MacOS/GhostermAppTests'))
    assert all(base.sha(ROOT / path) == digest for path, digest in inputs.items())
    assert all(base.sha(Path(path)) == digest for path, digest in binaries.items())
    out.parent.mkdir(parents=True, exist_ok=True); out.write_text(json.dumps(receipt, indent=2) + '\n')
    print(f'SELECTION REVIEW PASS: 19 tests, {len(groups)} groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame): raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]: signal.signal(number, interrupted)
    main()
