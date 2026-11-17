#!/usr/bin/env python3
"""Mutate configuration storage/publication in an isolated native checkout below project tmp/.

The checkout must have its own Core/Swift build outputs and pinned dependencies.
Tests use disposable files, never the user's ~/.ssh/config. No SSH service is used; the form phase uses offscreen native controls. Every case requires a compiled single assertion failure, an
independent configuration-reading control and restored success with SHA256.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/ConfigStore.swift'
TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/ConfigStoreTests.swift'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'
BASELINE = 'ConfigStoreTests|ConfigEditTests|HostListTests'
CASES = [
    ('backup', 'operations.rename(directory, stagedBackup, backup, true) == 0',
     'operations.rename(directory, stagedBackup, backup, true) != Int32.min',
     'testBackupRenameFailureCannotBeIgnored'),
    ('atomic', 'guard operations.rename(directory, staged, name, identity != nil) == 0 else {',
     'unlinkat(directory, name, 0)\n            guard operations.rename(directory, staged, name, identity != nil) == 0 else {',
     'testOldPathStillExistsAtAtomicReplacement'),
    ('write-error', 'guard count > 0 else { throw Self.ioError() }',
     'if count < 0 { return }', 'testWriteFailureCannotPublishPartialConfiguration'),
    ('interruption', 'if count < 0, errno == EINTR { continue }\n                    guard count > 0',
     'guard count > 0', 'testShortWritesAndInterruptionsPreserveExactBytes'),
    ('short-write', 'offset += count', 'offset = raw.count',
     'testShortWritesAndInterruptionsPreserveExactBytes'),
    ('file-sync', 'operations.sync(fd) == 0', 'operations.sync(fd) != Int32.min',
     'testStagingSyncFailureStopsBeforeCommit'),
    ('backup-sync', 'operations.sync(directory) == 0 else { throw Self.ioError() }\n            }',
     'operations.sync(directory) != Int32.min else { throw Self.ioError() }\n            }',
     'testBackupDirectorySyncFailureStopsBeforeCommit'),
    ('close', 'operations.close(fd) == 0', 'operations.close(fd) != Int32.min',
     'testStagingCloseFailureStopsBeforeCommit'),
    ('committed', 'if committed { throw ConfigStoreError.committed',
     'if false && committed { throw ConfigStoreError.committed',
     'testPostCommitSyncFailureReportsEffectAndRequiresReload'),
    ('invalidate', 'loaded = .failure(ConfigStoreError.committed("需要重新加载"))',
     '// mutation: preserve stale loaded state',
     'testPostCommitSyncFailureReportsEffectAndRequiresReload'),
    ('recheck', 'try verify()\n            guard operations.rename(directory, staged, name, identity != nil)',
     'guard operations.rename(directory, staged, name, identity != nil)',
     'testExternalChangeAfterBackupIsRecheckedBeforeCommit'),
    ('exclusive', 'operations.rename(directory, staged, name, identity != nil)',
     'operations.rename(directory, staged, name, true)', 'testLateCreationCannotBeOverwrittenByFirstSave'),
    ('read-error', 'loaded = Result { try Self.snapshot(directory: AT_FDCWD, name: path) }',
     'loaded = .success((try? Self.snapshot(directory: AT_FDCWD, name: path)) ?? .missing)',
     'testReadErrorCannotBecomeMissingWithoutReload'),
    ('snapshot', 'guard try Self.snapshot(directory: directory, name: name) == expected else {',
     'guard true else {', 'testSameLengthChangeWithRestoredMtimeRefusesSave'),
    ('permissions', 'fchmod(fd, identity.mode & 0o777)', 'fchmod(fd, 0o600)',
     'testExistingPermissionsSurviveReplacement'),
    ('cleanup', 'defer { for item in temporary { unlinkat(directory, item, 0) } }',
     'defer { _ = temporary }', 'testFinalRenameFailurePreservesOriginalAndCleansStaging'),
    ('refresh', 'loaded = .success(refreshed)', '_ = refreshed',
     'testSuccessfulCommitRefreshesSnapshotForNextEdit'),
    ('parent', 'held.st_dev == current.st_dev, held.st_ino == current.st_ino',
     'true', 'testChangedParentDirectoryCannotReceiveOrHideCommit'),
    ('readback', 'actual == bytes else {', 'true else {',
     'testPostCommitChangedBytesAreReportedAsCommittedError'),
    ('coordination', 'flock(lock, LOCK_EX | LOCK_NB) == 0',
     'flock(lock, LOCK_EX | LOCK_NB) != Int32.min',
     'testCooperatingWriterCannotEnterBetweenCheckAndRename'),
    ('lock-link', 'lockIdentity.st_nlink == 1,', 'true,',
     'testHardlinkedCoordinationFileCannotBeAdopted'),
    ('lock-identity', 'currentLock.st_dev == lockIdentity.st_dev, currentLock.st_ino == lockIdentity.st_ino',
     'true', 'testReplacedCoordinationFileStopsCommit'),
]


PUBLICATION_SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift'
PUBLICATION_TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/ConfigEditTests.swift'
PUBLICATION_CASES = [
    ('candidate-isolation', 'try update(candidate)', 'try update(model)',
     'testSuccessfulEditPublishesOnlyAfterRealCommit'),
    ('publish-after-save', 'do { try store.save(candidate.serialized) }',
     'configModel = candidate; hosts = candidate.hosts\n        do { try store.save(candidate.serialized) }',
     'testFailedNewHostCannotRemainInTheModel'),
    ('main-snapshot', 'Data(candidate.rawConfig.utf8) == Data(model.rawConfig.utf8)', 'true',
     'testLoadStoreRaceCannotAdoptChangedMainBytes'),
    ('committed-reload', 'if case ConfigStoreError.committed = error { reload() }',
     '// mutation: retain the stale published model',
     'testCommittedErrorReloadsObservedDiskAndStillReportsFailure'),
    ('host-publication', 'hosts = candidate.hosts', '_ = candidate.hosts',
     'testSuccessfulEditPublishesOnlyAfterRealCommit'),
    ('wildcard-publication', 'wildcards = candidate.wildcardPatterns', '_ = candidate.wildcardPatterns',
     'testNewWildcardPublishesRuleListAfterCommit'),
]


FORM_SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/HostEditorDraft.swift'
FORM_TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/HostEditorDraftTests.swift'
FORM_CASES = [
    ('dirty-only', 'public var changed: Bool { enabled != originalEnabled || (enabled && Data(value.utf8) != originalValue) }',
     'public var changed: Bool { true }', 'testUnchangedPlainSnapshotProducesNoEdits'),
    ('identity-list', 'ownIdentities.map { $0.arguments[0] }', 'ownIdentities.prefix(1).map { $0.arguments[0] }',
     'testUnchangedSnapshotProducesNoEditsDespiteDuplicatesAndRawStyle'),
    ('first-explicit', 'rows.first { $0.keyword == key.lowercased() }', 'rows.last { $0.keyword == key.lowercased() }',
     'testUnchangedSnapshotProducesNoEditsDespiteDuplicatesAndRawStyle'),
    ('inheritance', 'enabled: own != nil, source: source', 'enabled: true, source: source',
     'testUnchangedSnapshotProducesNoEditsDespiteDuplicatesAndRawStyle'),
    ('decoded-quote', 'result.append((field.keyword, [Self.quote(field.value)]))', 'result.append((field.keyword, [field.value]))',
     'testScalarQuotingKeepsOneDecodedArgument'),
    ('disabled-roundtrip', '(enabled && Data(value.utf8) != originalValue)', '(Data(value.utf8) != originalValue)',
     'testDisabledUnchangedInheritanceDoesNotWriteAfterToggleRoundtrip'),
    ('included-readonly', 'info?.included == true ?', 'false ?', 'testIncludedHostIsExplicitlyReadOnly'),
    ('unicode-dirty', 'Data(value.utf8) != originalValue', 'value != String(decoding: originalValue, as: UTF8.self)',
     'testUnicodeNormalizationDifferenceIsStillDirty'),
]
HOST_SAVE_CASES = [
    ('new-alias', 'guard !$0.contains(alias: alias) else {', 'guard true else {', 'testNewAliasCannotReplaceExistingBlock'),
    ('rename', 'try $0.rename(alias: alias, to: replacement)', '_ = replacement', 'testRenameAndEditAreOneAtomicCommit'),
    ('rename-fields', 'if !edits.isEmpty { try $0.edit(alias: alias, edits) }', '_ = edits', 'testRenameAndEditAreOneAtomicCommit'),
    ('unicode-snapshot', 'Data(candidate.rawConfig.utf8) == Data(model.rawConfig.utf8)', 'candidate.rawConfig == model.rawConfig',
     'testByteDifferentUnicodeLoadStoreRaceIsAConflict'),
    ('stale-form', 'expected != configModel.map({ Data($0.rawConfig.utf8) })', 'expected != expected',
     'testStaleFormCannotOverwriteAnotherSavedEdit'),
]
HOST_LIST_SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/HostListModel.swift'
HOST_LIST_CASES = [
    ('negative-alias', 'if pattern.hasPrefix("!") || pattern.contains("*")', 'if pattern.contains("*")',
     'testNegatedPatternsCannotBecomeConnectableHosts'),
    ('duplicate-alias', 'guard seen.insert(HostEntry.patternKey(pattern)).inserted else { continue }',
     '// mutation: allow duplicate connectable rows', 'testRepeatedAliasesProduceOneConnectableRow'),
]
UI_SOURCE = 'apple/GhostermApp/Sources/GhostermApp/HostEditorSheet.swift'
UI_TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/HostEditorUITests.swift'
UI_CASES = [
    ('cancel', 'Button("取消") { dismiss() }', 'Button("取消") { save() }', 'testActualCancelDoesNotWriteEditedDraft'),
]
TARGET_TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/HostTargetIdentityTests.swift'
TARGET_MATCH_CASES = [
    ('unicode-match', 'Self.patternKey(pattern) == Self.patternKey(candidate)', 'pattern == candidate',
     'testUnicodeAliasesResolveToTheirOwnTargets'),
]
TARGET_SELECTION_CASES = [
    ('unicode-selection', 'return HostEntry.patternKey(a) == HostEntry.patternKey(b)', 'return a == b',
     'testTargetSelectionDoesNotFoldUnicodeNormalization'),
]

PRIVATE_SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/NativePrivateKeyStore.swift'
PRIVATE_TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/NativePrivateKeyStoreTests.swift'
PRIVATE_CASES = [
    ('private-cache', 'do {\n            let cached = try checked(access.get(path))\n            try checkCancellation(cancellation)\n            return cached\n        }',
     'do { throw KeychainStoreError.notFound }', 'testFirstImportThenAuthenticationSurvivesRemovedSource'),
    ('private-readback', 'let stored = try checked(access.get(path))\n        try checkCancellation(cancellation)',
     'let stored = imported\n        try checkCancellation(cancellation)', 'testPostImportReadFailureCannotYieldFileMaterial'),
    ('private-denial', 'catch KeychainStoreError.unexpectedStatus(let status) where status == errSecDuplicateItem {',
     'catch KeychainStoreError.unexpectedStatus {', 'testKeychainWriteFailureCannotYieldFileMaterial'),
    ('private-race', '// Another import won.', 'return imported // Another import won.', 'testConcurrentFirstImportUsesWinningKeychainRecord'),
    ('private-source', 'same(before, after), same(after, current), after.st_size == bytes.count,',
     'true, true, after.st_size == bytes.count,', 'testSourceReplacementDuringReadCannotBeImported'),
    ('private-permission', 'before.st_mode & 0o077 == 0', 'true', 'testSourcePermissionsSymlinksAndMalformedBytesAreRefused'),
    ('private-cancel', 'if cancellation?.isCancelled == true { throw GhostermError.status(8, "私钥读取已取消") }',
     '// mutation: ignore cancellation', 'testCancelledLookupCannotReadOrImportTheFile'),
    ('private-provider', 'if path.hasPrefix("ghosterm-identity:") {', 'if false {', 'testUnsupportedAndroidContainerReferenceIsExplicit'),
]
CREDENTIAL_SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/SSHCredentialSelector.swift'
CREDENTIAL_TESTS = 'apple/GhostermApp/Tests/GhostermAppTests/SSHCredentialSelectorTests.swift'
CREDENTIAL_CASES = [
    ('private-failure-routing', 'if let materialError { self.finish(request, .failure(materialError)); return }',
     'if let materialError { _ = materialError }', 'testKeychainFailureCannotBecomePasswordFallback'),
    ('private-cancel-routing', 'material = try reader(path, request.cancellation)',
     'material = try reader(path, SSHCancellation())', 'testStorageReceivesTheRequestCancellationToken'),
]
KEYCHAIN_SOURCE = 'apple/GhostermApp/Sources/GhostermAppModel/KeychainStore.swift'
KEYCHAIN_CASES = [
    ('private-utf8', 'guard let text = String(data: d, encoding: .utf8), !text.utf8.contains(0)',
     'guard let text = Optional(String(decoding: d, as: UTF8.self)), !text.utf8.contains(0)', 'testInvalidKeychainUTF8CannotBeReplacedByAFileFallback'),
]
PRIVATE_BINDING_CASES = [
    ('private-production', 'keyWithCancellation: { try NativePrivateKeyStore.load($0, cancellation: $1) }',
     'keyWithCancellation: { _, _ in nil }', 'testProductionSelectorUsesImportedMaterialAfterSourceRemoval'),
]
PRIVATE_UI_CASES = [
    ('private-import-button', 'try await model.reimportPrivateKey(reference)', '_ = reference', 'testActualKeyImportButtonStoresMaterialWithoutSavingTheForm'),
    ('private-import-error', '} catch { errorText = "\\(error)" }\n            importing = false',
     '} catch { _ = error }\n            importing = false', 'testActualKeyImportFailureIsVisibleWithoutSaving'),
]
PHASES = {
    'private-store': (PRIVATE_SOURCE, PRIVATE_TESTS, 'NativePrivateKeyStoreTests', PRIVATE_CASES),
    'private-credential': (CREDENTIAL_SOURCE, CREDENTIAL_TESTS, 'SSHCredentialSelectorTests', CREDENTIAL_CASES),
    'private-binding': (CREDENTIAL_SOURCE, PRIVATE_TESTS, 'NativePrivateKeyStoreTests', PRIVATE_BINDING_CASES),
    'private-utf8': (KEYCHAIN_SOURCE, PRIVATE_TESTS, 'NativePrivateKeyStoreTests', KEYCHAIN_CASES),
    'private-ui': (UI_SOURCE, UI_TESTS, 'HostEditorUITests', PRIVATE_UI_CASES),
    'target-match': (HOST_LIST_SOURCE, TARGET_TESTS, 'HostTargetIdentityTests', TARGET_MATCH_CASES),
    'target-selection': (PUBLICATION_SOURCE, TARGET_TESTS, 'HostTargetIdentityTests', TARGET_SELECTION_CASES),
    'store': (SOURCE, TESTS, 'ConfigStoreTests', CASES),
    'publication': (PUBLICATION_SOURCE, PUBLICATION_TESTS, 'ConfigEditTests', PUBLICATION_CASES),
    'form': (FORM_SOURCE, FORM_TESTS, 'HostEditorDraftTests', FORM_CASES),
    'host-save': (PUBLICATION_SOURCE, PUBLICATION_TESTS, 'ConfigEditTests', HOST_SAVE_CASES),
    'host-list': (HOST_LIST_SOURCE, FORM_TESTS, 'HostEditorDraftTests', HOST_LIST_CASES),
    'host-ui': (UI_SOURCE, UI_TESTS, 'HostEditorUITests', UI_CASES),
}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--phase', choices=tuple(PHASES), default='store')
    parser.add_argument('--case', action='append', choices=sorted({case[0] for case in [case for phase in PHASES.values() for case in phase[3]]}))
    args = parser.parse_args()
    checkout = args.checkout.resolve(strict=True)
    if ROOT / 'tmp' not in checkout.parents:
        parser.error('--checkout must be an isolated checkout under this project tmp/')
    actual = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', '--show-toplevel'], text=True).strip()
    if Path(actual).resolve() != checkout or checkout == ROOT:
        parser.error('refusing to mutate the main checkout or an ordinary directory')
    scratch = checkout / 'tmp'
    scratch.mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='config-mutations-', dir=scratch))
    source_name, tests_name, test_class, cases = PHASES[args.phase]
    if args.case and set(args.case) - {case[0] for case in cases}:
        parser.error('selected cases do not belong to this phase')
    source, tests = checkout / source_name, checkout / tests_name
    original, test_bytes = source.read_bytes(), tests.read_bytes()
    main_source = ROOT / source_name
    main_bytes = main_source.read_bytes() if main_source.exists() else None
    environment = {**os.environ, 'TMPDIR': str(scratch), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1'}
    command = ['swift', 'test', '--package-path', str(checkout / 'apple/GhostermApp'),
               '--cache-path', str(scratch / 'spm-cache'), '--skip-update', '--filter']
    baseline = BASELINE if args.phase in ('store', 'publication') else BASELINE + '|HostEditor|HostTargetIdentityTests'
    if args.phase.startswith('private-'):
        baseline += '|NativePrivateKeyStoreTests|SSHCredentialSelectorTests|KeychainStoreTests'
    records = []

    def run(label, selected, red=False):
        logfile = work / (label + '.log')
        with logfile.open('w') as output:
            result = subprocess.run([*command, selected], cwd=checkout, env=environment,
                                    stdout=output, stderr=subprocess.STDOUT, timeout=180)
        text = logfile.read_text()
        if red:
            accepted = (result.returncode == 1 and 'Build complete!' in text
                        and re.search(r'Executed 1 test, with 1 failures? \(0 unexpected\)', text)
                        and 'XCTAssert' in text
                        and not re.search(r'unexpected signal|Program crashed|error: emit-module|error: fatalError|AddressSanitizer|UndefinedBehaviorSanitizer', text))
        else:
            accepted = (result.returncode == 0 and
                        re.search(r'Executed [1-9][0-9]* tests?, with 0 failures', text)
                        and not re.search(r'Executed 0 tests|Test Case .* skipped', text))
        if not accepted:
            raise RuntimeError(f'{label}: expected {"normal assertion red" if red else "green"}; see {logfile}')
        records.append({'case': label, 'returncode': result.returncode, 'log': str(logfile.relative_to(checkout))})

    try:
        run('baseline', baseline)
        for name, before, after, target in cases:
            if args.case and name not in args.case:
                continue
            text = original.decode()
            if text.count(before) != 1:
                raise RuntimeError(f'{name}: mutation anchor must match exactly once')
            source.write_text(text.replace(before, after))
            run(name + '-red', test_class + '.' + target, red=True)
            run(name + '-control', CONTROL)
            source.write_bytes(original)
            run(name + '-restored', test_class + '.' + target)
            if source.read_bytes() != original or tests.read_bytes() != test_bytes:
                raise RuntimeError('restored source/test SHA256 differs')
            print(f'PASS {name}: single assertion red, control green, restored green, SHA256', flush=True)
        run('final', baseline)
    finally:
        source.write_bytes(original)
        if tests.read_bytes() != test_bytes or (main_source.read_bytes() if main_source.exists() else None) != main_bytes:
            raise RuntimeError('test inputs or main checkout changed during the scan')
        (work / 'receipt.json').write_text(json.dumps({
            'source_sha256': sha(original), 'tests_sha256': sha(test_bytes),
            'scope': 'Native configuration XCTest on disposable files; no user SSH data or services',
            'phase': args.phase, 'source': source_name, 'tests': tests_name,
            'records': records,
        }, indent=2) + '\n')
    print(f'PASS macOS configuration {args.phase} mutation scan: {work / "receipt.json"}', flush=True)


if __name__ == '__main__':
    main()
