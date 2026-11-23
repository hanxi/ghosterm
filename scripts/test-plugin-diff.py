#!/usr/bin/env python3
"""Check the packaged Lua Diff Executor through the public Core Session."""
import argparse
import json
from pathlib import Path
import subprocess

SCENARIOS = {'change': ('alpha\r\nbeta\nlast', 'alpha\r\ngamma\nlast\n', 5),
    'empty': ('', '', 0), 'equal': ('same\n', 'same\n', 1),
    'unicode': ('é\n😀', 'e\u0301\n😀\n', 4), 'maximum': ('a\n' * 128, 'b\n' * 128, 256),
    'interleaved': ('a\nb\n' * 64, 'b\na\n' * 64, 129),
    'wide': (('😀' * 128 + '\n') * 3, ('😀' * 128 + '\n') * 3, 3)}


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
    assert values[-1]['kind'] == 'failed' and values[-1]['error']['code'] == 'INVALID_INPUT', 'stale revision'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    args = parser.parse_args()
    probe = args.probe.resolve(strict=True)
    package = args.package.resolve(strict=True)
    for scenario in SCENARIOS:
        output = subprocess.check_output([probe, package, "executor", scenario], text=True, timeout=30)
        verify_diff_output(output, scenario)
    output = subprocess.check_output([probe, package, "client", "empty"], text=True, timeout=30)
    assert output.strip() == "CLIENT_UNSUPPORTED", "headless Client must report unsupported"
    print("DIFF PASS: Lua Executor comparison, Unicode, EOLs, pagination and stale revision")


if __name__ == "__main__":
    main()
