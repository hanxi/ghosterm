#!/usr/bin/env python3
"""Compile isolated macOS Codex full-review mutations; require normal assertion reds."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
ASSEMBLER = 'apple/GhostermApp/Sources/GhostermAppModel/CodexChangeReview.swift'
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/AppModel.swift'
VIEW = 'apple/GhostermApp/Sources/GhostermApp/CodexChangeReviewView.swift'
TEST = 'apple/GhostermApp/Tests/GhostermAppTests/CodexChangeReviewTests.swift'
CASES = (
    ('identity', ASSEMBLER, 'chunk.snapshotID == reference.snapshotID', 'true', 1,
     'testAssemblerRejectsMismatchedIdentity', 'Snapshot identity must match'),
    ('offset', ASSEMBLER, 'chunk.offset == nextOffset', 'true', 1,
     'testAssemblerRejectsDiscontinuousOffsets', 'Chunks must be contiguous'),
    ('utf16', ASSEMBLER, 'chunk.offset + chunk.content.utf16.count', 'chunk.offset + chunk.content.count', 1,
     'testAssemblerUsesUTF16OffsetsForEmoji', 'UTF16 offsets must count surrogate pairs'),
    ('bytes', ASSEMBLER,
     'if chunk.done && receivedBytes + data.count != reference.totalBytes { throw CodexChangeReadError.invalid }',
     'if false { throw CodexChangeReadError.invalid }', 1,
     'testAssemblerRejectsEOFWithWrongActualByteCount', 'EOF must match advertised UTF8 bytes'),
    ('stale', MODEL, 'guard isCurrent() else { self.cancelCodexChangeReview(); return }',
     '// mutation removed scope check', 3,
     'testChangedReferenceDiscardsAnUncancelledLateRead', 'Changed references must discard late data'),
    ('readonly', VIEW, 'view.isEditable = false', 'view.isEditable = true', 1,
     'testNativeReadOnlyViewContainsLinesBeyondTheOldPreviewLimit', 'Full diff text must remain read only'),
    ('unicode', VIEW, '!view.string.utf8.elementsEqual(text.utf8)', 'view.string != text', 1,
     'testNativeTextUpdatePreservesExactUnicodeBytes', 'Full diff must update byte-distinct Unicode text'),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, source, env, *, assertion=None):
    result = subprocess.run(command, cwd=source, env=env, capture_output=True, text=True, timeout=300)
    output = result.stdout + result.stderr
    if assertion:
        valid = result.returncode != 0 and assertion in output and 'Executed 1 test, with 1 failure' in output
        valid = valid and not any(value in output for value in (
            'unexpected signal', 'AddressSanitizer', 'runtime error:', 'fatal error:'))
    else:
        valid = result.returncode == 0 and 'Executed 1 test, with 0 failures' in output
    if not valid:
        raise RuntimeError(f'Unexpected result from {command}:\n{output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    selected = [case for case in CASES if not args.case or case[0] in args.case]
    (ROOT / 'tmp').mkdir(exist_ok=True)
    hashes = {name: sha(ROOT / name) for name in (ASSEMBLER, MODEL, VIEW, TEST)}
    with tempfile.TemporaryDirectory(prefix='codex-review-mutation-', dir=ROOT / 'tmp') as temporary:
        source = Path(temporary)
        for name in ('GhostermApp', 'GhostermCore', 'GhostermTerminal'):
            shutil.copytree(ROOT / 'apple' / name, source / 'apple' / name,
                            symlinks=True, ignore=shutil.ignore_patterns('.build'))
        (source / 'build').symlink_to(ROOT / 'build', target_is_directory=True)
        (source / 'core').symlink_to(ROOT / 'core', target_is_directory=True)
        (source / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(source / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1')
        if 'DEVELOPER_DIR' not in env and Path('/Applications/Xcode.app/Contents/Developer').is_dir():
            env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
        swift = ['swift', 'test', '--package-path', str(source / 'apple/GhostermApp'), '--filter']
        control = 'CodexChangeReviewTests.testReferenceRejectsBooleanCountsNoncanonicalIDsAndOversizedBudgets'
        for name, relative, old, new, count, test, assertion in selected:
            target = source / relative
            original = target.read_bytes()
            if original.count(old.encode()) != count:
                raise RuntimeError(f'{name}: mutation anchor changed')
            command = swift + ['CodexChangeReviewTests.' + test]
            run(command, source, env)
            target.write_bytes(original.replace(old.encode(), new.encode()))
            run(command, source, env, assertion=assertion)
            run(swift + [control], source, env)
            target.write_bytes(original)
            if sha(target) != hashes[relative]:
                raise RuntimeError(f'{name}: restored SHA256 mismatch')
            run(command, source, env)
            print(f'PASS {name}: Swift compiled assertion red / same build control green / restored green / SHA256', flush=True)
    if {name: sha(ROOT / name) for name in hashes} != hashes:
        raise RuntimeError('Original worktree changed during isolated mutations')
    print(f'CODEX REVIEW MUTATIONS PASS: {len(selected)} groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
