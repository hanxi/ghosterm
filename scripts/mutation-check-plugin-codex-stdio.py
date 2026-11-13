#!/usr/bin/env python3
"""Compile isolated stdio mutations; require assertion red, control green and restoration."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'plugins/official/ai-codex/protocol/stdio.ts'
CASES = (
    ('slices', 'this.#lines.push(line);', 'this.client.receiveLine(line);',
     'stdio decodes and dispatches in separate slices before observing EOF',
     'decoding must defer business dispatch to the next owner slice'),
    ('queue', 'this.#lines.length >= CODEX_PENDING_LINES_MAX', 'false',
     'stdio bounds its decoded line queue before business dispatch',
     'decoded line queue must reject overflow'),
    ('stderr', 'if (!this.#stderrEOF) this.#drainStderr(CODEX_PIPE_CHUNK_MAX);', 'void 0;',
     'stdio continues stderr drainage while dispatching queued lines',
     'business dispatch must not block stderr drainage'),
)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, root, env, assertion=None):
    result = subprocess.run(['bash', '-c',
        '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require codex-stdio; shift; gt_timeout 90 "$@"',
        'codex-stdio', str(ROOT), *command], cwd=root, env=env, capture_output=True, text=True)
    output = result.stdout + result.stderr
    valid = result.returncode == 0 if assertion is None else (
        result.returncode == 1 and 'AssertionError [ERR_ASSERTION]' in output and
        'ℹ fail 1' in output and assertion in output)
    if not valid:
        raise RuntimeError(f'Unexpected result from {command}:\n{output}')
    return output


def main():
    (ROOT / 'tmp').mkdir(exist_ok=True)
    inputs = [SOURCE, 'scripts/test-plugin-codex-protocol.mjs']
    before = {name: sha(ROOT / name) for name in inputs}
    with tempfile.TemporaryDirectory(prefix='codex-stdio-mutation-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for relative in ('plugins/official/ai-codex', 'packages/plugin-sdk/portable',
                         'scripts/fixtures/plugin-sdk-tools'):
            shutil.copytree(ROOT / relative, work / relative)
        shutil.copyfile(ROOT / inputs[1], work / inputs[1])
        (work / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(work / 'tmp'))
        tools = work / 'scripts/fixtures/plugin-sdk-tools'
        run(['npm', 'ci', '--prefix', str(tools), '--ignore-scripts', '--no-audit', '--no-fund',
             '--cache', str(ROOT / 'tmp/plugin-codex-npm-cache'), '--offline'], work, env)
        strict = ['node', str(tools / 'node_modules/typescript/bin/tsc'), '-p',
                  str(work / 'plugins/official/ai-codex/tsconfig.json')]
        def test(name, assertion=None):
            output = run(['node', '--experimental-strip-types', '--test',
                f'--test-name-pattern=^{name}$', 'scripts/test-plugin-codex-protocol.mjs'], work, env, assertion)
            if assertion is None and 'ℹ pass 1' not in output:
                raise RuntimeError(f'Target did not execute: {name}\n{output}')
        target = work / SOURCE
        original = target.read_bytes()
        for name, old, new, test_name, assertion in CASES:
            if original.count(old.encode()) != 1:
                raise RuntimeError(f'{name}: mutation anchor changed')
            run(strict, work, env)
            test(test_name)
            try:
                target.write_bytes(original.replace(old.encode(), new.encode(), 1))
                run(strict, work, env)
                test(test_name, assertion)
                test('turn/start emits the stable thread input shape')
            finally:
                target.write_bytes(original)
            if sha(target) != before[SOURCE]:
                raise RuntimeError(f'{name}: restored source digest mismatch')
            run(strict, work, env)
            test(test_name)
            print(f'PASS {name}: compiled assertion red / independent control green / restored green / SHA256', flush=True)
    if {name: sha(ROOT / name) for name in inputs} != before:
        raise RuntimeError('Original source changed during isolated mutations')
    print('CODEX STDIO MUTATIONS PASS: 3 groups', flush=True)


if __name__ == '__main__':
    main()
