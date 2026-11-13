#!/usr/bin/env python3
"""Native Claude controls over owned SSH/Node/real SDK, with an in-memory CLI fixture."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-claude')
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--observation', action='store_true', help='cancel an admitted start and exercise the production read-only observation button')
    parser.add_argument('--retained-reply', action='store_true', help='hold an actual SDK poll reply through the native deadline, then read the retained document')
    parser.add_argument('--route', choices=['direct', 'jump', 'both'], default='both')
    args = parser.parse_args()
    if args.observation and args.retained_reply: parser.error('--observation and --retained-reply are separate scenarios')
    gterm, package = args.gterm.resolve(strict=True), args.package.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix='claude-app-sdk-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); node = work / 'node'
        shutil.copytree(ROOT / 'plugins/official/ai-claude/node', node)
        environment = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
        install = ['npm', 'ci', '--prefix', node, '--ignore-scripts', '--no-audit', '--no-fund', '--cache',
                   ROOT / 'tmp/claude-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch: install.append('--offline')
        subprocess.run(list(map(str, install)), env=environment, check=True, capture_output=True, timeout=300)
        subprocess.run(['node', str(node / 'node_modules/typescript/bin/tsc'), '-p', str(node / 'tsconfig.json'), '--noEmitOnError'],
                       env=environment, check=True, timeout=120)
        environment.update(GHOSTERM_TEST_GTERM=str(gterm), GHOSTERM_TEST_CLAUDE_COMPONENT=str(node),
                           GHOSTERM_TEST_CLAUDE_PACKAGE=str(package), GHOSTERM_TEST_CLAUDE_NODE=shutil.which('node'),
                           GHOSTERM_AUTOMATION_OFFSCREEN='1',
                           GHOSTERM_APP_TEST_FILTER='ClaudeAppModelTests' if args.route == 'both' else
                               'ClaudeAppModelTests.testClaudeConversationOver' + ('DirectSSH' if args.route == 'direct' else 'ProxyJump'))
        environment.pop('GHOSTERM_TEST_CLAUDE_RETAINED', None)
        if args.retained_reply: environment['GHOSTERM_TEST_CLAUDE_RETAINED'] = '1'
        environment.pop('GHOSTERM_TEST_CLAUDE_OBSERVATION', None)
        if args.observation: environment['GHOSTERM_TEST_CLAUDE_OBSERVATION'] = '1'
        for name in list(environment):
            if name.startswith('GHOSTERM_SSH_'): del environment[name]
        environment.pop('GHOSTERM_TEST_CODEX_JUMP', None)
        if args.route != 'direct': environment['GHOSTERM_TEST_CODEX_JUMP'] = '1'
        command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-app; gt_timeout 600 bash "$1/scripts/apple-app-gate.sh"',
                   'claude-app', str(ROOT)]
        result = subprocess.run(command, cwd=ROOT, env=environment)
        if result.returncode: raise SystemExit(result.returncode)
        expected = 2 if args.route == 'both' else 1
        log = (ROOT / 'tmp/apple-app-test.log').read_text()
        if f'Executed {expected} test' not in log or '0 failures' not in log or 'skipped' in log.lower():
            raise RuntimeError('Native Claude SDK fixture cases did not all execute without skips/failures')
        scope = 'admitted cancellation/production observation UI; no claim that SDK started' if args.observation else 'production Native controls/Node/SDK, explicit no-model CLI fixture'
        if args.retained_reply: scope = 'actual SDK poll reply held through native timeout, exact retained document/Native readonly UI; no model'
        print(f'CLAUDE APP PASS: {expected} owned SSH cases; {scope}')


if __name__ == '__main__': main()
