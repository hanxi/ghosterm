#!/usr/bin/env python3
"""Independent candidate package metadata checks. No install or activation.

Explicit --fetch permits integrity-pinned developer dependencies. Default is
offline; Node modules/cache/config/temp stay under repository tmp/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
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
        raise RuntimeError('package metadata command failed: ' + repr(result))
    return result


def manifest_example():
    text = (ROOT / 'docs/reference/plan-plugin-bridge.md').read_text().split('### Manifest 示例', 1)[1]
    match = re.search(r'```json\n(.*?)\n```', text, re.S)
    if not match:
        raise RuntimeError('plan manifest example missing')
    return match.group(1).encode()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args()
    fixture = ROOT / 'scripts/fixtures/plugin-package-tools'
    schema = ROOT / 'packages/plugin-sdk/schema'
    probe = ROOT / 'scripts/probes/plugin-package.mjs'
    inputs = [Path(__file__), probe, ROOT / 'packages/plugin-sdk/testkit/package-metadata.mjs',
              fixture / 'package.json', fixture / 'package-lock.json']
    inputs += [schema / name for name in ('bridge-v1.candidate.json', 'manifest-v1.candidate.json', 'integrity-v1.candidate.json')]
    hashes = {str(path.relative_to(ROOT)): sha(path) for path in inputs}
    example = manifest_example()
    lock = json.loads((fixture / 'package-lock.json').read_text())
    for name, package in lock['packages'].items():
        if name and (not package.get('resolved', '').startswith('https://registry.npmjs.org/') or
                     not package.get('integrity', '').startswith('sha512-')):
            raise RuntimeError('package developer tool is not integrity-pinned')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='plugin-package-', dir=ROOT / 'tmp') as temp:
        work = Path(temp)
        for name in ('package.json', 'package-lock.json'):
            shutil.copyfile(fixture / name, work / name)
        npmrc = work / 'empty.npmrc'
        npmrc.touch()
        example_file = work / 'plan-manifest.json'
        example_file.write_bytes(example)
        environment = dict(os.environ, TMPDIR=str(work), NPM_CONFIG_USERCONFIG=str(npmrc),
                           NPM_CONFIG_UPDATE_NOTIFIER='false')
        command = ['npm', 'ci', '--prefix', work, '--ignore-scripts', '--no-audit', '--no-fund',
                   '--cache', ROOT / 'tmp/plugin-schema-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch:
            command.append('--offline')
        run(command, env=environment)
        result = run(['node', probe, ROOT, work, example_file] + (['--mutate'] if args.mutate else []), env=environment)
        if result.stderr:
            raise RuntimeError('unexpected metadata diagnostics: ' + repr(result))
        print(result.stdout.decode(), end='', flush=True)
        if any(sha(ROOT / name) != digest for name, digest in hashes.items()):
            raise RuntimeError('metadata inputs changed during run')
        if example != manifest_example():
            raise RuntimeError('plan manifest example changed during run')
    print(json.dumps({'candidate': True, 'inputSHA256': hashes,
                      'planManifestExampleSHA256': hashlib.sha256(example).hexdigest(),
                      'devTools': {name: value['version'] for name, value in lock['packages'].items() if name}}, sort_keys=True))


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
