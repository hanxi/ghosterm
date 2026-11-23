#!/usr/bin/env python3
"""Check the packaged Lua Task Executor through the public Core Session."""
import argparse
import json
from pathlib import Path
import subprocess

SCENARIOS = ('complete', 'empty', 'one', 'two', 'late-cancel', 'cancel', 'progress', 'duplicate', 'lost-ack')


def prime_count(limit):
    return sum(all(n % divisor for divisor in range(2, int(n ** .5) + 1)) for n in range(2, limit + 1))


def verify_task_output(text, scenario):
    values = [json.loads(line) for line in text.splitlines()]
    assert len(values) >= 3 and values[0]['kind'] == 'success', 'actual descriptor confirmed'
    d = values[0]['value']
    assert set(d) == {'viewId', 'revision', 'title', 'unit', 'cancellable'} and d['cancellable'] is True, 'actual task descriptor'
    assert values[-1]['kind'] == 'failed' and values[-1]['error']['code'] == 'INVALID_INPUT', 'stale revision'
    states, acks, uncertain, rejected = [], [], [], []
    for outcome in values[1:-1]:
        if outcome['kind'] == 'unknown': uncertain.append(outcome); continue
        if outcome['kind'] == 'failed':
            rejected.append(outcome['error']['code'])
            continue
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
            if scenario == 'duplicate':
                assert acks[0] == acks[1], 'duplicate cancellation retains the original acknowledgement'
            assert not uncertain, 'known cancel request remains known'
        if scenario == 'progress': assert int(states[1]['sequence']) > int(states[0]['sequence']), 'worker actually advances between reads'
    assert rejected == (['INVALID_INPUT'] if scenario == 'duplicate' else []), 'different cancellation identity is explicitly refused'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    package = args.package.resolve(strict=True)
    for scenario in SCENARIOS:
        output = subprocess.check_output([probe, package, "executor", scenario], text=True, timeout=30)
        verify_task_output(output, scenario)
    output = subprocess.check_output([probe, package, "client", "empty"], text=True, timeout=30)
    assert output.strip() == "CLIENT_UNSUPPORTED", "headless Client must report unsupported"
    print("TASK PASS: Lua Executor progress, cancellation and exact results")


if __name__ == "__main__":
    main()
