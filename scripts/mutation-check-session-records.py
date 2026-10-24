#!/usr/bin/env python3
"""Private durable session metadata: isolated ordinary assertion mutations."""
import hashlib, os, shutil, signal, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/session-record-mutations'
MARK = 'GT_SESSION_RECORD_MUTATION_MARK'
REL = Path('Sources/GhostermAppModel/SessionRecordStore.swift')

def run(args):
    p = subprocess.Popen([str(x) for x in args], cwd=ROOT, start_new_session=True,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        out, _ = p.communicate(timeout=180)
        return p.returncode, out
    finally:
        if p.poll() is None:
            os.killpg(p.pid, signal.SIGTERM)
            try: p.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL); p.wait()

def main():
    source = ROOT / 'apple/GhostermApp' / REL
    original = source.read_bytes(); digest = hashlib.sha256(original).digest()
    package = WORK / 'apple/GhostermApp'; copy = package / REL
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()): return 2
    def interrupted(sig, frame): raise RuntimeError('session record mutation interrupted: ' + str(sig))
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    os.environ['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
    if WORK.exists(): shutil.rmtree(WORK)
    (WORK / 'apple').mkdir(parents=True)
    for name in ('GhostermApp', 'GhostermCore', 'GhostermTerminal'):
        shutil.copytree(ROOT / 'apple' / name, WORK / 'apple' / name,
                        symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
    for name in ('core', 'terminal', 'build', 'assets'):
        (WORK / name).symlink_to(ROOT / name, target_is_directory=True)
    def restore():
        copy.write_bytes(original)
        if hashlib.sha256(copy.read_bytes()).digest() != digest or hashlib.sha256(source.read_bytes()).digest() != digest:
            raise RuntimeError('session record SHA256 rollback failed')
    def probe(test, red=False):
        rc, out = run(['xcrun', 'swift', 'test', '--package-path', package, '--filter', test])
        (WORK / ('last-red.log' if red else 'last-green.log')).write_text(out)
        if red:
            if rc != 1 or 'XCTAssert' not in out or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:
                raise RuntimeError('ordinary record assertion red required: ' + out[-6000:])
        elif rc or 'with 0 failures' not in out:
            raise RuntimeError('record baseline/control failed: ' + out[-6000:])
    specs = [
        ('stop uncertainty overwritten', '(disposition != .session || old.disposition == .session)', 'true', 'testStopUncertaintyAndEndedStateCannotBeLostByStaleWriter'),
        ('ended state resurrected', '(old.disposition != .ended || disposition == .ended)', 'true', 'testStopUncertaintyAndEndedStateCannotBeLostByStaleWriter'),
        ('known result kept active', '(session > 0 && disposition == .ended)', '(session > 0)', 'testStopUncertaintyAndEndedStateCannotBeLostByStaleWriter'),
        ('version', 'envelope.format == 1', 'envelope.format >= 1', 'testCorruptionAndFutureVersionNeverOverwrite'),
        ('file permissions', '(info.st_mode & 0o077) == 0\n    }', '(info.st_mode & 0o077) >= 0\n    }', 'testSymlinkAndPermissionsRejected'),
        ('directory permissions', '(info.st_uid == geteuid() && (info.st_mode & 0o077) == 0)', '(info.st_uid == geteuid())', 'testSymlinkAndPermissionsRejected'),
        ('file symlink', 'O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK', 'O_RDONLY | O_CLOEXEC | O_NONBLOCK', 'testSymlinkAndPermissionsRejected'),
        ('known identity', '(old.session == 0 || session == old.session)', '(old.session == 0 || session > 0)', 'testInvalidMetadataAndAuthorityRegressionRejected'),
        ('stop uncertainty', 'disposition != .stopUnknown || stopStatus == nil', 'disposition != .stopUnknown || stopStatus != nil', 'testInvalidMetadataAndAuthorityRegressionRejected'),
        ('record count', 'envelope.records.count <= Self.maxRecords', 'envelope.records.count <= Self.maxRecords + 1', 'testBoundedValidJSONAndRecordCount'),
        ('byte limit', 'private static let maxBytes = 1_048_576', 'private static let maxBytes = 2_097_152', 'testBoundedValidJSONAndRecordCount'),
        ('creation nonce', 'record.conflicts(with: $0)', 'false && record.conflicts(with: $0)', 'testWritersReloadAndPreserveAuthority'),
    ]
    try:
        restore(); probe('SessionRecordStoreTests')
        for label, old, new, test in specs:
            text = original.decode()
            if text.count(old) != 1: raise RuntimeError('record mutation anchor drift/' + label)
            try:
                copy.write_text(text.replace(old, new) + '\n// ' + MARK + '\n')
                probe('SessionRecordStoreTests.' + test, red=True)
                probe('HostListTests.testReadingDoesNotMutateConfig')
                print('PASS session record mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally: restore()
        probe('SessionRecordStoreTests')
        print('PASS 12 session record mutations and SHA256 rollback', flush=True)
    finally: restore()
    return 0
if __name__ == '__main__': sys.exit(main())
