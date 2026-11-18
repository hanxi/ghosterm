#!/usr/bin/env python3
"""Run the Native AppModel Codex no-model test against an owned loopback sshd."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TEST = 'CodexAppModelTests.testCodex(ThreadThroughAppModelAndIsolatedSSH|StoppedThreadRequestCannotAffectReplacementSession)'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-codex')
    parser.add_argument('--codex', default='codex')
    parser.add_argument('--route', choices=('direct', 'jump', 'both'), default='direct')
    args = parser.parse_args()
    gterm = args.gterm.resolve(strict=True)
    package = args.package.resolve(strict=True)
    codex = Path(shutil.which(args.codex) or args.codex).resolve(strict=True)
    selected = TEST if args.route == 'direct' else ('CodexAppModelTests.*Jump' if args.route == 'jump' else 'CodexAppModelTests')
    expected = {'direct': 2, 'jump': 2, 'both': 4}[args.route]
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1',
               GHOSTERM_TEST_GTERM=str(gterm),
               GHOSTERM_TEST_CODEX=str(codex), GHOSTERM_TEST_CODEX_PACKAGE=str(package),
               GHOSTERM_APP_TEST_FILTER=selected)
    # Only the gate's disposable loopback daemons may supply SSH endpoints.
    for key in list(env):
        if key.startswith('GHOSTERM_SSH_'):
            del env[key]
    env.pop('GHOSTERM_TEST_CODEX_JUMP', None)
    if args.route != 'direct':
        env['GHOSTERM_TEST_CODEX_JUMP'] = '1'
    result = subprocess.run(['bash', 'scripts/apple-app-gate.sh'], cwd=ROOT, env=env, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
    log = (ROOT / 'tmp/apple-app-test.log').read_text()
    summary = f'Executed {expected} test' + ('s' if expected != 1 else '') + ', with 0 failures'
    if 'CodexAppModelTests' not in log or 'skipped' in log.lower() or summary not in log:
        raise SystemExit('Codex AppModel tests did not execute the selected count without skips/failures')
    print(f'CODEX APP SSH PASS: {args.route}, {expected} cases; real CLI thread/events/stop and selected route guards; no model turn')


if __name__ == '__main__':
    main()
