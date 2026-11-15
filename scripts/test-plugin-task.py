#!/usr/bin/env python3
"""Task value/worker conformance, actual public Core outcomes and mutations."""
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
SCENARIOS = ('complete', 'empty', 'one', 'two', 'late-cancel', 'cancel', 'progress', 'duplicate', 'lost-ack')


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def prime_count(limit):
    return sum(all(n % divisor for divisor in range(2, int(n ** .5) + 1)) for n in range(2, limit + 1))


def verify_task_output(text, scenario):
    values = [json.loads(line) for line in text.splitlines()]
    assert len(values) >= 3 and values[0]['kind'] == 'success', 'actual descriptor confirmed'
    d = values[0]['value']
    assert set(d) == {'viewId', 'revision', 'title', 'unit', 'cancellable'} and d['cancellable'] is True, 'actual task descriptor'
    assert values[-1]['kind'] == 'unknown', 'stale revision must not succeed'
    states, acks, uncertain = [], [], []
    for outcome in values[1:-1]:
        if outcome['kind'] == 'unknown': uncertain.append(outcome); continue
        assert outcome['kind'] == 'success', 'actual confirmed observation'
        p = outcome['value']
        assert p['viewId'] == d['viewId'] and p['revision'] == d['revision'], 'same active task scope'
        assert len(json.dumps(p, ensure_ascii=False, separators=(',', ':')).encode()) <= 2048, 'bounded actual wire value'
        if 'phase' not in p:
            assert set(p) == {'viewId', 'revision', 'requestId', 'kind'} and p['requestId'] == 'cancel-1', 'cancel acknowledgement exact identity'
            assert p['kind'] in ('requested', 'alreadyFinished'), 'ack is not an invented stopped result'
            acks.append(p); continue
        assert set(p) == {'viewId', 'revision', 'sequence', 'phase', 'completed', 'total', 'message', 'result'}, 'closed task snapshot'
        assert p['sequence'].isdigit() and p['sequence'][0] != '0' and int(p['sequence']) <= 18446744073709551615, 'canonical exact sequence'
        assert p['completed'] == str(int(p['completed'])) and p['total']['kind'] == 'known', 'sample exact counters'
        assert int(p['completed']) <= int(p['total']['units']), 'progress never exceeds total'
        expected = {'succeeded': 'success', 'failed': 'failed', 'cancelled': 'cancelled', 'unknown': 'unknown'}.get(p['phase'], 'none')
        assert p['result']['kind'] == expected, 'result matches observed phase'
        if states:
            old = states[-1]
            assert int(p['sequence']) >= int(old['sequence']) and int(p['completed']) >= int(old['completed']), 'monotonic observations'
            assert p['total'] == old['total'], 'stable known total'
            if p['sequence'] == old['sequence'] or old['phase'] in ('succeeded', 'failed', 'cancelled'): assert p == old, 'immutable sequence/terminal result'
        states.append(p)
    assert states, 'actual task state read'
    limit = dict(complete=10000, empty=0, one=1, two=2, **{'late-cancel': 100}).get(scenario, 1000000)
    assert states[-1]['total'] == {'kind': 'known', 'units': str(max(0, limit - 1))}, 'real candidate total'
    if scenario in ('complete', 'empty', 'one', 'two', 'late-cancel'):
        assert states[-1]['phase'] == 'succeeded' and states[-1]['completed'] == str(max(0, limit - 1)), 'complete computation confirmed'
        assert states[-1]['result'] == {'kind': 'success', 'summary': f'{prime_count(limit)} primes up to {limit}'}, 'independent prime count'
        assert acks == ([dict(viewId=d['viewId'], revision=d['revision'], requestId='cancel-1', kind='alreadyFinished')] if scenario == 'late-cancel' else []), 'late cancel preserves success'
        assert not uncertain, 'no fabricated uncertain task success'
    else:
        assert states[0]['phase'] == 'running' and states[-1]['phase'] == 'cancelled', 'genuine running task cooperatively stops'
        assert states[-1]['result'] == {'kind': 'cancelled', 'effect': 'none'} and int(states[-1]['completed']) < limit - 1, 'stopped before complete with no external effect'
        if scenario == 'lost-ack':
            assert not acks and len(uncertain) == 1, 'uncertain cancellation not denied or acknowledged'
        else:
            assert len(acks) == (2 if scenario == 'duplicate' else 1) and all(p['kind'] == 'requested' for p in acks), 'cancel request acknowledgement'
            if scenario == 'duplicate': assert acks[0] == acks[1] and len(uncertain) == 1, 'stable duplicate ack and different identity refused'
            else: assert not uncertain, 'known cancel request remains known'
        if scenario == 'progress': assert int(states[1]['sequence']) > int(states[0]['sequence']), 'worker actually advances between reads'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--tools', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    build, probe, tools, receipt = (p.resolve() for p in (args.build, args.probe, args.tools, args.receipt))
    if any(not p.is_relative_to(ROOT / 'tmp') for p in (build, probe, tools, receipt)): parser.error('use owned project tmp paths')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('core/src', 'core/include', 'packages/plugin-sdk', 'plugins/official/task') for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('scripts/test-plugin-task.py', 'scripts/test-plugin-task.mjs', 'scripts/probes/plugin-task.c', 'scripts/build-plugin-sample.py', 'scripts/plugin-sdk-test-loader.mjs', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_TASK_MUTATION' in (ROOT / n).read_bytes() for n in inputs if n.endswith('.ts')): raise RuntimeError('leftover task mutation')
    logs = receipt.parent / (receipt.stem + '-logs'); logs.mkdir(parents=True, exist_ok=False)
    commands, groups = [], []
    with tempfile.TemporaryDirectory(prefix='task-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; (source / 'tmp').mkdir(parents=True)
        for folder in ('packages/plugin-sdk', 'plugins/official/task', 'scripts'):
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('__pycache__'))
        shutil.copyfile(ROOT / 'LICENSE', source / 'LICENSE')
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
        def run(label, command, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-task; shift; exec "$gt_timeout_bin" "$@"'
            child = subprocess.Popen(list(map(str, ['bash', '-c', shell, 'plugin-task', ROOT, 180, *command])), cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
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
                if child.returncode != 1 or 'AssertionError [ERR_ASSERTION]' not in err or any(x in err for x in ('SyntaxError', 'ReferenceError', 'TypeError:', 'AddressSanitizer', 'runtime error:')):
                    raise RuntimeError(label + ': expected compiled normal assertion\n' + out + err)
            elif child.returncode: raise RuntimeError(label + '\n' + out + err)
            return out
        sdk, package = source / 'tmp/sdk', source / 'tmp/package'
        def compile(label):
            for p in (sdk, package):
                if p.exists(): shutil.rmtree(p)
            run(label + '-sdk', ['node', tools / 'node_modules/typescript/bin/tsc', '-p', source / 'packages/plugin-sdk/tsconfig.json', '--outDir', sdk])
            run(label + '-package', ['python3', source / 'scripts/build-plugin-sample.py', '--source', source / 'plugins/official/task', '--tools', tools, '--out', package])
            env.update(GHOSTERM_TEST_PLUGIN_SDK_ENTRY=str(sdk / 'index.js'), GHOSTERM_TASK_EXECUTOR=str(package / 'executor/index.js'),
                GHOSTERM_TASK_CLIENT=str(package / 'client/index.js'), GHOSTERM_TASK_ENGINE=str(package / 'executor/primes.js'))
        def node(label, mode='all', red=False): return run(label, ['node', '--experimental-loader', source / 'scripts/plugin-sdk-test-loader.mjs', source / 'scripts/test-plugin-task.mjs', mode], red)
        def actual(label, scenario, red=False):
            output = run(label, [probe, package, 'executor', scenario])
            try: verify_task_output(output, scenario)
            except AssertionError as error:
                if red:
                    (logs / (label + '-assertion.log')).write_text('AssertionError: ' + str(error) + '\n'); return
                raise
            if red: raise RuntimeError('Business mutation survived: ' + label)
        compile('baseline'); baseline = node('baseline')
        if 'TASK CONFORMANCE PASS: 7642 checks / all' not in baseline: raise RuntimeError('task discovery drift')
        for scenario in SCENARIOS: actual('baseline-' + scenario, scenario)
        if run('unsupported', [probe, package, 'client', 'empty']).strip() != 'CLIENT_UNSUPPORTED': raise RuntimeError('missing Native task service must be unsupported')
        codec, engine, executor = 'packages/plugin-sdk/portable/task.ts', 'plugins/official/task/executor/primes.ts', 'plugins/official/task/executor/index.ts'
        cases = [
            ('revision', codec, 'uint64(value, true)', 'uint64(value)', 'codec'),
            ('result-phase', codec, 'value.result.kind !== expected', 'value.result.kind !== expected && false', 'codec'),
            ('complete-progress', codec, 'value.completed !== value.total.units', 'false', 'codec'),
            ('sequence', codec, 'order < 0', 'false', 'codec'),
            ('progress', codec, 'compareUint64(p.completed, prior.completed) < 0', 'false', 'codec'),
            ('known-total', codec, 'prior.total.kind === "known" && (p.total.kind !== "known" || p.total.units !== prior.total.units)', 'false', 'codec'),
            ('terminal', codec, 'taskIsTerminal(prior.phase) &&', 'false && taskIsTerminal(prior.phase) &&', 'codec'),
            ('ack-identity', codec, 'a.requestId !== r.requestId', 'false', 'codec'),
            ('body-bytes', codec, '> TASK_BODY_BYTES_MAX', '> 3072', 'codec'),
            ('bounded-worker', engine, 'let remaining = 512', 'let remaining = 513', 'algorithm'),
            ('sieve-result', engine, 'this.composite![this.multiple] = 1', 'this.composite![this.multiple] = 0', 'core-complete'),
            ('cancel-worker', engine, 'if (this.phase === "cancelling")', 'if (false && this.phase === "cancelling")', 'core-cancel'),
            ('cancel-dedup', executor, 'return cancellation;\n    }', 'return { ...cancellation, kind: "alreadyFinished" as const };\n    }', 'core-duplicate'),
            ('cancel-request-identity', executor, 'if (input.requestId !== cancellation.requestId)', 'if (false)', 'core-duplicate'),
            ('disposed-read', executor, '!live || context.cancellation.cancelled', 'context.cancellation.cancelled', 'lifecycle'),
        ]
        for label, filename, old, new, mode in cases if args.mutate else []:
            path = source / filename; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError('Ambiguous mutation anchor: ' + label)
            artifact = sdk / 'task.js' if mode == 'codec' else package / 'executor' / Path(filename).with_suffix('.js').name
            before = sha(artifact)
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_TASK_MUTATION\n'); compile(label)
                if sha(artifact) == before: raise RuntimeError('Compiled artifact unchanged: ' + label)
                if mode.startswith('core-'): actual(label + '-red', mode[5:], True)
                else: node(label + '-red', mode, True)
                run(label + '-control', [build / 'test_config', ROOT / 'core/tests/fixtures'])
            finally: path.write_bytes(original)
            compile(label + '-restored')
            if mode.startswith('core-'): actual(label + '-restored', mode[5:])
            else: node(label + '-restored', mode)
            if sha(path) != inputs[filename]: raise RuntimeError('Source restoration mismatch')
            groups.append(dict(name=label, target='actual public Core business' if mode.startswith('core-') else 'compiled value/worker contract'))
            print('PASS ' + label + ': compiled assertion / same-archive Core config / restore / artifact / SHA256', flush=True)
        node('final')
        for scenario in SCENARIOS: actual('final-' + scenario, scenario)
        if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('Root input changed')
        receipt.write_text(json.dumps(dict(accepted=True, inputs=inputs, commands=commands, groups=groups,
            probe=sha(probe), core=sha(build / 'libghosterm_core.a'),
            scope='compiled Portable task values, real sieve, simulated Session adapter; actual public Core/QuickJS; controlled Session clock for lost-ack; no Native task UI, SSH, device or release'), indent=2) + '\n')
        print('TASK PASS: 7642 checks / 9 actual Core scenarios / ' + str(len(groups)) + ' mutations', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
