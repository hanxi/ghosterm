#!/usr/bin/env python3
"""Native Claude controls over owned SSH and Lua, using a no-model CLI fixture."""
import argparse
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-claude')
    parser.add_argument('--official-cli', type=Path, help='probe the pinned CLI in a network-denied, logged-out workspace')
    parser.add_argument('--selected-environment', action='store_true', help='verify scoped environment with the no-model CLI')
    parser.add_argument('--disconnect', action='store_true', help='disconnect the owned relay during a confirmed input')
    parser.add_argument('--route', choices=['direct', 'jump', 'both'], default='both')
    args = parser.parse_args()
    if args.official_cli and args.selected_environment:
        parser.error('--selected-environment requires the no-model CLI fixture')
    if args.disconnect and (args.official_cli or args.selected_environment):
        parser.error('--disconnect uses the plain no-model CLI fixture')
    gterm, package = args.gterm.resolve(strict=True), args.package.resolve(strict=True)
    workspace_tmp = ROOT / 'tmp'
    workspace_tmp.mkdir(exist_ok=True)
    parent = Path(os.environ.get('TMPDIR', str(workspace_tmp))).resolve()
    if not parent.is_relative_to(workspace_tmp) or not parent.is_dir():
        parent = workspace_tmp
    with tempfile.TemporaryDirectory(prefix='agent-', dir=parent) as temporary:
        work = Path(temporary)
        if args.official_cli:
            program = args.official_cli.resolve(strict=True)
            home = work / 'home'
            home.mkdir()
            version = subprocess.run([program, '--version'], check=True, capture_output=True, text=True,
                                     env=dict(PATH='/usr/bin:/bin', HOME=str(home), TMPDIR=temporary), timeout=10)
            if version.stdout.strip() != '2.1.323 (Claude Code)':
                raise RuntimeError('Expected Claude Code 2.1.323')
        else:
            program = work / 'claude-cli-fixture'
            fixture = ROOT / 'scripts/probes/claude-lua-cli.py'
            program.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                               shlex.quote(str(fixture)) + ' "$@"\n')
            program.chmod(0o700)
        environment = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary,
                           GHOSTERM_APP_TEST_TMP=temporary, GHOSTERM_TEST_GTERM=str(gterm),
                           GHOSTERM_TEST_CLAUDE_PROGRAM=str(program), GHOSTERM_TEST_CLAUDE_PACKAGE=str(package),
                           GHOSTERM_AUTOMATION_OFFSCREEN='1',
                           GHOSTERM_APP_TEST_FILTER='ClaudeAppModelTests' if args.route == 'both' else
                               'ClaudeAppModelTests.testClaudeConversationOver' + ('DirectSSH' if args.route == 'direct' else 'ProxyJump'))
        for name in list(environment):
            if name.startswith('GHOSTERM_SSH_'): del environment[name]
        environment.pop('GHOSTERM_TEST_CODEX_JUMP', None)
        environment['GHOSTERM_TEST_CLAUDE_LOGGED_OUT'] = '1' if args.official_cli else '0'
        environment['GHOSTERM_TEST_CLAUDE_SELECTED_ENV'] = '1' if args.selected_environment else '0'
        environment['GHOSTERM_TEST_CLAUDE_DISCONNECT'] = '1' if args.disconnect else '0'
        if args.route != 'direct': environment['GHOSTERM_TEST_CODEX_JUMP'] = '1'
        command = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-app; gt_timeout 600 bash "$1/scripts/apple-app-gate.sh"',
                   'claude-app', str(ROOT)]
        result = subprocess.run(command, cwd=ROOT, env=environment)
        if result.returncode: raise SystemExit(result.returncode)
        expected = 2 if args.route == 'both' else 1
        log = (work / 'apple-app-test.log').read_text()
        passes = sum(line.startswith("Test Case '-[GhostermAppTests.ClaudeAppModelTests ")
                     and "' passed (" in line for line in log.splitlines())
        if passes != expected or 'skipped' in log.lower():
            raise RuntimeError('Native Claude fixture cases did not all execute without skips/failures')
        backend = 'official CLI, logged out' if args.official_cli else 'no-model CLI fixture'
        print(f'CLAUDE LUA APP PASS: {expected} owned SSH cases; {backend}')


if __name__ == '__main__': main()
