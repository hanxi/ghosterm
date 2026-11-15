#!/usr/bin/env python3
"""Compiled diff contracts, actual public Core outcomes and isolated mutations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
SCENARIOS = {'change': ('alpha\r\nbeta\nlast', 'alpha\r\ngamma\nlast\n', 5),
    'empty': ('', '', 0), 'equal': ('same\n', 'same\n', 1),
    'unicode': ('é\n😀', 'e\u0301\n😀\n', 4), 'maximum': ('a\n' * 128, 'b\n' * 128, 256),
    'interleaved': ('a\nb\n' * 64, 'b\na\n' * 64, 129),
    'wide': (('😀' * 128 + '\n') * 3, ('😀' * 128 + '\n') * 3, 3)}


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_diff_output(text, scenario):
    before, after, total = SCENARIOS[scenario]
    values = [json.loads(line) for line in text.splitlines()]
    offsets = [*range(0, total, 3), total] if total else [0]
    assert len(values) == len(offsets) + 2, 'actual outcome count'
    assert values[0]['kind'] == 'success', 'descriptor confirmed'
    d = values[0]['value']
    assert d['total'] == total and d['pageSize'] == 3, 'computed complete comparison size'
    assert d['beforeLines'] == len(before.splitlines()) and d['afterLines'] == len(after.splitlines()), 'source line counts'
    old, new, rows, old_at, new_at = [], [], [], 0, 0
    endings = dict(lf='\n', crlf='\r\n', cr='\r', none='')
    for offset, outcome in zip(offsets, values[1:-1]):
        assert outcome['kind'] == 'success', 'page confirmed'
        p = outcome['value']
        assert p['viewId'] == d['viewId'] and p['revision'] == d['revision'], 'same instance and revision'
        assert p['offset'] == offset and p['total'] == total and len(p['rows']) == min(3, total - offset), 'exact selected page'
        assert (p['beforeOffset'], p['afterOffset']) == (old_at, new_at), 'continuous source cursors'
        assert len(json.dumps(p, ensure_ascii=False, separators=(',', ':')).encode()) <= 2048, 'bounded actual wire value'
        for row in p['rows']:
            assert row['kind'] in ('equal', 'delete', 'insert') and row['ending'] in endings, 'known row values'
            if row['kind'] != 'insert': old_at += 1; old.append(row['text'] + endings[row['ending']])
            if row['kind'] != 'delete': new_at += 1; new.append(row['text'] + endings[row['ending']])
            assert row['beforeLine'] == (0 if row['kind'] == 'insert' else old_at), 'exact before line'
            assert row['afterLine'] == (0 if row['kind'] == 'delete' else new_at), 'exact after line'
            rows.append(row)
    assert ''.join(old) == before and ''.join(new) == after, 'exact Unicode and EOL reconstruction'
    assert len(rows) == total and d['added'] == sum(r['kind'] == 'insert' for r in rows) and d['removed'] == sum(r['kind'] == 'delete' for r in rows), 'complete edit counts'
    if scenario == 'change': assert [r['kind'] for r in rows] == ['equal', 'delete', 'delete', 'insert', 'insert'], 'deterministic deletion-first ties'
    assert values[-1]['kind'] == 'unknown', 'stale revision never succeeds'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='enabled Core build with test_config')
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    build, probe, tools, receipt = (p.resolve() for p in (args.build, args.probe, args.tools, args.receipt))
    if any(not p.is_relative_to(ROOT / 'tmp') for p in (build, probe, tools, receipt)): parser.error('use project tmp inputs and receipt')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('core/src', 'core/include', 'packages/plugin-sdk', 'plugins/official/diff')
        for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('scripts/test-plugin-diff.py', 'scripts/test-plugin-diff.mjs', 'scripts/probes/plugin-diff.c',
                 'scripts/build-plugin-sample.py', 'scripts/plugin-sdk-test-loader.mjs', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_DIFF_MUTATION' in (ROOT / n).read_bytes() for n in inputs if n.endswith('.ts')): raise RuntimeError('leftover diff mutation')
    logs = receipt.parent / (receipt.stem + '-logs'); logs.mkdir(parents=True, exist_ok=False)
    commands, groups = [], []
    with tempfile.TemporaryDirectory(prefix='diff-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; (source / 'tmp').mkdir(parents=True)
        for folder in ('packages/plugin-sdk', 'plugins/official/diff', 'scripts'):
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copyfile(ROOT / 'LICENSE', source / 'LICENSE')
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
        def run(label, command, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-diff; shift; exec "$gt_timeout_bin" "$@"'
            child = subprocess.Popen(list(map(str, ['bash', '-c', shell, 'plugin-diff', ROOT, 180, *command])), cwd=ROOT,
                env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
            try: out, err = child.communicate(timeout=180)
            except BaseException:
                if child.poll() is None:
                    try: os.killpg(child.pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                try: child.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    if child.poll() is None:
                        try: os.killpg(child.pid, signal.SIGKILL)
                        except ProcessLookupError: pass
                    child.communicate(timeout=3)
                raise
            (logs / (label + '.log')).write_text(out + err); commands.append(dict(label=label, returncode=child.returncode))
            if red:
                if child.returncode != 1 or 'AssertionError [ERR_ASSERTION]' not in err or any(m in err for m in ('SyntaxError', 'ReferenceError', 'TypeError:', 'AddressSanitizer', 'runtime error:')):
                    raise RuntimeError(label + ': expected compiled normal assertion\n' + out + err)
            elif child.returncode: raise RuntimeError(label + '\n' + out + err)
            return out
        sdk, package = source / 'tmp/sdk', source / 'tmp/package'
        def compile(label):
            for p in (sdk, package):
                if p.exists(): shutil.rmtree(p)
            run(label + '-sdk', ['node', tools / 'node_modules/typescript/bin/tsc', '-p', source / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
            run(label + '-package', ['python3', source / 'scripts/build-plugin-sample.py', '--source', source / 'plugins/official/diff', '--tools', tools, '--out', package])
            env.update(GHOSTERM_TEST_PLUGIN_SDK_ENTRY=str(sdk / 'index.js'), GHOSTERM_DIFF_EXECUTOR=str(package / 'executor/index.js'),
                GHOSTERM_DIFF_CLIENT=str(package / 'client/index.js'), GHOSTERM_DIFF_COMPARE=str(package / 'executor/compare.js'))
        def node(label, mode='all', red=False):
            return run(label, ['node', '--experimental-loader', source / 'scripts/plugin-sdk-test-loader.mjs', source / 'scripts/test-plugin-diff.mjs', mode], red)
        def actual(label, scenario, red=False):
            output = run(label, [probe, package, 'executor', scenario])
            try: verify_diff_output(output, scenario)
            except AssertionError as error:
                if red:
                    (logs / (label + '-assertion.log')).write_text('AssertionError: ' + str(error) + '\n'); return
                raise
            if red: raise RuntimeError('Business mutation survived: ' + label)
        compile('baseline'); baseline = node('baseline')
        if 'DIFF CONFORMANCE PASS: 2107 checks / all' not in baseline: raise RuntimeError('conformance discovery drift')
        for scenario in SCENARIOS: actual('baseline-' + scenario, scenario)
        if run('unsupported', [probe, package, 'client', 'empty']).strip() != 'CLIENT_UNSUPPORTED': raise RuntimeError('missing Native diff service must be unsupported')
        codec, algorithm, executor = 'packages/plugin-sdk/portable/diff.ts', 'plugins/official/diff/executor/compare.ts', 'plugins/official/diff/executor/index.ts'
        cases = [
            ('revision', codec, 'uint64(value, true)', 'uint64(value)', 'codec'),
            ('counts', codec, 'value.total - value.added !== value.beforeLines', 'false', 'codec'),
            ('complete-page', codec, 'p.rows.length !== Math.min(r.limit, d.total - r.offset)', 'false', 'codec'),
            ('coordinates', codec, 'item.beforeLine !==', 'false && item.beforeLine !==', 'codec'),
            ('continuity', codec, 'if (previous !== undefined)', 'if (false && previous !== undefined)', 'codec'),
            ('final-newline', codec, 'item.ending === "none"', 'false', 'codec'),
            ('row-limit', codec, 'DIFF_PAGE_ROWS_MAX = 3', 'DIFF_PAGE_ROWS_MAX = 4', 'codec'),
            ('line-ending', algorithm, 'line.ending + "\\0" + line.text', 'line.text', 'core'),
            ('delete-first', algorithm, 'matrix[(i + 1) * width + j]! >= matrix[i * width + j + 1]!', 'matrix[(i + 1) * width + j]! > matrix[i * width + j + 1]!', 'core'),
            ('page-cursor', executor, 'oldOffsets[input.offset]!', 'oldOffsets[0]!', 'core'),
            ('worker-progress', executor, 'if (!live || result) return;', 'if (!live || !result) return;', 'lifecycle'),
            ('dispose-revoke', executor, '!live || context.cancellation.cancelled', 'context.cancellation.cancelled', 'executor'),
        ]
        for label, filename, old, new, mode in cases if args.mutate else []:
            path = source / filename; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError('Ambiguous mutation anchor: ' + label)
            artifact = sdk / 'diff.js' if mode == 'codec' else package / 'executor' / Path(filename).with_suffix('.js').name
            before = sha(artifact)
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_DIFF_MUTATION\n')
                compile(label)
                if sha(artifact) == before: raise RuntimeError('Compiled target unchanged: ' + label)
                if mode == 'core': actual(label + '-red', 'change', True)
                else: node(label + '-red', mode, True)
                run(label + '-control', [build / 'test_config', ROOT / 'core/tests/fixtures'])
            finally: path.write_bytes(original)
            compile(label + '-restored')
            if mode == 'core': actual(label + '-restored', 'change')
            else: node(label + '-restored', mode)
            if sha(path) != inputs[filename]: raise RuntimeError('Source restoration mismatch')
            groups.append(dict(name=label, target='actual Core business outcome' if mode == 'core' else 'compiled Portable value contract'))
            print('PASS ' + label + ': compiled ordinary assertion / same-archive Core configuration / restored / artifact / SHA256', flush=True)
        node('final')
        for scenario in SCENARIOS: actual('final-' + scenario, scenario)
        run('deterministic-package', ['python3', source / 'scripts/build-plugin-sample.py', '--source', source / 'plugins/official/diff', '--tools', tools, '--out', package, '--check'])
        if any(sha(ROOT / n) != v for n, v in inputs.items()): raise RuntimeError('Root input changed')
        receipt.write_text(json.dumps(dict(accepted=True, checks=2107, inputs=inputs, groups=groups, commands=commands,
            core=sha(build / 'libghosterm_core.a'), probe=sha(probe), package=sha(package / 'integrity.json'),
            scope='compiled Portable contracts/adapter and actual public Core package/Session/QuickJS; no Native UI, device, SSH or release'), indent=2) + '\n')
    print('DIFF PASS: 2107 checks / seven actual Core scenarios / unsupported Client / ' + str(len(groups)) + ' mutations', flush=True)


if __name__ == '__main__': main()
