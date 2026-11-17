#!/usr/bin/env python3
"""Mutate only ConfigStore in a prepared native checkout below project tmp/.

The checkout must have its own Core/Swift build outputs and pinned dependencies.
Tests use disposable files, never the user's ~/.ssh/config. No SSH service or UI
test is run. Every case requires a compiled single assertion failure, an
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


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', type=Path, required=True)
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
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
    source, tests = checkout / SOURCE, checkout / TESTS
    original, test_bytes = source.read_bytes(), tests.read_bytes()
    main_bytes = (ROOT / SOURCE).read_bytes()
    environment = {**os.environ, 'TMPDIR': str(scratch), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1'}
    command = ['swift', 'test', '--package-path', str(checkout / 'apple/GhostermApp'),
               '--cache-path', str(scratch / 'spm-cache'), '--skip-update', '--filter']
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
        run('baseline', BASELINE)
        for name, before, after, target in CASES:
            if args.case and name not in args.case:
                continue
            text = original.decode()
            if text.count(before) != 1:
                raise RuntimeError(f'{name}: mutation anchor must match exactly once')
            source.write_text(text.replace(before, after))
            run(name + '-red', 'ConfigStoreTests.' + target, red=True)
            run(name + '-control', CONTROL)
            source.write_bytes(original)
            run(name + '-restored', 'ConfigStoreTests.' + target)
            if source.read_bytes() != original or tests.read_bytes() != test_bytes:
                raise RuntimeError('restored source/test SHA256 differs')
            print(f'PASS {name}: single assertion red, control green, restored green, SHA256', flush=True)
        run('final', BASELINE)
    finally:
        source.write_bytes(original)
        if tests.read_bytes() != test_bytes or (ROOT / SOURCE).read_bytes() != main_bytes:
            raise RuntimeError('test inputs or main checkout changed during the scan')
        (work / 'receipt.json').write_text(json.dumps({
            'source_sha256': sha(original), 'tests_sha256': sha(test_bytes),
            'scope': 'ConfigStore real native XCTest on disposable files; no user SSH data or services',
            'records': records,
        }, indent=2) + '\n')
    print(f'PASS ConfigStore mutation scan: {work / "receipt.json"}', flush=True)


if __name__ == '__main__':
    main()
