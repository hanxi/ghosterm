#!/usr/bin/env python3
"""Run the checked Codex package through real Core Runtimes and gterm, without a model turn."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='CMake build with gterm and test_gterm_codex')
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-codex')
    parser.add_argument('--codex', default='codex')
    args = parser.parse_args()
    executable = Path(shutil.which(args.codex) or args.codex).resolve(strict=True)
    package = args.package.resolve(strict=True)
    build = args.build.resolve(strict=True)
    version = json.loads((ROOT / 'plugins/official/ai-codex/protocol/app-server.lock.json').read_text())['codexCliVersion']
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='codex-runtime-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for name in ('home', 'config', 'project', 'codex'):
            (work / name).mkdir()
        environment = {'HOME': str(work / 'home'), 'XDG_CONFIG_HOME': str(work / 'config'),
                       'CODEX_HOME': str(work / 'codex'), 'TMPDIR': str(work),
                       'PATH': os.environ.get('PATH', '/usr/bin:/bin')}
        observation = subprocess.run([executable, '--version'], cwd=work, env=environment,
                                     capture_output=True, text=True, timeout=10, check=True)
        observed = observation.stdout.strip()
        if observed != version:
            raise SystemExit(f'expected pinned {version}, got {observed}')
        digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
        executable_digest = hashlib.sha256(executable.read_bytes()).hexdigest()
        result = subprocess.run([build / 'test_gterm_codex', build / 'gterm', package, digest,
                                 work / 'project', executable, version, executable_digest],
                                cwd=work, env=environment, capture_output=True, text=True, timeout=45)
        print(result.stdout, end='')
        if result.returncode or any(word in result.stderr for word in ('AddressSanitizer', 'runtime error:', 'ABNORMAL')):
            raise SystemExit(f'Codex Runtime fixture failed ({result.returncode}):\n{result.stderr}')
        if 'PASS Codex checked package' not in result.stdout:
            raise SystemExit('Runtime fixture completed without its assertions')
        print('CODEX RUNTIME PASS: isolated pinned CLI, no model or user configuration')


if __name__ == '__main__':
    main()
