#!/usr/bin/env python3
"""Run the Native AppModel Codex no-model test against an owned loopback sshd."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TEST = 'CodexAppModelTests.testCodexThreadThroughAppModelAndIsolatedSSH'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-codex')
    parser.add_argument('--codex', default='codex')
    args = parser.parse_args()
    gterm = args.gterm.resolve(strict=True)
    package = args.package.resolve(strict=True)
    codex = Path(shutil.which(args.codex) or args.codex).resolve(strict=True)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_TEST_GTERM=str(gterm),
               GHOSTERM_TEST_CODEX=str(codex), GHOSTERM_TEST_CODEX_PACKAGE=str(package),
               GHOSTERM_APP_TEST_FILTER=TEST)
    result = subprocess.run(['bash', 'scripts/apple-app-gate.sh'], cwd=ROOT, env=env, check=False)
    if result.returncode:
        raise SystemExit(result.returncode)
    log = (ROOT / 'tmp/apple-app-test.log').read_text()
    if 'testCodexThreadThroughAppModelAndIsolatedSSH' not in log or 'skipped' in log.lower() or 'Executed 1 test, with 0 failures' not in log:
        raise SystemExit('Codex AppModel test did not execute exactly once without a skip/failure')
    print('CODEX APP SSH PASS: reviewed package, consent, real CLI thread/start, Native event receipt and stop; no model turn')


if __name__ == '__main__':
    main()
