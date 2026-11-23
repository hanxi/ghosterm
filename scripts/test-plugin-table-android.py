#!/usr/bin/env python3
"""Run the same public Core table probe/package on two owned no-window AVDs.

No APK, JNI/Compose test, user device, account, network service or Native table UI.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    for path in (args.probe, args.package, args.receipt):
        if not path.resolve().is_relative_to(ROOT / 'tmp'): parser.error('use owned project tmp inputs/receipt')
    helper = load('table_owned_headless', 'scripts/test-plugin-codex-patch-android.py')
    business = load('table_business_contract', 'scripts/test-plugin-table.py')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('core/src', 'core/include', 'plugins/official/table', 'core/cmake')
        for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('scripts/test-plugin-table-android.py', 'scripts/test-plugin-table.py', 'scripts/probes/plugin-table.c', 'scripts/test-plugin-codex-patch-android.py'):
        inputs[name] = sha(ROOT / name)
    package = {p.relative_to(args.package).as_posix(): sha(p) for p in args.package.rglob('*') if p.is_file()}
    binary = sha(args.probe); records = []
    for api in ('29', '36'):
        with helper.headless(api, scratch=args.receipt.resolve().parent) as serial:
            adb = [str(ROOT / 'build/android-toolchain/sdk/platform-tools/adb'), '-s', serial]
            def run(*command):
                p = subprocess.run([*adb, *map(str, command)], env=os.environ.copy(),
                    text=True, capture_output=True, timeout=120)
                if p.returncode: raise RuntimeError('owned device command failed\n' + p.stdout + p.stderr)
                return p.stdout
            actual_api = run('shell', 'getprop', 'ro.build.version.sdk').strip()
            abi = run('shell', 'getprop', 'ro.product.cpu.abi').strip()
            page = int(run('shell', 'getconf', 'PAGE_SIZE').strip())
            if actual_api != api or abi != 'arm64-v8a': raise RuntimeError('wrong owned device profile')
            remote = '/data/local/tmp/ghosterm-table-' + secrets.token_hex(8)
            run('shell', 'mkdir', '-m', '700', remote)
            try:
                run('push', args.probe, remote + '/probe'); run('push', args.package, remote + '/package')
                run('shell', 'chmod', '700', remote + '/probe')
                if run('shell', 'sha256sum', remote + '/probe').split()[0] != binary: raise RuntimeError('device probe mismatch')
                for name, digest in package.items():
                    if run('shell', 'sha256sum', remote + '/package/' + name).split()[0] != digest: raise RuntimeError('device package mismatch')
                for total in (1000000, 0):
                    output = run('shell', remote + '/probe', remote + '/package', 'executor', total)
                    business.verify_table_output(output, total)
                if run('shell', remote + '/probe', remote + '/package', 'client', 0).strip() != 'CLIENT_UNSUPPORTED': raise RuntimeError('device missing renderer not unsupported')
                records.append(dict(api=api, abi=abi, pageSize=page, noWindow=True, millionRows=True, empty=True, staleRejected=True, clientUnsupported=True))
                print('PASS API' + api + ': same Core probe/package, exact pages, empty/stale, Client unsupported', flush=True)
            finally: run('shell', 'rm', '-rf', remote)
    if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('root inputs changed')
    record = dict(accepted=True, inputs=inputs, devices=records, probe=binary, package=package,
        scope='NDK arm64 / actual public Core Session and Lua 5.5.1 on owned AVDs; no JNI, APK, Native views, SSH, model, true device or release')
    args.receipt.write_text(json.dumps(record, indent=2) + '\n')


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
