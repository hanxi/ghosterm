#!/usr/bin/env python3
"""CLI Ed25519 release -> Core snapshot -> native macOS controls.

Requires an explicitly built JS-enabled core/build and locked developer tools.
Only copied Swift sources are mutated; owned windows remain offscreen.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VERIFIER = 'apple/GhostermApp/Sources/GhostermAppModel/PluginDirectorySignature.swift'
VIEW = 'apple/GhostermApp/Sources/GhostermApp/PluginSignatureReviewView.swift'
CONTROLLER = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'
TEST = 'PluginDirectorySignatureTests.'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'
sys.dont_write_bytecode = True


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    out, tools = args.out.resolve(), args.tools.resolve(strict=True)
    assert ROOT / 'tmp' in out.parents
    spec = importlib.util.spec_from_file_location('release_fixture', ROOT / 'scripts/test-plugin-release.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    node = Path(shutil.which('node')).resolve(strict=True)
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ['apple', 'core/include', 'core/src', 'core/tests', 'packages', 'plugins/official/preview', 'scripts']
              for p in (ROOT / directory).rglob('*') if p.is_file() and not any(x in p.parts for x in ['.build', '.swiftpm', '__pycache__'])}
    binaries = {str(p): sha(p) for p in tools.rglob('*') if p.is_file()}
    for p in [node, ROOT / 'core/build/libghosterm_core.a', ROOT / 'core/build/libghosterm_quickjs.a', ROOT / 'build/host-native/lib/libssh2.a']:
        binaries[str(p)] = sha(p)
    recipes = [
        ('signature', VERIFIER, 'guard publicKey.isValidSignature(signatureBytes, for: message)',
         'guard publicKey.rawRepresentation.count == 32', TEST + 'testRealSignatureBindsRawDescriptorAndDomain'),
        ('revoked', VERIFIER, 'guard !revokedIDs.contains(keyID)', 'guard keyID.count == 64', TEST + 'testPolicyRevocationAndScope'),
        ('channel', VERIFIER, 'guard requestedChannel == channel', 'guard !requestedChannel.isEmpty && !channel.isEmpty', TEST + 'testPolicyRevocationAndScope'),
        ('identity', VERIFIER, 'package == identity.packageSHA256 else', 'package.count == identity.packageSHA256.count else', TEST + 'testIdentityCannotBeReplacedByCaller'),
        ('checked-selection', CONTROLLER, 'guard let record = self.packages[self.key(packageID, digest)] else',
         'guard let record = self.packages.values.first(where: { $0.review.id == packageID }) else', TEST + 'testCLISignatureAgainstActualCoreSnapshotWithoutApproval'),
        ('number-precision', VERIFIER, 'var value = raw.lowercased()',
         'if let rounded = Double(raw), rounded >= 0, rounded <= Double(maximum), rounded.rounded(.towardZero) == rounded { return UInt64(rounded) }; var value = raw.lowercased()', TEST + 'testExactIntegerLexemesAndBooleanRejection'),
        ('duplicate', VERIFIER, 'object[key] == nil, ![', 'true, ![', TEST + 'testDuplicateEscapedAndReservedKeys'),
        ('file-link', VERIFIER, 'O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC', 'O_RDONLY | O_NONBLOCK | O_CLOEXEC', TEST + 'testSelectedPublicFilesRejectLinksDirectoriesFIFOsAndLimit'),
        ('late-result', VIEW, 'guard request == token else', 'guard token != UUID() else', TEST + 'testPageDraftInvalidatesProofAndLateResults'),
        ('page-action', VIEW, 'action: verify)', 'action: {})', 'PluginSignatureUITests.testExplicitPolicyAndSignatureThroughActualPackageControls'),
    ]
    originals = {name: (ROOT / name).read_bytes() for name in [VERIFIER, VIEW, CONTROLLER]}
    for label, filename, old, _, _ in recipes:
        assert originals[filename].count(old.encode()) == 1, label
    commands, groups = [], []
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1', GHOSTERM_CORE_TEST_PLUGIN_JS='1')
    with tempfile.TemporaryDirectory(prefix='ns-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        fixture = work / 'fixture'; fixture.mkdir()
        with helper.agent(node, work) as (public, endpoint):
            assert helper.exercise(ROOT, fixture, tools, node, public, endpoint, selected='success') == ['success']
        source = work / 'source'
        for directory in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal']:
            shutil.copytree(ROOT / directory, source / directory, symlinks=True,
                            ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir()
        for name in ['include', 'build']:
            (source / 'core' / name).symlink_to(ROOT / 'core' / name, target_is_directory=True)
        for name in ['build', 'patches']:
            (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir()
        env['GHOSTERM_TEST_SIGNED_RELEASE'] = str(fixture)
        package = source / 'apple/GhostermApp'

        def run(label, selected, count=1, red=False):
            command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require native-signature; shift; exec "$gt_timeout_bin" --kill-after=8 350 "$@"',
                       'native-signature', str(ROOT), 'swift', 'test', '--package-path', str(package), '--filter', selected]
            child = subprocess.Popen(command, cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                output, _ = child.communicate(timeout=360)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try: child.communicate(timeout=8)
                    except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate()
            log = output.decode(errors='replace')
            (work / (label + '.log')).write_bytes(output)
            if red:
                assert child.returncode == 1 and re.search(r'Executed 1 test, with [1-9][0-9]* failures? \(0 unexpected\)', log), (label, log[-9000:])
                assert re.search(r': error: .* : (?:XCTAssert\w+ failed|failed -)', log), (label, log[-9000:])
                assert not any(s in log for s in ['unexpected signal', 'AddressSanitizer', 'fatalError', 'Build failed']), label
            else:
                assert child.returncode == 0 and f'Executed {count} test' + ('s' if count != 1 else '') + ', with 0 failures' in log, (label, log[-9000:])
                assert 'skipped' not in log.lower(), label
            commands.append(dict(label=label, selected=selected, status=child.returncode, logSHA256=hashlib.sha256(output).hexdigest()))
            print('PASS ' + label, flush=True)

        run('baseline', '(PluginDirectorySignatureTests|PluginSignatureUITests)', count=11)
        for label, filename, old, new, selected in recipes if args.mutate else []:
            path = source / filename
            stem = Path(filename).stem
            objects = {p: sha(p) for p in (package / '.build').rglob('*') if p.name in [stem + '.swift.o', stem + '.o']}
            assert objects, 'compiled production object missing'
            path.write_bytes(originals[filename].replace(old.encode(), new.encode()))
            try:
                run(label + '-assertion', selected, red=True)
                assert any(sha(p) != digest for p, digest in objects.items()), 'mutation must actually compile'
                run(label + '-control', CONTROL)
            finally:
                path.write_bytes(originals[filename])
            run(label + '-restored', selected)
            assert sha(path) == inputs[filename]
            groups.append(label)
        binary = next((package / '.build').rglob('GhostermAppTests.xctest/Contents/MacOS/GhostermAppTests'))
        binary_sha = sha(binary)
    assert all(sha(ROOT / name) == digest for name, digest in inputs.items()), 'source changed'
    assert all(sha(Path(name)) == digest for name, digest in binaries.items()), 'tool/archive changed'
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(dict(schemaVersion=1, scope='CLI public release to immutable Core directory attestation and native controls; no ZIP import/install',
        baselineTests=11, inputs=inputs, binaries=binaries, xctestSHA256=binary_sha, commands=commands, mutations=groups), indent=2) + '\n')
    print(f'NATIVE SIGNATURE PASS: 11 tests, {len(groups)} mutation/control/restore groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]:
        signal.signal(number, interrupted)
    main()
