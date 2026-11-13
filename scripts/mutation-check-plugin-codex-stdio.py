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
DECODER = 'plugins/official/ai-codex/protocol/line-decoder.ts'
CLIENT = 'plugins/official/ai-codex/protocol/client.ts'
UTF8 = 'plugins/official/ai-codex/protocol/utf8.ts'
CASES = (
    ('slices', SOURCE, 'this.#lines.push(line);', 'this.client.receiveLine(line);',
     'stdio decodes and dispatches in separate slices before observing EOF',
     'decoding must defer business dispatch to the next owner slice'),
    ('queue', SOURCE, 'this.#lines.length >= CODEX_PENDING_LINES_MAX', 'false',
     'stdio bounds its decoded line queue before business dispatch',
     'decoded line queue must reject overflow'),
    ('stderr', SOURCE, 'if (!this.#stderrEOF) this.#drainStderr(CODEX_PIPE_CHUNK_MAX);', 'void 0;',
     'stdio continues stderr drainage while dispatching queued lines',
     'business dispatch must not block stderr drainage'),
    ('validation-slice', SOURCE, 'this.#dispatch = this.client.prepareLine(line);', 'this.client.receiveLine(line);',
     'stdio decodes and dispatches in separate slices before observing EOF',
     'JSON validation must defer business dispatch to its own owner slice'),
    ('decode-carry', DECODER, 'if (end - start < width) end = start;', 'if (false) end = start;',
     'incremental line decoding preserves split code points at block and pipe boundaries',
     'incremental UTF-8 must retain an incomplete code point across decode blocks'),
    ('decode-budget', DECODER, 'this.lineBytes += count;', 'this.lineBytes = count;',
     'incremental line decoding retains aggregate byte and partial EOF limits',
     'flushed fragments must retain the complete line byte budget'),
    ('decode-eof', DECODER, 'if (this.lineBytes !== 0)', 'if (this.length !== 0)',
     'incremental line decoding retains aggregate byte and partial EOF limits',
     'fully decoded fragments must still reject EOF before newline'),
    ('dispatch-once', CLIENT, 'if (consumed) return;', 'if (false) return;',
     'prepared protocol messages dispatch once and cannot deliver after close',
     'prepared protocol frames must dispatch once'),
    ('utf8-width', UTF8, '"xxxx"', '"xx"',
     'UTF-8 length preserves every scalar width and rejects unpaired surrogates',
     'UTF-8 scalar byte width must be exact'),
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
    inputs = [SOURCE, DECODER, CLIENT, UTF8, 'scripts/test-plugin-codex-protocol.mjs']
    before = {name: sha(ROOT / name) for name in inputs}
    with tempfile.TemporaryDirectory(prefix='codex-stdio-mutation-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for relative in ('plugins/official/ai-codex', 'packages/plugin-sdk/portable',
                         'scripts/fixtures/plugin-sdk-tools'):
            shutil.copytree(ROOT / relative, work / relative)
        shutil.copyfile(ROOT / inputs[-1], work / inputs[-1])
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
        for name, source, old, new, test_name, assertion in CASES:
            target = work / source
            original = target.read_bytes()
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
            if sha(target) != before[source]:
                raise RuntimeError(f'{name}: restored source digest mismatch')
            run(strict, work, env)
            test(test_name)
            print(f'PASS {name}: compiled assertion red / independent control green / restored green / SHA256', flush=True)
    if {name: sha(ROOT / name) for name in inputs} != before:
        raise RuntimeError('Original source changed during isolated mutations')
    print(f'CODEX STDIO MUTATIONS PASS: {len(CASES)} groups', flush=True)


if __name__ == '__main__':
    main()
