#!/usr/bin/env python3
"""Compile isolated Claude portable bridge mutations; no SDK/model/network."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PLUGIN = Path('plugins/official/ai-claude')
DOCS = 'Claude documents require'
VERSIONS = 'Claude version and grant checks'
CASES = [
    ('consume-once', 'executor/documents.ts', 'const text = this.input.text; this.input = undefined; return text;',
     'const text = this.input.text; return text;', DOCS),
    ('reserve-before-effect', 'executor/documents.ts', 'this.reserved > RESPONSE_BUDGET - RESPONSE_MAX',
     'false', 'Claude reply reservations'),
    ('complete-pages', 'executor/documents.ts', 'content: text.slice(offset, end)',
     'content: text.slice(offset, end).replace(/中/g, "")', DOCS),
    ('response-id', 'protocol/process.ts', 'value.id !== pending.id', 'false', 'Claude transport handles'),
    ('response-sequence', 'protocol/process.ts', 'value.sequence !== pending.sequence', 'false', 'Claude transport refuses'),
    ('canonical-json', 'protocol/json.ts', 'canonical && JSON.stringify(value) !== text', 'false', 'Claude transport refuses'),
    ('backend-version', 'protocol/process.ts', 'value.result.sdkVersion !== CLAUDE_SDK_VERSION', 'false', 'Claude transport refuses'),
    ('response-byte-limit', 'protocol/line-decoder.ts', 'count > MAX_NODE_RESPONSE_BYTES - this.lineBytes', 'false', 'Claude decoder preserves'),
    ('cancelled-handoff', 'executor/index.ts', 'disposed || request.cancelled', 'disposed', 'Claude bridge checks cancellation'),
    ('node-minimum', 'protocol/process.ts', 'Number(version[1]) < CLAUDE_NODE_MIN_MAJOR', 'false', VERSIONS),
]


def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tools', type=Path, required=True, help='locked plugin-sdk-tools install; read only')
    args = parser.parse_args()
    tools = args.tools.resolve()
    originals = {p: digest(p) for p in (ROOT / PLUGIN).rglob('*.ts') if 'node' not in p.relative_to(ROOT / PLUGIN).parts}
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    with tempfile.TemporaryDirectory(prefix='claude-portable-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'source'; emitted = work / 'emitted'
        shutil.copytree(ROOT / PLUGIN, source / PLUGIN, ignore=shutil.ignore_patterns('node'))
        shutil.copytree(ROOT / 'packages/plugin-sdk', source / 'packages/plugin-sdk')
        env['GHOSTERM_TEST_PLUGIN_SDK_ENTRY'] = str(emitted / 'packages/plugin-sdk/portable/index.js')
        env['GHOSTERM_CLAUDE_PACKAGE_ENTRY'] = str(emitted / PLUGIN / 'executor/index.js')
        def run(command):
            return subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, text=True, timeout=120)
        def compile_source():
            shutil.rmtree(emitted, ignore_errors=True)
            result = run(['node', tools / 'node_modules/typescript/bin/tsc', '-p', source / PLUGIN / 'tsconfig.json',
                          '--noEmit', 'false', '--rewriteRelativeImportExtensions', 'true', '--rootDir', source,
                          '--outDir', emitted, '--noEmitOnError', 'true'])
            if result.returncode: raise RuntimeError('Compilation is not assertion red:\n' + result.stdout + result.stderr)
        def test(pattern=None, red=False):
            command = ['node', '--test', '--test-reporter=tap', '--experimental-loader', ROOT / 'scripts/plugin-sdk-test-loader.mjs']
            if pattern: command.extend(['--test-name-pattern', '^' + pattern])
            result = run([*command, ROOT / 'scripts/test-plugin-claude-entry.mjs'])
            output = result.stdout + result.stderr
            if red:
                if result.returncode != 1 or "code: 'ERR_ASSERTION'" not in output or '# fail 1' not in output:
                    raise RuntimeError('Expected exactly one compiled ordinary assertion failure:\n' + output)
            elif result.returncode or f'# pass {1 if pattern else 8}' not in output:
                raise RuntimeError('Expected green test count:\n' + output)
        compile_source(); test()
        print('BASELINE PASS: eight compiled portable bridge tests; no OS child/model', flush=True)
        for name, file, old, new, target in CASES:
            path = source / PLUGIN / file; before = path.read_bytes()
            if before.count(old.encode()) != 1: raise RuntimeError(name + ': anchor changed')
            try:
                path.write_bytes(before.replace(old.encode(), new.encode(), 1))
                compile_source(); test(target, red=True)
                test(DOCS if target == VERSIONS else VERSIONS)
            finally: path.write_bytes(before)
            compile_source(); test(target)
            if digest(path) != hashlib.sha256(before).hexdigest(): raise RuntimeError('restore digest mismatch')
            print(f'PASS {name}: compiled assertion red / independent control green / restored green / SHA256', flush=True)
        test()
    if any(digest(path) != value for path, value in originals.items()): raise RuntimeError('working source changed')
    print(f'CLAUDE PORTABLE PASS: {len(CASES)} isolated mutation groups; production source unchanged')


if __name__ == '__main__': main()
