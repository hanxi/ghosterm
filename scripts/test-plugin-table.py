#!/usr/bin/env python3
"""Compiled table contracts and actual packaged Core/QuickJS page outcomes."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_table_output(text, total):
    values = [json.loads(line) for line in text.splitlines()]
    assert len(values) == (3 if total == 0 else 6), 'exact actual outcome count'
    descriptor = values[0]
    assert descriptor['kind'] == 'success' and descriptor['value']['total'] == total, 'descriptor'
    descriptor = descriptor['value']
    for outcome in values[1:-1]:
        assert outcome['kind'] == 'success', 'confirmed page'
        page = outcome['value']
        assert page['viewId'] == descriptor['viewId'] and page['revision'] == descriptor['revision'], 'same instance/revision'
        assert len(page['rows']) == min(16, total - page['offset']), 'bounded exact page'
        for index, row in enumerate(page['rows']):
            n = page['offset'] + index + 1
            assert row['id'] == str(n) and row['cells'] == list(map(str, (n, n*n, n*n*n))), 'exact computed row'
    assert values[-1]['kind'] == 'unknown', 'stale revision cannot succeed'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args(); build, tools = args.build.resolve(), args.tools.resolve()
    if not build.is_relative_to(ROOT / 'tmp'): parser.error('use an owned enabled Core build in project tmp')
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): parser.error('receipt must be in project tmp')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('core/src', 'core/include', 'packages/plugin-sdk', 'plugins/official/table')
        for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('scripts/test-plugin-table.py', 'scripts/test-plugin-table.mjs', 'scripts/build-plugin-sample.py',
        'scripts/probes/plugin-table.c', 'scripts/plugin-sdk-test-loader.mjs', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_TABLE_MUTATION' in (ROOT / p).read_bytes() for p in inputs if p.endswith('.ts')):
        raise RuntimeError('leftover table mutation marker')
    with tempfile.TemporaryDirectory(prefix='table-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; (source / 'tmp').mkdir(parents=True)
        for folder in ('packages/plugin-sdk', 'plugins/official/table', 'scripts'):
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copyfile(ROOT / 'LICENSE', source / 'LICENSE')
        env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp'), 'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer'}
        logs = work / 'logs'; logs.mkdir(); commands = []
        def run(label, command, assertion=False):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require table; shift; exec "$gt_timeout_bin" "$@"', 'table', ROOT, 180, *command]
            p = subprocess.Popen(list(map(str, bounded)), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, start_new_session=True)
            try: out, err = p.communicate(timeout=180)
            except BaseException:
                if p.poll() is None:
                    try: os.killpg(p.pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    if p.poll() is None:
                        try: os.killpg(p.pid, signal.SIGKILL)
                        except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            (logs / (label + '.log')).write_text(out + err); commands.append(dict(label=label, returncode=p.returncode))
            if assertion:
                if p.returncode != 1 or 'AssertionError [ERR_ASSERTION]' not in err or any(s in err for s in ('SyntaxError', 'ReferenceError', 'TypeError:', 'runtime error:', 'AddressSanitizer')):
                    raise RuntimeError(label + ': expected compiled normal JS assertion\n' + out + err)
            elif p.returncode: raise RuntimeError(label + '\n' + out + err)
            return out
        sdk = source / 'tmp/sdk'; package = source / 'tmp/package'
        def compile(label):
            if sdk.exists(): shutil.rmtree(sdk)
            if package.exists(): shutil.rmtree(package)
            run(label + '-sdk', ['node', tools / 'node_modules/typescript/bin/tsc', '-p', source / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
            run(label + '-package', ['python3', source / 'scripts/build-plugin-sample.py', '--source', source / 'plugins/official/table', '--tools', tools, '--out', package])
            env.update(GHOSTERM_TEST_PLUGIN_SDK_ENTRY=str(sdk / 'index.js'), GHOSTERM_TABLE_EXECUTOR=str(package / 'executor/index.js'), GHOSTERM_TABLE_CLIENT=str(package / 'client/index.js'))
        def node_test(label, mode='all', red=False):
            return run(label, ['node', '--experimental-loader', source / 'scripts/plugin-sdk-test-loader.mjs', source / 'scripts/test-plugin-table.mjs', mode], assertion=red)
        def actual(label, total, red=False):
            text = run(label, [build.parent / 'probe', package, 'executor', total])
            try: verify_table_output(text, int(total))
            except AssertionError as error:
                if red: return str(error)
                raise
            if red: raise RuntimeError(label + ': wrong business behavior survived')
        compile('baseline'); node_test('baseline-values')
        actual('baseline-core', 1000000); actual('baseline-empty', 0)
        if run('baseline-client', [build.parent / 'probe', package, 'client', 0]).strip() != 'CLIENT_UNSUPPORTED': raise RuntimeError('missing renderer was not unsupported')
        baseline = {p: sha(source / p) for p in ('packages/plugin-sdk/portable/table.ts', 'packages/plugin-sdk/portable/codec.ts', 'plugins/official/table/executor/index.ts')}
        cases = [
            ('row-limit', 'packages/plugin-sdk/portable/table.ts', 'TABLE_PAGE_ROWS_MAX = 16', 'TABLE_PAGE_ROWS_MAX = 17', 'codec'),
            ('snapshot', 'packages/plugin-sdk/portable/table.ts', 'r.revision !== d.revision || p.revision !== d.revision', 'false', 'codec'),
            ('complete-page', 'packages/plugin-sdk/portable/table.ts', 'p.rows.length !== Math.min(r.limit, d.total - r.offset)', 'false', 'codec'),
            ('row-identity', 'packages/plugin-sdk/portable/table.ts', 'new Set(value.rows.map(r => r.id)).size !== value.rows.length', 'false', 'codec'),
            ('body-limit', 'packages/plugin-sdk/portable/table.ts', 'utf8Bytes(encode(result)) > TABLE_BODY_BYTES_MAX', '(void utf8Bytes(encode(result)), false)', 'codec'),
            ('positive-revision', 'packages/plugin-sdk/portable/table.ts', 'uint64(value, true)', 'uint64(value)', 'codec'),
            ('page-offset', 'plugins/official/table/executor/index.ts', 'input.offset + index + 1', 'index + 1', 'core'),
            ('decimal-cube', 'plugins/official/table/executor/index.ts', '(n * n * n).toString()', 'Number(n * n * n).toString()', 'core'),
        ]
        results = []
        for label, filename, old, new, mode in cases if args.mutate else []:
            path = source / filename; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError('mutation anchor: ' + label)
            artifact = sdk / Path(filename).with_suffix('.js').name if filename.startswith('packages/') else package / 'executor/index.js'
            before = sha(artifact)
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_TABLE_MUTATION\n')
                compile(label)
                if sha(artifact) == before: raise RuntimeError('compiled target unchanged')
                if mode == 'core': actual(label + '-red', 1000000, True)
                else: node_test(label + '-red', mode, True)
                run(label + '-control', [build / 'test_config', ROOT / 'core/tests/fixtures'])
            finally: path.write_bytes(original)
            compile(label + '-restore')
            if mode == 'core': actual(label + '-restored', 1000000)
            else: node_test(label + '-restored', mode)
            if sha(path) != baseline[filename]: raise RuntimeError('source restoration mismatch')
            results.append(dict(group=label, target='actual Core package outcomes' if mode == 'core' else 'compiled Portable value contract'))
            print('PASS ' + label + ': compiled assertion red / independent Core configuration green / restore / SHA256', flush=True)
        node_test('final-values'); actual('final-core', 1000000)
        run('deterministic-package', ['python3', source / 'scripts/build-plugin-sample.py', '--source', source / 'plugins/official/table', '--tools', tools, '--out', package, '--check'])
        if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('root inputs changed')
        receipt = dict(accepted=True, inputs=inputs, commands=commands, mutations=results,
            sdk={p.name: sha(p) for p in sdk.glob('*.js')}, integrity=sha(package / 'integrity.json'),
            core=sha(build / 'libghosterm_core.a'), probe=sha(build.parent / 'probe'),
            scope='56 Portable value/fixture checks; real public C package/Session/QuickJS Executor outcomes and unsupported Client; no Native renderer, device, SSH or release')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print('TABLE PASS: compiled contracts / actual Core paged Executor / ' + str(len(results)) + ' mutations', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
