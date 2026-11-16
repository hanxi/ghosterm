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
    for name in ['v2', 'compressed', 'live']:
        target = fixture / name
        shutil.copytree(ROOT / 'plugins/official/preview' if name == 'live' else fixture / 'p', target / 'p')
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
    parser.add_argument('--case', action='append', help='run only these named mutation groups, retaining the complete selected baseline')
    parser.add_argument('--install', action='store_true', help='also exercise signed ZIP installation transactions')
    parser.add_argument('--versions', action='store_true', help='also exercise persistent version choices and an active SSH Runtime')
    parser.add_argument('--remove', action='store_true', help='also exercise explicit version removal, recovery and an actual managed Runtime lease')
    parser.add_argument('--gterm', type=Path)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    if args.remove: args.versions = True
    if args.versions:
        args.install = True
        if not args.gterm: parser.error('--versions requires --gterm')
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
    if args.gterm:
        args.gterm = args.gterm.resolve(strict=True)
        binaries[str(args.gterm)] = sha(args.gterm)
        inputs['core/CMakeLists.txt'] = sha(ROOT / 'core/CMakeLists.txt')
        inputs.update({str(p.relative_to(ROOT)): sha(p) for p in (ROOT / 'cli/gterm').rglob('*') if p.is_file() and 'build' not in p.parts})
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
            ('install-root', stage, 'same(value, parentIdentity), value.st_mode & 0o077 == 0 else',
             'same(value, parentIdentity) else', 'PluginInstallJournalTests.testRootPermissionsChangedInvalidateLiveStage'),
            ('install-limit', stage, 'guard count < 64 else', 'guard count < 65 else', selected + 'testPrivateRootAndRetainedVersionLimit'),
            ('install-page', install, 'await draft.inspect(using: controller)', '_ = controller', 'PluginReleaseInstallUITests.testInspectCancelAndInstallThroughActualControls'),
            ('install-retry-owner', CONTROLLER, 'try pending.stage.discard()\n                try pending.record.package.close()',
             'try pending.record.package.close()\n                try pending.stage.discard()', selected + 'testFailedDiscardRetainsOwnerForExplicitRetry'),
            ('install-snapshot', stage, 'guard files[path] == FileStamp(value) else', 'guard files[path] != nil else', selected + 'testChangedPreparedFilesMustNotPublish'),
            ('install-member-set', stage, 'guard verifiedFiles == files.count else', 'guard verifiedFiles <= files.count else', selected + 'testAddedOrMissingPreparedMembersMustNotPublish'),
        ]
    if args.remove:
        removal = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseRemoval.swift'
        selected = 'PluginReleaseRemovalTests.'
        recipes += [
            ('removal-disabled', removal, '!selection.choices.values.contains(where: { $0.releaseID == releaseID && $0.enabled })',
             'selection.choices.count <= 64', selected + 'testEnabledChoiceRequiresExplicitDisable'),
            ('removal-lease', removal, 'guard try handle.exclusiveUse() else { throw Failure.occupied }\n        let root = try owner.removalRoot()',
             'guard handle.fd >= 0 else { throw Failure.occupied }\n        let root = try owner.removalRoot()', selected + 'testSharedRunLeaseBlocksRemovalAcrossOwners'),
            ('removal-cas', removal, '!checkRevision || choice.revision == header.revision',
             'choice.choices.count <= 64', selected + 'testChangedChoiceRejectsStaleConfirmation'),
            ('removal-snapshot', removal, 'try Self.scan(version.fd, device: header.root.device) == members',
             'try Self.scan(version.fd, device: header.root.device).count == members.count', selected + 'testChangedFileRejectsConfirmation'),
            ('removal-recovery-content', removal, 'item.directory ? item.stamp.sameObject(old.stamp) : item == old',
             'item.stamp.sameObject(old.stamp)', selected + 'testRecoveryRejectsReplacedRemainingFile'),
            ('removal-record-before-rename', removal, 'try checkpoint("after-intent")',
             'try checkpoint("before-intent")', selected + 'testDurableIntentBeforeRenameCanResumeExplicitly'),
            ('removal-durability', removal, 'guard synchronize(root) == 0 else { return result(0, 0, false, false) }',
             'guard root >= 0 else { return result(0, 0, false, false) }', selected + 'testUnsyncedIntentRetainsVersionAndRecord'),
            ('removal-absence', removal, 'let absent = members.count - current.count', 'let absent = 0', selected + 'testPartialUnlinkRecoverySeparatesObservedAbsence'),
            ('removal-member-limit', removal, 'result.count < 4096', 'result.count < 4097', selected + 'testMemberAndFileByteLimitsRejectBeforeConfirmation'),
            ('removal-ui', 'apple/GhostermApp/Sources/GhostermApp/PluginInstalledVersionsView.swift', 'await draft.remove(controller, resume: confirmed)',
             'Optional<ExtensionController.RemovalResult>.none', 'PluginInstalledVersionsUITests.testCurrentPolicySelectDisableAndRollbackControls'),
            ('removal-live-reference', CONTROLLER, 'try handle.pin()\n            return handle',
             '// removed shared version pin\n            return handle', 'PluginPaneFlowTests.testManagedPreviewRetainsVersionUntilStopped'),
            ('removal-runtime-transfer', CONTROLLER, 'self.runtimeVersionLease = self.sessionVersionLease; self.sessionVersionLease = nil',
             'self.runtimeVersionLease = nil; self.sessionVersionLease = nil', 'PluginPaneFlowTests.testManagedPreviewRetainsVersionUntilStopped'),
            ('removal-cancel-pending-ui', 'apple/GhostermApp/Sources/GhostermApp/PluginInstalledVersionsView.swift', 'await draft.cancelPending(controller)',
             '_ = controller', 'PluginInstalledVersionsUITests.testCurrentPolicySelectDisableAndRollbackControls'),
            ('removal-pending-status', CONTROLLER, 'catalog.removalPending = pendingRemoval',
             'catalog.removalPending = false', 'PluginReleaseCatalogTests.testPendingRemovalCanBeReviewedAndCancelledWithoutReplay'),
            ('removal-foreign-marker', removal, 'if let detached = try namedStamp(root, header.tombstone) {\n            guard detached.sameObject(header.version) else { throw Failure.changed }',
             'if let detached = try namedStamp(root, header.tombstone) {\n            guard !header.tombstone.isEmpty else { throw Failure.changed }', selected + 'testUnrelatedTombstoneDoesNotProveDeletionStarted'),
        ]
    if args.versions:
        catalog = 'apple/GhostermApp/Sources/GhostermAppModel/PluginReleaseCatalog.swift'
        view = 'apple/GhostermApp/Sources/GhostermApp/PluginInstalledVersionsView.swift'
        selected = 'PluginReleaseCatalogTests.'
        live = 'PluginPaneFlowTests.testRunningPreviewSurvivesVersionSelectionAndDisable'
        recipes += [
            ('catalog-enabled', CONTROLLER, 'let choice = access.choices[record.review.id], choice.enabled,',
             'let choice = access.choices[record.review.id],', selected + 'testUpgradeRollbackAndDisableNeedFreshSourceApproval'),
            ('catalog-generation', CONTROLLER, 'return !requireApproval || record.approvalGeneration == choice.generation',
             'return !requireApproval || choice.generation.count > 0', selected + 'testOtherControllerDisableAndStaleRevisionCannotBeBypassed'),
            ('catalog-cas', catalog, 'try beforeRename?()\n        guard try selection().revision == expected else',
             'try beforeRename?()\n        guard fd >= 0 else', selected + 'testAtomicSelectionCASAndFailureBeforeRename'),
            ('catalog-durability', catalog, 'parentSync(fd) == 0)', 'parentSync(fd) != 42)', selected + 'testSelectionParentSyncFailureIsPublishedButUnknown'),
            ('catalog-no-follow', catalog, 'O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC', 'O_RDONLY | O_NONBLOCK | O_CLOEXEC', selected + 'testStrictSelectionRejectsDuplicateBooleanAndSymlink'),
            ('catalog-policy', CONTROLLER, 'signature: handle.read("release.sig", limit: 64), trustedPolicy: PluginSignatureFile.read(trustedPolicy, limit: 131072)',
             'signature: handle.read("release.sig", limit: 64), trustedPolicy: handle.read("policy.json", limit: 131072)', selected + 'testCurrentPolicyRevocationCannotUseStoredPolicyAsTrust'),
            ('catalog-identity', CONTROLLER, 'let proof = try authenticated.bind(.init(id: metadata.id, version: metadata.version,\n                        publisher: metadata.publisher, packageSHA256: metadata.packageDigest))',
             'let proof = authenticated.proof', selected + 'testAlteredPayloadCannotBorrowStoredSignature'),
            ('catalog-ui', view, 'await draft.select(version, controller: controller)', 'Optional<ExtensionController.VersionSelectionResult>.none', 'PluginInstalledVersionsUITests.testCurrentPolicySelectDisableAndRollbackControls'),
            ('catalog-live-owner', CONTROLLER, 'self.invalidateSourceDecisions(pluginID: metadata.id)',
             'self.runtime?.stop(); self.invalidateSourceDecisions(pluginID: metadata.id)', live),
        ]
    if args.case:
        if not args.mutate: parser.error('--case requires --mutate')
        unknown = set(args.case) - {recipe[0] for recipe in recipes}
        if unknown: parser.error('unknown mutation group(s): ' + ', '.join(sorted(unknown)))
        recipes = [recipe for recipe in recipes if recipe[0] in set(args.case)]
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
        for name in ['build', 'patches', 'plugins']:
            (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir()
        env['GHOSTERM_TEST_SIGNED_RELEASE'] = str(fixture)
        package = source / 'apple/GhostermApp'

        def run(label, selected, count=1, red=False):
            lifetime = selected.startswith('PluginPaneFlowTests.')
            child_env = dict(env)
            if lifetime:
                child_env.update(GHOSTERM_TEST_GTERM=str(args.gterm), GHOSTERM_APP_TEST_PACKAGE=str(package), GHOSTERM_APP_TEST_FILTER=selected)
                for key in list(child_env):
                    if key.startswith('GHOSTERM_SSH_'): del child_env[key]
                payload = ['bash', 'scripts/apple-app-gate.sh']
            else: payload = ['swift', 'test', '--package-path', str(package), '--filter', selected]
            command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require native-signature; shift; exec "$gt_timeout_bin" --kill-after=8 350 "$@"',
                       'native-signature', str(ROOT), *payload]
            child = subprocess.Popen(command, cwd=ROOT, env=child_env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                output, _ = child.communicate(timeout=360)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try: child.communicate(timeout=8)
                    except subprocess.TimeoutExpired: os.killpg(child.pid, signal.SIGKILL); child.communicate()
            log = output.decode(errors='replace')
            if lifetime: log += (ROOT / 'tmp/apple-app-test.log').read_text()
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
        if args.versions:
            baseline = '(PluginDirectorySignatureTests|PluginSignatureUITests|PluginReleaseInstallTests|PluginReleaseInstallUITests|PluginReleaseCatalogTests|PluginInstalledVersionsUITests)'
            baseline_count = 41
        if args.remove:
            baseline = '(PluginDirectorySignatureTests|PluginSignatureUITests|PluginReleaseInstallTests|PluginReleaseInstallUITests|PluginReleaseCatalogTests|PluginInstalledVersionsUITests|PluginReleaseRemovalTests)'
            baseline_count = 63
        run('baseline', baseline, count=baseline_count)
        if args.versions: run('live-version-baseline', live)
        if args.remove: run('live-removal-baseline', 'PluginPaneFlowTests.testManagedPreviewRetainsVersionUntilStopped')
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
    scope = ('macOS explicit native removal preview/confirmation, bounded intent and explicit recovery; actual managed SSH Runtime lease, no automatic replay or remote cleanup' if args.remove else
             'macOS version inventory, explicit policy revalidation, atomic select/disable/rollback, and active owned SSH Runtime; no permission persistence' if args.versions else
             'macOS signed ZIP version publication and native controls; no upgrade manager or runtime grant' if args.install else
             'CLI public release to immutable Core directory attestation and native controls; no ZIP import/install')
    out.write_text(json.dumps(dict(schemaVersion=1, scope=scope,
        baselineTests=baseline_count, lifetimeTests=2 if args.remove else 1 if args.versions else 0, installation=args.install, versions=args.versions, removal=args.remove,
        inputs=inputs, binaries=binaries, xctestSHA256=binary_sha, commands=commands, mutations=groups), indent=2) + '\n')
    print(f'NATIVE SIGNATURE PASS: {baseline_count} tests, {len(groups)} mutation/control/restore groups; {out}', flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise SystemExit(128 + number)
    for number in [signal.SIGINT, signal.SIGTERM]:
        signal.signal(number, interrupted)
    main()
