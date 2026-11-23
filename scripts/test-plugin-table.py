#!/usr/bin/env python3
"""Check the packaged Lua Table Executor through the public Core Session."""
import argparse
import json
from pathlib import Path
import subprocess

def verify_table_output(text, total):
    values = [json.loads(line) for line in text.splitlines()]
    offsets = [0] if total == 0 else [0, 999984, 999999, 1000000]
    assert len(values) == len(offsets) + 2, 'exact actual outcome count'
    descriptor = values[0]
    assert descriptor['kind'] == 'success' and descriptor['value']['total'] == total, 'descriptor'
    descriptor = descriptor['value']
    assert descriptor['pageSize'] == 16, 'page size'
    for offset, outcome in zip(offsets, values[1:-1], strict=True):
        assert outcome['kind'] == 'success', 'confirmed page'
        page = outcome['value']
        assert page['viewId'] == descriptor['viewId'] and page['revision'] == descriptor['revision'], 'same instance/revision'
        assert page['offset'] == offset and page['total'] == total, 'page origin and total'
        assert len(page['rows']) == min(16, total - offset), 'bounded exact page'
        for index, row in enumerate(page['rows']):
            n = page['offset'] + index + 1
            assert row['id'] == str(n) and row['cells'] == list(map(str, (n, n*n, n*n*n))), 'exact computed row'
    assert values[-1]['kind'] == 'failed' and values[-1]['error']['code'] == 'INVALID_INPUT', 'stale revision'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    args = parser.parse_args()

    def run(role, total):
        return subprocess.check_output(
            [args.probe.resolve(strict=True), args.package.resolve(strict=True), role, str(total)],
            text=True,
            timeout=30,
        )

    for total in (1_000_000, 0):
        verify_table_output(run("executor", total), total)
    assert run("client", 0).strip() == "CLIENT_UNSUPPORTED", "headless Client must report unsupported"
    print("TABLE PASS: Lua Executor paging, empty table, stale revision and unsupported Client")


if __name__ == "__main__":
    main()
