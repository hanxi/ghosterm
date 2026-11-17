#!/usr/bin/env python3
"""Reject empty XCTest runs and required methods without a unique passed receipt."""
import argparse
from pathlib import Path
import re
import sys


def verify(text, required):
    completions = {}
    for raw, status in re.findall(r"Test Case '([^'\n]+)' (passed|failed|skipped)\b", text):
        objc = re.fullmatch(r'-\[([\w.]+) (\w+)\]', raw)
        if objc:
            key = objc[1].rsplit('.', 1)[-1] + '.' + objc[2]
        else:
            key = '.'.join(raw.split('.')[-2:])
        completions.setdefault(key, []).append(status)
    if not any('passed' in states for states in completions.values()):
        raise ValueError('no actual passed XCTest method; an empty or all-skipped run is not evidence')
    summaries = re.findall(r'Executed (\d+) tests?, with ([^\n]+)', text)
    if not summaries or int(summaries[-1][0]) == 0 or not re.search(r'\b0 failures?\b', summaries[-1][1]):
        raise ValueError('missing successful nonempty XCTest summary')
    for name in required:
        if completions.get(name) != ['passed']:
            raise ValueError(f'{name}: expected exactly one passed completion, saw {completions.get(name, [])}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', type=Path)
    parser.add_argument('required', nargs='*')
    args = parser.parse_args()
    try:
        verify(args.log.read_text(errors='replace'), args.required)
    except (OSError, ValueError) as error:
        print(f'XCTEST REQUIRED FAIL: {error}', file=sys.stderr)
        return 1
    print(f'XCTEST PASS: nonempty execution; {len(args.required)} required methods confirmed')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
