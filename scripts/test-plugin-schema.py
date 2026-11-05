#!/usr/bin/env python3
"""P0 schema candidate conformance. Explicit --fetch installs pinned dev tools.

All npm cache/modules/temp live in repository tmp/. Offline by default, with
integrity-locked npm ci, lifecycle scripts/audit disabled, no production SDK.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(args, **kwargs):
    command = ['bash', '-c', 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require; shift; gt_timeout "$@"',
               '_', str(ROOT), '180'] + [str(x) for x in args]
    result = subprocess.run(command, capture_output=True, **kwargs)
    if result.returncode:
        raise RuntimeError('schema command failed: ' + repr(result))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args()
    schema = ROOT / 'packages/plugin-sdk/schema/bridge-v1.candidate.json'
    cases = ROOT / 'packages/plugin-sdk/schema/bridge-v1.cases.json'
    fixture = ROOT / 'scripts/fixtures/plugin-schema-tools'
    probe = ROOT / 'scripts/probes/plugin-schema.mjs'
    inputs = [schema, cases, probe, Path(__file__), fixture / 'package.json', fixture / 'package-lock.json']
    hashes = {str(path.relative_to(ROOT)): sha(path) for path in inputs}
    lock = json.loads((fixture / 'package-lock.json').read_text())
    for key, package in lock['packages'].items():
        if key and (not package.get('resolved', '').startswith('https://registry.npmjs.org/') or not package.get('integrity', '').startswith('sha512-')):
            raise RuntimeError('dev dependency not integrity pinned')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='plugin-schema-', dir=ROOT / 'tmp') as temp:
        work = Path(temp)
        for name in ('package.json', 'package-lock.json'):
            shutil.copyfile(fixture / name, work / name)
        npmrc = work / 'empty.npmrc'
        npmrc.touch()
        environment = dict(os.environ, TMPDIR=str(work), NPM_CONFIG_USERCONFIG=str(npmrc),
                           NPM_CONFIG_UPDATE_NOTIFIER='false')
        command = ['npm', 'ci', '--prefix', work, '--ignore-scripts', '--no-audit', '--no-fund',
                   '--cache', ROOT / 'tmp/plugin-schema-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch:
            command.append('--offline')
        run(command, env=environment)
        result = run(['node', probe, schema, cases, work] + (['--mutate'] if args.mutate else []), env=environment)
        if result.stderr:
            raise RuntimeError('unexpected schema diagnostics: ' + repr(result))
        print(result.stdout.decode(), end='', flush=True)
        if any(sha(ROOT / name) != digest for name, digest in hashes.items()):
            raise RuntimeError('schema inputs changed during run')
    print(json.dumps({'candidate': True, 'inputSHA256': hashes,
                      'devTools': {name: package['version'] for name, package in lock['packages'].items() if name}}, sort_keys=True))


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
