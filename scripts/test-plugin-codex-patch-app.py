#!/usr/bin/env python3
"""Native patch/approval RPC over owned SSH, using an explicit no-model app-server fixture."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-codex')
    parser.add_argument('--route', choices=('direct', 'jump', 'both'), default='both')
    args = parser.parse_args()
    gterm = args.gterm.resolve(strict=True)
    package = args.package.resolve(strict=True)
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='codex-patch-run-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        program = work / 'ghosterm-codex-protocol-fixture'
        import shlex
        program.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                           shlex.quote(str(ROOT / 'scripts/probes/codex-patch-server.py')) + ' "$@"\n')
        program.chmod(0o700)
        selected = 'CodexPatchRPCAppTests' if args.route == 'both' else \
            'CodexPatchRPCAppTests.test(PatchAndApprovalOverDirectSSH|LostTurnReplyOverDirectSSH)' if args.route == 'direct' else \
            'CodexPatchRPCAppTests.testPatchAndApprovalOverProxyJump'
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_TEST_GTERM=str(gterm),
                   GHOSTERM_TEST_CODEX_PACKAGE=str(package), GHOSTERM_TEST_CODEX_PATCH_FIXTURE=str(program),
                   GHOSTERM_APP_TEST_FILTER=selected, GHOSTERM_AUTOMATION_OFFSCREEN='1')
        for key in list(env):
            if key.startswith('GHOSTERM_SSH_'):
                del env[key]
        env.pop('GHOSTERM_TEST_CODEX_JUMP', None)
        if args.route != 'direct':
            env['GHOSTERM_TEST_CODEX_JUMP'] = '1'
        command = ['bash', '-c',
            '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require codex-patch-app; gt_timeout 300 bash "$1/scripts/apple-app-gate.sh"',
            'codex-patch-app', str(ROOT)]
        result = subprocess.run(command, cwd=ROOT, env=env)
        if result.returncode:
            raise SystemExit(result.returncode)
        expected = {'both': 3, 'direct': 2, 'jump': 1}[args.route]
        log = (ROOT / 'tmp/apple-app-test.log').read_text()
        summary = f'Executed {expected} test' + ('s' if expected != 1 else '') + ', with 0 failures'
        if summary not in log or 'skipped' in log.lower():
            raise RuntimeError('Patch RPC tests did not execute the exact selected count without skips')
        print(f'CODEX PATCH APP PASS: {args.route}; {expected} cases; real RPC and Native UI; explicit protocol fixture, no model')


if __name__ == '__main__':
    main()
