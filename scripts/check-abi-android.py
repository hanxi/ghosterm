#!/usr/bin/env python3
"""Compare JNI Core references and explicitly deferred bindings with public C ABI."""
import argparse
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def symbols(text):
    text = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
    return {name for name in re.findall(r'\b(gt_[a-z][a-z0-9_]*)\s*\(', text) if not name.endswith('_t')}


def check(root):
    headers = list((root / 'core/include').rglob('*.h'))
    sources = list((root / 'android/bridge/src/main/cpp').rglob('*.c'))
    sources += list((root / 'terminal/android/native').rglob('*.c'))
    if not headers or not sources:
        raise ValueError('missing headers or JNI sources')
    declared = set().union(*(symbols(p.read_text()) for p in headers))
    used = set().union(*(symbols(p.read_text()) for p in sources))
    for p in sources:
        if re.search(r'#\s*include\s*["<]ghosterm/', p.read_text()):
            raise ValueError('JNI must include ghosterm.h, not Core subheaders')
    allow = json.loads((root / 'android/bridge/deferred-abi.json').read_text())
    if any(not reason.strip() for reason in allow.values()):
        raise ValueError('deferred binding needs a reason')
    if used - declared:
        raise ValueError('undeclared JNI references: ' + ', '.join(sorted(used - declared)))
    unbound = declared - used
    if set(allow) - unbound:
        raise ValueError('stale deferred bindings: ' + ', '.join(sorted(set(allow) - unbound)))
    if unbound - set(allow):
        raise ValueError('unregistered bindings: ' + ', '.join(sorted(unbound - set(allow))))
    print(f'ANDROID ABI PASS: {len(used)} references; {len(unbound)} explicitly deferred')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    try:
        check(parser.parse_args().root)
    except (ValueError, OSError) as error:
        print('ANDROID ABI FAIL:', error, file=sys.stderr)
        sys.exit(1)
