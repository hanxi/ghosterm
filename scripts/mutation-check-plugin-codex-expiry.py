#!/usr/bin/env python3
"""Verify Codex approval expiry assertions in disposable protocol and Swift copies."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CLIENT = 'plugins/official/ai-codex/protocol/client.ts'
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/ExtensionInstanceModel.swift'
NODE_TARGET = 'turn completion expires only matching unanswered approvals without writing a decision'
NODE_CONTROL = 'turn/start emits the stable thread input shape'
SWIFT_TARGET = 'ExtensionControllerTests.testCodexApprovalButtonsExpireOnCompletionAndUncertainTerminalEvents'
SWIFT_CONTROL = 'ExtensionControllerTests.testRelayBootstrapValidatesAndQuotesEveryArgument'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, cwd, env, *, passed=True, marker=''):
    result = subprocess.run(command, cwd=cwd, env=env, capture_output=True, text=True, timeout=300)
    output = result.stdout + result.stderr
    if passed:
        valid = result.returncode == 0 and (not marker or marker in output)
    else:
        valid = result.returncode != 0 and marker in output and (
            'ℹ fail 1' in output or 'Executed 1 test, with 1 failure' in output)
        valid = valid and not any(value in output for value in (
            'AddressSanitizer', 'runtime error:', 'unexpected signal', 'fatal error:'))
    if not valid:
        raise RuntimeError(f'Unexpected result from {command}:\n{output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--apple', action='store_true', help='also compile and mutate the macOS AppModel')
    parser.add_argument('--fetch', action='store_true', help='fetch the integrity-locked TypeScript toolchain')
    args = parser.parse_args()
    (ROOT / 'tmp').mkdir(exist_ok=True)
    inputs = [CLIENT, MODEL, 'scripts/test-plugin-codex-protocol.mjs',
              'apple/GhostermApp/Tests/GhostermAppTests/ExtensionControllerTests.swift']
    before = {name: sha(ROOT / name) for name in inputs}
    with tempfile.TemporaryDirectory(prefix='codex-expiry-mutation-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for relative in ('plugins/official/ai-codex', 'packages/plugin-sdk/portable',
                         'scripts/fixtures/plugin-sdk-tools'):
            shutil.copytree(ROOT / relative, work / relative)
        shutil.copyfile(ROOT / 'scripts/test-plugin-codex-protocol.mjs', work / 'scripts/test-plugin-codex-protocol.mjs')
        (work / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(work / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1')
        tools = work / 'scripts/fixtures/plugin-sdk-tools'
        install = ['npm', 'ci', '--prefix', str(tools), '--ignore-scripts', '--no-audit', '--no-fund',
                   '--cache', str(ROOT / 'tmp/plugin-codex-npm-cache'), '--registry', 'https://registry.npmjs.org']
        if not args.fetch:
            install.append('--offline')
        run(install, work, env)
        tsc = ['node', str(tools / 'node_modules/typescript/bin/tsc'), '-p',
               str(work / 'plugins/official/ai-codex/tsconfig.json')]
        node = ['node', '--experimental-strip-types', '--test']

        def node_test(name, **options):
            run(node + [f'--test-name-pattern={name}', 'scripts/test-plugin-codex-protocol.mjs'], work, env, **options)

        target = work / CLIENT
        original = target.read_bytes()
        anchor = b'if (request.params.threadId === threadId && request.params.turnId === turn.id) {'
        if original.count(anchor) != 1:
            raise RuntimeError('Protocol expiry mutation anchor changed')
        run(tsc, work, env)
        node_test(NODE_TARGET, marker='ℹ pass 1')
        target.write_bytes(original.replace(anchor, b'if (false) {', 1))
        run(tsc, work, env)
        node_test(NODE_TARGET, passed=False, marker='completed turn approval must expire before any decision is written')
        node_test(NODE_CONTROL, marker='ℹ pass 1')
        target.write_bytes(original)
        if sha(target) != before[CLIENT]:
            raise RuntimeError('Protocol restore SHA256 mismatch')
        run(tsc, work, env)
        node_test(NODE_TARGET, marker='ℹ pass 1')
        print('PASS protocol expiry: compiled assertion red / same source control green / restored green / SHA256', flush=True)

        if args.apple:
            for name in ('GhostermApp', 'GhostermCore', 'GhostermTerminal'):
                shutil.copytree(ROOT / 'apple' / name, work / 'apple' / name,
                                symlinks=True, ignore=shutil.ignore_patterns('.build'))
            (work / 'build').symlink_to(ROOT / 'build', target_is_directory=True)
            (work / 'core').symlink_to(ROOT / 'core', target_is_directory=True)
            if 'DEVELOPER_DIR' not in env and Path('/Applications/Xcode.app/Contents/Developer').is_dir():
                env['DEVELOPER_DIR'] = '/Applications/Xcode.app/Contents/Developer'
            swift = ['swift', 'test', '--package-path', str(work / 'apple/GhostermApp'), '--filter']
            target = work / MODEL
            original = target.read_bytes()
            anchor = b'notificationMethod == "turn/completed"'
            if original.count(anchor) != 1:
                raise RuntimeError('AppModel expiry mutation anchor changed')
            run(swift + [SWIFT_TARGET], work, env, marker='Executed 1 test, with 0 failures')
            target.write_bytes(original.replace(anchor, b'notificationMethod == "mutation/never"', 1))
            run(swift + [SWIFT_TARGET], work, env, passed=False, marker='XCTAssertTrue failed - notification')
            run(swift + [SWIFT_CONTROL], work, env, marker='Executed 1 test, with 0 failures')
            target.write_bytes(original)
            if sha(target) != before[MODEL]:
                raise RuntimeError('AppModel restore SHA256 mismatch')
            run(swift + [SWIFT_TARGET], work, env, marker='Executed 1 test, with 0 failures')
            print('PASS AppModel expiry: compiled assertion red / same build control green / restored green / SHA256', flush=True)
    if {name: sha(ROOT / name) for name in inputs} != before:
        raise RuntimeError('Original worktree changed during isolated mutations')
    print('CODEX EXPIRY MUTATIONS PASS', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
