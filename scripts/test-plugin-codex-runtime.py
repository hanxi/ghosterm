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
    parser.add_argument('--package', type=Path, help='Use an existing Lua package instead of building one')
    parser.add_argument('--codex', default='codex')
    args = parser.parse_args()
    executable = Path(shutil.which(args.codex) or args.codex).resolve(strict=True)
    build = args.build.resolve(strict=True)
    version = json.loads((ROOT / 'plugins/official/ai-codex/protocol/app-server.lock.json').read_text())['codexCliVersion']
    project_tmp = ROOT / 'tmp'
    project_tmp.mkdir(exist_ok=True)
    scratch = Path(os.environ.get('TMPDIR', project_tmp)).resolve()
    if not scratch.is_dir() or not scratch.is_relative_to(project_tmp):
        scratch = project_tmp
    with tempfile.TemporaryDirectory(prefix='codex-runtime-', dir=scratch) as temporary:
        work = Path(temporary)
        package = args.package.resolve(strict=True) if args.package else work / 'package'
        if args.package is None:
            subprocess.run(['python3', '-B', ROOT / 'scripts/gterm-plugin', 'build',
                            ROOT / 'plugins/official/ai-codex', '--out', package],
                           cwd=ROOT, check=True, timeout=30)
        for name in ('home', 'config', 'project', 'codex'):
            (work / name).mkdir()
        environment = {'HOME': str(work / 'home'), 'XDG_CONFIG_HOME': str(work / 'config'),
                       'CODEX_HOME': str(work / 'codex'), 'TMPDIR': str(work), 'TMP': str(work), 'TEMP': str(work),
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
