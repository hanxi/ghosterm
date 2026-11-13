#!/usr/bin/env python3
"""Compile isolated Codex snapshot mutations and verify their targeted assertions."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'plugins/official/ai-codex/executor/changes.ts'
CASES = (
    ('identity', 'value => value.id === snapshotID', '() => true',
     'snapshot identity must select the requested document'),
    ('size', 'bytes > CODEX_CHANGE_MAX_BYTES', 'false',
     'oversized snapshots must be unavailable'),
    ('wire', 'encode({ kind: "success", value });', 'void value;',
     'complete snapshot chunks must fit the RPC outcome body'),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, root, env, *, assertion=None):
    result = subprocess.run(command, cwd=root, env=env, capture_output=True, text=True, timeout=300)
    output = result.stdout + result.stderr
    if assertion is None:
        valid = result.returncode == 0
    else:
        valid = result.returncode == 1 and 'AssertionError [ERR_ASSERTION]' in output and assertion in output
    if not valid:
        raise RuntimeError(f'Unexpected result from {command}:\n{output}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true', help='fetch integrity-locked developer tools')
    args = parser.parse_args()
    (ROOT / 'tmp').mkdir(exist_ok=True)
    inputs = [SOURCE, 'plugins/official/ai-codex/executor/index.ts', 'scripts/test-plugin-codex-entry.mjs']
    before = {name: sha(ROOT / name) for name in inputs}
    with tempfile.TemporaryDirectory(prefix='codex-changes-mutation-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for relative in ('plugins/official/ai-codex', 'packages/plugin-sdk/portable',
                         'scripts/fixtures/plugin-sdk-tools'):
            shutil.copytree(ROOT / relative, work / relative)
        for name in ('test-plugin-codex-entry.mjs', 'test-plugin-codex-protocol.mjs', 'plugin-sdk-test-loader.mjs'):
            shutil.copyfile(ROOT / 'scripts' / name, work / 'scripts' / name)
        (work / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(work / 'tmp'))
        env.pop('GHOSTERM_CODEX_PACKAGE_ENTRY', None)
        tools = work / 'scripts/fixtures/plugin-sdk-tools'
        install = ['npm', 'ci', '--prefix', str(tools), '--ignore-scripts', '--no-audit', '--no-fund',
                   '--cache', str(ROOT / 'tmp/plugin-codex-npm-cache'), '--registry', 'https://registry.npmjs.org']
        if not args.fetch:
            install.append('--offline')
        run(install, work, env)
        tsc = ['node', str(tools / 'node_modules/typescript/bin/tsc')]
        strict = tsc + ['-p', str(work / 'plugins/official/ai-codex/tsconfig.json')]
        sdk = work / 'sdk'
        run(tsc + ['--target', 'ES2022', '--module', 'ES2022', '--moduleResolution', 'Bundler',
                   '--lib', 'ES2022', '--strict', '--exactOptionalPropertyTypes', '--noEmitOnError',
                   '--outDir', str(sdk), str(work / 'packages/plugin-sdk/portable/index.ts')], work, env)
        env['GHOSTERM_TEST_PLUGIN_SDK_ENTRY'] = str(sdk / 'index.js')
        entry = ['node', '--experimental-strip-types', '--experimental-loader',
                 str(work / 'scripts/plugin-sdk-test-loader.mjs'), 'scripts/test-plugin-codex-entry.mjs']
        control = ['node', '--experimental-strip-types', '--test',
                   '--test-name-pattern=turn/start emits the stable thread input shape',
                   'scripts/test-plugin-codex-protocol.mjs']
        target = work / SOURCE
        original = target.read_bytes()
        run(strict, work, env)
        if 'CODEX EXECUTOR ENTRY PASS' not in run(entry, work, env):
            raise RuntimeError('Baseline entry assertions did not execute')
        for name, old, new, assertion in CASES:
            if original.count(old.encode()) != 1:
                raise RuntimeError(f'{name}: mutation anchor changed')
            target.write_bytes(original.replace(old.encode(), new.encode(), 1))
            run(strict, work, env)
            run(entry, work, env, assertion=assertion)
            if 'ℹ pass 1' not in run(control, work, env):
                raise RuntimeError(f'{name}: independent control did not execute')
            target.write_bytes(original)
            if sha(target) != before[SOURCE]:
                raise RuntimeError(f'{name}: restore digest mismatch')
            run(strict, work, env)
            if 'CODEX EXECUTOR ENTRY PASS' not in run(entry, work, env):
                raise RuntimeError(f'{name}: restored assertions did not execute')
            print(f'PASS {name}: compiled assertion red / independent control green / restored green / SHA256', flush=True)
    if {name: sha(ROOT / name) for name in inputs} != before:
        raise RuntimeError('Original source changed during isolated mutations')
    print('CODEX CHANGE MUTATIONS PASS: 3 groups', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
