#!/usr/bin/env python3
"""Compile isolated Native Claude document/launch mutations, without windows or a model."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODEL = Path('apple/GhostermApp/Sources/GhostermAppModel')
CLIENT = 'ClaudeRPCClient.swift'
BOOT = 'ExtensionRelayBootstrap.swift'
MARK = 'GT_CLAUDE_SWIFT_MUTATION'
COMPLETE = 'testCompleteUnicodeParametersAndReplyAreNeverReserializedOrTruncated'
PAGES = 'testReplyPageIdentityOffsetsCountsAndCompletionMustMatch'
IDENTITY = 'testChildReplyIdentityAndDuplicateFieldsCannotBecomeSuccess'
LAUNCH = 'testRelayNodePrefixArgumentsAndSelectedEnvironmentAreLiteralAndOrdered'
CASES = [
    ('complete-upload', CLIENT, 'String(data: parameters, encoding: .utf8)',
     'String(data: parameters, encoding: .utf8)?.replacingOccurrences(of: "中文", with: "")', COMPLETE),
    ('page-identity', CLIENT, 'page.responseID == receipt.responseID', 'true', PAGES),
    ('page-bytes', CLIENT, 'page.totalBytes == receipt.totalBytes', 'true', PAGES),
    ('page-completion', CLIENT, 'page.done == (page.nextOffset == receipt.totalUnits)', 'true', PAGES),
    ('child-id', CLIENT, 'response.id == "command-\\(next)"', 'true', IDENTITY),
    ('child-sequence', CLIENT, 'response.sequence == next', 'true', IDENTITY),
    ('duplicate-json', BOOT, 'keys.insert(Data(key.utf8)).inserted', 'true', IDENTITY),
    ('late-scope', CLIENT, 'guard valid else', 'guard true else',
     'testInvalidationRejectsLateReceiptAndBusyCannotDispatchSecondAction'),
    ('unknown-no-replay', CLIENT, '} catch {\n            valid = false', '} catch {\n            valid = true',
     'testUnknownActionCannotBeRetriedAndObservationIdentityRemains'),
    ('fixed-arguments', BOOT, '["--process-argument", program.name, argument]',
     '["--process-argument", program.name, program.name]', LAUNCH),
    ('selected-environment', BOOT, 'for name in request.processEnvironmentNames', 'for name in [String]()', LAUNCH),
    ('prefix-count', BOOT, 'program.prefixArguments.count <= 16', 'program.prefixArguments.count <= 17',
     'testRelayRejectsInvalidPrefixAndEnvironmentBeforeLaunch'),
]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    originals = {file: (ROOT / MODEL / file).read_bytes() for file in [CLIENT, BOOT]}
    for name, file, old, _, _ in CASES:
        if originals[file].count(old.encode()) != 1 or MARK.encode() in originals[file]:
            raise RuntimeError(name + ': mutation preflight mismatch')
    with tempfile.TemporaryDirectory(prefix='claude-swift-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'source'; scratch = work / 'scratch'
        for package in ['GhostermCore', 'GhostermTerminal', 'GhostermApp']:
            shutil.copytree(ROOT / 'apple' / package, source / 'apple' / package,
                            symlinks=True, ignore=shutil.ignore_patterns('.build', '__pycache__'))
        # Only this new suite and the owned AppModel fixture are needed here.
        tests = source / 'apple/GhostermApp/Tests/GhostermAppTests'
        for entry in tests.iterdir():
            if entry.name not in ['ClaudeRPCClientTests.swift', 'AppModelFixture.swift']:
                if entry.is_dir(): shutil.rmtree(entry)
                else: entry.unlink()
        (source / 'build').symlink_to(ROOT / 'build', target_is_directory=True)
        (source / 'core').mkdir(); (source / 'core/build').symlink_to(ROOT / 'core/build', target_is_directory=True)
        shutil.copytree(ROOT / 'core/include', source / 'core/include')
        env = dict(os.environ, TMPDIR=str(work), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
        def test(target=None, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-swift; shift; gt_timeout 240 "$@"'
            command = ['swift', 'test', '--verbose', '--package-path', source / 'apple/GhostermApp', '--scratch-path', scratch,
                       '--filter', 'ClaudeRPCClientTests' + ('.' + target if target else '')]
            result = subprocess.run(['bash', '-c', shell, 'claude-swift', str(ROOT), *map(str, command)],
                                    env=env, cwd=ROOT, text=True, capture_output=True, timeout=250)
            output = result.stdout + result.stderr
            if red:
                accepted = result.returncode == 1 and re.search(r'Executed 1 test, with 1 failure(?:s)? \(0 unexpected\)', output)
                accepted = accepted and ('XCTAssert' in output or 'failed - Claude' in output)
                accepted = accepted and not any(token in output for token in ['emit-module command failed', 'Fatal error:', 'unexpected signal'])
            else:
                accepted = result.returncode == 0 and f'Executed {1 if target else 8} test' in output and 'skipped' not in output.lower()
            if not accepted: raise RuntimeError(('Expected compiled XCTest assertion red' if red else 'Expected green suite') + '\n' + output)
            return output
        test(); print('BASELINE PASS: eight Native Claude document/launch tests', flush=True)
        for name, file, old, new, target in CASES:
            path = source / MODEL / file
            try:
                path.write_bytes(originals[file].replace(old.encode(), new.encode(), 1) + ('\n// ' + MARK + '\n').encode())
                output = test(target, red=True)
                compiled = re.search(r'-primary-file\s+[\"\']?' + re.escape(str(path)), output)
                compiled = compiled or re.search(r'^(?:Compile|Compiling) ' + re.escape(file) + r'$', output, re.M)
                if not compiled:
                    raise RuntimeError(name + ': source was not recompiled:\n' + output)
                test(COMPLETE if file == BOOT or target == LAUNCH else LAUNCH)
            finally: path.write_bytes(originals[file])
            test(target)
            if sha(path) != hashlib.sha256(originals[file]).hexdigest(): raise RuntimeError('restore digest mismatch')
            print(f'PASS {name}: compiled assertion red / independent XCTest green / restored green / SHA256', flush=True)
        test()
    if any((ROOT / MODEL / file).read_bytes() != original for file, original in originals.items()):
        raise RuntimeError('working production source changed')
    print(f'CLAUDE SWIFT PASS: {len(CASES)} isolated mutation groups; working sources unchanged')


def stop(number, frame): raise KeyboardInterrupt(str(number))


if __name__ == '__main__':
    signal.signal(signal.SIGINT, stop); signal.signal(signal.SIGTERM, stop)
    main()
