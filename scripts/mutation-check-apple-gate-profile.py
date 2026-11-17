#!/usr/bin/env python3
"""Check required-gate fault assertions using isolated shell fixtures, not an SSH client."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    ('missing-required', 'apple-gate-profile.sh', 'if [ "$APPLE_GATE_MODE" = required ]; then\n    echo "Apple gate required:',
     'if false; then\n    echo "Apple gate required:', 'test_required_missing_ssh_is_failure'),
    ('missing-swift', 'apple-app-gate.sh', 'apple_gate_missing "未找到 swift 工具链"',
     'APPLE_GATE_PARTIAL=1', 'test_required_missing_swift_is_failure'),
    ('required-receipt', 'apple-gate-profile.sh', '  if [ "$APPLE_GATE_MODE" = required ]; then\n    python3',
     '  if false; then\n    python3', 'test_required_missing_ssh_receipt_is_failure'),
    ('empty-tests', 'check-required-xctests.py', 'def verify(text, required):',
     'def verify(text, required):\n    return', 'test_empty_swift_execution_cannot_pass'),
    ('partial-label', 'apple-gate-profile.sh', 'echo "$1 PARTIAL (mode=$APPLE_GATE_MODE; skipped scope is unverified)"',
     'echo "$1 PASS (mode=$APPLE_GATE_MODE)"', 'test_optional_missing_swift_is_partial'),
    ('core-no-e2e', 'apple-gate.sh', '[ "$E2E" = 0 ] && [ "$APPLE_GATE_MODE" = required ]',
     '[ "$E2E" = 0 ] && false', 'test_core_required_forbids_no_e2e'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', required=True, type=Path)
    args = parser.parse_args()
    checkout = args.checkout.resolve()
    registered = subprocess.check_output(['git', '-C', ROOT, 'worktree', 'list', '--porcelain'], text=True)
    if ROOT / 'tmp' not in checkout.parents or f'worktree {checkout}\n' not in registered:
        parser.error('requires registered worktree beneath project tmp/')
    names = {case[1] for case in CASES}
    originals = {name: (checkout / 'scripts' / name).read_bytes() for name in names}
    main_bytes = {name: (ROOT / 'scripts' / name).read_bytes() for name in names}
    test = ROOT / 'scripts/test-apple-app-gate.py'
    tests = test.read_bytes()
    work = Path(tempfile.mkdtemp(prefix='apple-profile-mutations-', dir=checkout / 'tmp'))
    records = []
    environment = dict(os.environ, TMPDIR=str(checkout / 'tmp'))
    command = ['python3', str(test), '--gate', str(checkout / 'scripts/apple-app-gate.sh'),
               '--fixture-scripts', str(checkout / 'scripts'), '--shell', '/bin/bash']

    def run(label, selected=None, red=False):
        result = subprocess.run(command + ([f'GateExitTests.{selected}'] if selected else []),
                                cwd=ROOT, env=environment, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (work / f'{label}.log').write_text(result.stdout)
        if red:
            assert result.returncode == 1 and 'Ran 1 test' in result.stdout and 'FAILED (failures=1)' in result.stdout, (label, result.stdout)
            assert 'AssertionError' in result.stdout and not re.search(r'SyntaxError|ERROR:|Traceback.*Syntax', result.stdout), label
        else:
            assert result.returncode == 0 and re.search(r'Ran [1-9][0-9]* tests?', result.stdout), (label, result.stdout)
        records.append({'case': label, 'returncode': result.returncode})

    try:
        run('baseline')
        for name, file, before, after, target in CASES:
            source = checkout / 'scripts' / file
            text = originals[file].decode()
            assert text.count(before) == 1, (name, text.count(before))
            source.write_text(text.replace(before, after, 1))
            if file.endswith('.sh'):
                subprocess.run(['/bin/bash', '-n', source], check=True)
            run(name + '-red', target, red=True)
            run(name + '-control', 'test_abi_failure_stops_before_build')
            source.write_bytes(originals[file])
            run(name + '-restored', target)
        run('final')
    finally:
        for name, original in originals.items():
            (checkout / 'scripts' / name).write_bytes(original)
        assert test.read_bytes() == tests
        assert all((ROOT / 'scripts' / name).read_bytes() == value for name, value in main_bytes.items())
        (work / 'receipt.json').write_text(json.dumps({'source_sha256': {name: hashlib.sha256(value).hexdigest() for name, value in originals.items()},
            'tests_sha256': hashlib.sha256(tests).hexdigest(), 'records': records,
            'scope': 'isolated shell dependency stubs; no native compile, UI or authentication'}, indent=2) + '\n')
    print(f'PASS: {work / "receipt.json"}')


if __name__ == '__main__':
    main()
