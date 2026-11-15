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
import struct
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
VERIFIER = 'apple/GhostermApp/Sources/GhostermAppModel/PluginDirectorySignature.swift'
VIEW = 'apple/GhostermApp/Sources/GhostermApp/PluginSignatureReviewView.swift'
CONTROLLER = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'
TEST = 'PluginDirectorySignatureTests.'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'
sys.dont_write_bytecode = True


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def installation_fixtures(helper, node, fixture, tools, endpoint):
    """Only public materials reach disk; the disposable signer retains its key."""
    policy = (fixture / 'policy.json').read_bytes()
    base = json.loads((fixture / 'r/release.json').read_bytes())
    def envelope(destination, archive, identity=None):
        (destination / 'r').mkdir(parents=True)
        (destination / 'policy.json').write_bytes(policy)
        (destination / 'r/plugin.zip').write_bytes(archive)
        descriptor = dict(base)
        descriptor['artifact'] = dict(format='zip-v1', bytes=len(archive), sha256=hashlib.sha256(archive).hexdigest())
        if identity is not None: descriptor['packageSHA256'] = identity
        raw = json.dumps(descriptor, separators=(',', ':')).encode()
        signed = subprocess.run([node, ROOT / 'scripts/fixtures/plugin-release-agent.mjs', 'client', endpoint],
            input=helper.DOMAIN + raw, capture_output=True, check=True)
        (destination / 'r/release.json').write_bytes(raw)
        (destination / 'r/release.sig').write_bytes(signed.stdout)
    for name in ['v2', 'compressed']:
        target = fixture / name
        shutil.copytree(fixture / 'p', target / 'p')
        (target / 'policy.json').write_bytes(policy)
        if name == 'v2':
            manifest = target / 'p/plugin.json'
            value = json.loads(manifest.read_bytes()); value['pluginVersion'] = '0.2.0'
            manifest.write_text(json.dumps(value))
            integrity = target / 'p/integrity.json'; value = json.loads(integrity.read_bytes())
            for item in value['members']:
                data = (target / 'p' / item['path']).read_bytes()
                item.update(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())
            integrity.write_text(json.dumps(value))
        archive = target / 'input.zip'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_DEFLATED if name == 'compressed' else zipfile.ZIP_STORED) as output:
            for member in sorted((target / 'p').rglob('*')):
                if member.is_file(): output.writestr(member.relative_to(target / 'p').as_posix(), member.read_bytes())
        config = json.loads((fixture / 'signer.json').read_bytes())
        signer = target / 'signer.json'; signer.write_text(json.dumps(config))
        subprocess.run([sys.executable, '-B', ROOT / 'scripts/gterm-plugin', 'release', 'sign', '--artifact', archive,
            '--policy', target / 'policy.json', '--tools', tools, '--key-id', base['keyId'], '--signer-json', signer,
            '--out', target / 'r'], check=True, stdout=subprocess.DEVNULL, env=dict(os.environ, TMPDIR=str(ROOT / 'tmp')))
    members = [(p.relative_to(fixture / 'p').as_posix(), p.read_bytes()) for p in sorted((fixture / 'p').rglob('*')) if p.is_file()]
    def archive(entries, mode=None):
        records, central, offset = [], [], 0
        for index, (name, data) in enumerate(entries):
            raw = name.encode(); crc = zlib.crc32(data); method = 8 if mode == 'compressed-tail' else 0
            if method:
                encoder = zlib.compressobj(wbits=-15); packed = encoder.compress(data) + encoder.flush()
                if index == 0: packed += b'x'
            else: packed = data
            declared = len(data)
            external = 0o100600 << 16
            extra = b''
            if index == 0:
                if mode == 'crc': crc ^= 1
                if mode == 'link': external = 0o120777 << 16
                if mode == 'size': declared = 16777217
                if mode == 'extra': extra = b'\x01\x00\x00\x00'
            local = struct.pack('<4s5H3I2H', b'PK\x03\x04', 20, 0x800, method, 0, 0, crc, len(packed), declared, len(raw), len(extra)) + raw + extra + packed
            records.append(local)
            central.append(struct.pack('<4s6H3I5H2I', b'PK\x01\x02', 0x314, 20, 0x800, method, 0, 0, crc, len(packed), declared,
                len(raw), len(extra), 0, 0, 0, external, offset) + raw + extra)
            offset += len(local)
        catalog = b''.join(central)
        return b''.join(records) + catalog + struct.pack('<4s4H2IH', b'PK\x05\x06', 0, 0, len(entries), len(entries), len(catalog), offset, 0)
    original = (fixture / 'r/plugin.zip').read_bytes()
    for name in ['crc', 'hidden', 'path', 'case', 'file-parent', 'link', 'size', 'extra', 'compressed-tail', 'identity', 'signature', 'bytehash']:
        entries = members
        if name == 'path': entries = [('../escape', b'x')]
        if name == 'case': entries = [('a', b'a'), ('A', b'b')]
        if name == 'file-parent': entries = [('a', b'a'), ('a/b', b'b')]
        data = archive(entries, name)
        if name == 'hidden': data = b'x' + data
        destination = fixture / 'bad' / name
        if name in ['signature', 'bytehash']:
            shutil.copytree(fixture / 'r', destination / 'r')
            (destination / 'policy.json').write_bytes(policy)
            if name == 'signature': (destination / 'r/release.sig').write_bytes(bytes(64))
            else:
                changed = bytearray(original); changed[12] ^= 1
                (destination / 'r/plugin.zip').write_bytes(changed)
        else: envelope(destination, data, '0' * 64 if name == 'identity' else None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--install', action='store_true', help='also exercise signed ZIP installation transactions')
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
        ('identity', VERIFIER, 'identity.packageSHA256 == actual.packageSHA256 else', 'identity.packageSHA256.count == actual.packageSHA256.count else', TEST + 'testIdentityCannotBeReplacedByCaller'),
        ('checked-selection', CONTROLLER, 'guard let record = self.packages[self.key(packageID, digest)] else',
         'guard let record = self.packages.values.first(where: { $0.review.id == packageID }) else', TEST + 'testCLISignatureAgainstActualCoreSnapshotWithoutApproval'),
        ('number-precision', VERIFIER, 'var value = raw.lowercased()',
         'if let rounded = Double(raw), rounded >= 0, rounded <= Double(maximum), rounded.rounded(.towardZero) == rounded { return UInt64(rounded) }; var value = raw.lowercased()', TEST + 'testExactIntegerLexemesAndBooleanRejection'),
        ('duplicate', VERIFIER, 'object[key] == nil, ![', 'true, ![', TEST + 'testDuplicateEscapedAndReservedKeys'),
        ('file-link', VERIFIER, 'O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC', 'O_RDONLY | O_NONBLOCK | O_CLOEXEC', TEST + 'testSelectedPublicFilesRejectLinksDirectoriesFIFOsAndLimit'),
        ('late-result', VIEW, 'guard request == token else', 'guard token != UUID() else', TEST + 'testPageDraftInvalidatesProofAndLateResults'),
        ('page-action', VIEW, 'action: verify)', 'action: {})', 'PluginSignatureUITests.testExplicitPolicyAndSignatureThroughActualPackageControls'),
    ]
    if args.install:
        archive = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseArchive.swift'
        stage = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseStage.swift'
        install = 'apple/GhostermApp/Sources/GhostermApp/PluginReleaseInstallView.swift'
        selected = 'PluginReleaseInstallTests.'
        recipes += [
            ('install-artifact', archive, 'digest == proof.artifactSHA256 else', 'digest.count == proof.artifactSHA256.count else', selected + 'testBadSignedArchivesNeverPublishOrLeaveOwnedStage'),
            ('install-crc', archive, 'guard UInt32(checksum) == member.crc else', 'guard checksum <= 0xffff_ffff else', selected + 'testBadSignedArchivesNeverPublishOrLeaveOwnedStage'),
            ('install-stream', archive, 'stream.total_in == member.compressed,', 'stream.total_in <= member.compressed,', selected + 'testBadSignedArchivesNeverPublishOrLeaveOwnedStage'),
            ('install-freeze', stage, 'fchmod(fd, 0o500)', 'fchmod(fd, 0o700)', selected + 'testInstallPublishesReadOnlyVersionWithoutSourceApproval'),
            ('install-no-overwrite', stage, 'UInt32(RENAME_EXCL)', '0', selected + 'testExistingEmptySlotIsNeverAdopted'),
            ('install-durability', stage, 'let durable = parentSync(parent) == 0', 'let durable = parentSync(parent) != 42', selected + 'testPublishedButUnsyncedParentIsExplicitlyUnknown'),
            ('install-cancel', stage, 'guard unlinkat(parent, name, AT_REMOVEDIR) == 0 else', 'guard name.hasPrefix(".prepare-") else', selected + 'testPreparedReleaseIsPrivateAndCancelCleansIt'),
            ('install-lock', stage, 'guard flock(parent, LOCK_EX | LOCK_NB) == 0 else', 'guard parent >= 0 else', selected + 'testStageLockFreezeAndFailedPublishCleanup'),
            ('install-root', stage, 'parentIdentity.st_mode & 0o077 == 0 else', 'parentIdentity.st_mode & S_IFMT == S_IFDIR else', selected + 'testPrivateRootAndRetainedVersionLimit'),
            ('install-limit', stage, 'guard count < 64 else', 'guard count < 65 else', selected + 'testPrivateRootAndRetainedVersionLimit'),
            ('install-page', install, 'await draft.inspect(using: controller)', '_ = controller', 'PluginReleaseInstallUITests.testInspectCancelAndInstallThroughActualControls'),
            ('install-retry-owner', CONTROLLER, 'try pending.stage.discard()\n                try pending.record.package.close()',
             'try pending.record.package.close()\n                try pending.stage.discard()', selected + 'testFailedDiscardRetainsOwnerForExplicitRetry'),
            ('install-snapshot', stage, 'guard files[path] == FileStamp(value) else', 'guard files[path] != nil else', selected + 'testChangedPreparedFilesMustNotPublish'),
            ('install-member-set', stage, 'guard verifiedFiles == files.count else', 'guard verifiedFiles <= files.count else', selected + 'testAddedOrMissingPreparedMembersMustNotPublish'),
        ]
    originals = {name: (ROOT / name).read_bytes() for name in {recipe[1] for recipe in recipes}}
    for label, filename, old, _, _ in recipes:
        assert originals[filename].count(old.encode()) == 1, label
    commands, groups = [], []
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1', GHOSTERM_CORE_TEST_PLUGIN_JS='1')
    with tempfile.TemporaryDirectory(prefix='ns-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        fixture = work / 'fixture'; fixture.mkdir()
        with helper.agent(node, work) as (public, endpoint):
            assert helper.exercise(ROOT, fixture, tools, node, public, endpoint, selected='success') == ['success']
            if args.install: installation_fixtures(helper, node, fixture, tools, endpoint)
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

        baseline = '(PluginDirectorySignatureTests|PluginSignatureUITests|PluginReleaseInstallTests|PluginReleaseInstallUITests)' if args.install else '(PluginDirectorySignatureTests|PluginSignatureUITests)'
        baseline_count = 28 if args.install else 11
        run('baseline', baseline, count=baseline_count)
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
    scope = ('macOS signed ZIP version publication and native controls; no upgrade manager or runtime grant' if args.install else
             'CLI public release to immutable Core directory attestation and native controls; no ZIP import/install')
    out.write_text(json.dumps(dict(schemaVersion=1, scope=scope,
        baselineTests=baseline_count, installation=args.install, inputs=inputs, binaries=binaries, xctestSHA256=binary_sha, commands=commands, mutations=groups), indent=2) + '\n')
    print(f'NATIVE SIGNATURE PASS: {baseline_count} tests, {len(groups)} mutation/control/restore groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]:
        signal.signal(number, interrupted)
    main()
