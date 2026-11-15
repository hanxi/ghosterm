#!/usr/bin/env python3
"""Actual CLI Client-service rejection and two isolated compiled mutations."""
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


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--service', choices=('ui.table', 'ui.diff', 'ui.task', 'ui.views'), default='ui.table')
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path, required=True)
    args = parser.parse_args()
    service = args.service
    contribution = service.removeprefix('ui.')
    build, package = args.build.resolve(strict=True), args.package.resolve(strict=True)
    if any(not p.is_relative_to(ROOT / 'tmp') for p in (build, package, args.receipt.resolve())):
        parser.error('use owned project tmp builds, package and receipt')
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('cli/gterm', 'core/include') for p in (ROOT / folder).rglob('*') if p.is_file()}
    inputs['scripts/test-gterm-client-service.py'] = sha(Path(__file__))
    if b'GT_CLIENT_SERVICE_MUTATION' in (ROOT / 'cli/gterm/stdio.c').read_bytes(): raise RuntimeError('leftover mutation marker')
    with tempfile.TemporaryDirectory(prefix='client-service-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'cli'; shutil.copytree(ROOT / 'cli/gterm', source)
        env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp'), 'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer',
            'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        def run(command):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require client-service; shift; exec "$gt_timeout_bin" "$@"', 'client-service', ROOT, 30, *command]
            p = subprocess.Popen(list(map(str, bounded)), cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
            try: out, err = p.communicate(timeout=30)
            except BaseException:
                if p.poll() is None:
                    try: os.killpg(p.pid, signal.SIGTERM)
                    except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    if p.poll() is None:
                        try: os.killpg(p.pid, signal.SIGKILL)
                        except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            if any(s in err for s in (b'AddressSanitizer', b'runtime error:')): raise RuntimeError('sanitizer failure')
            return p.returncode, out, err
        core = build / 'core/libghosterm_core.a'; quickjs = build / 'core/libghosterm_quickjs.a'
        compiler = ['xcrun', '--sdk', 'macosx', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-mmacosx-version-min=13.0', '-I' + str(ROOT / 'core/include')]
        libraries = [core, quickjs, '-L' + str(ROOT / 'build/host-native/lib'), '-lssh2', '-lm']
        binary = work / 'gterm'; control = work / 'config'
        def compile():
            status, out, err = run([*compiler, '-DGTERM_SDK_VERSION="1.0.0"', '-DGTERM_PROTOCOL="1"',
                source / 'main.c', source / 'stdio.c', source / 'ipc.c', *libraries, '-o', binary])
            if status: raise RuntimeError('compile failed\n' + err.decode(errors='replace'))
        status, _, err = run([*compiler, '-DGT_TEST_TMP_DIR="' + str(ROOT / 'tmp') + '"',
            ROOT / 'core/tests/test_config.c', *libraries, '-o', control])
        if status: raise RuntimeError('control compile failed\n' + err.decode(errors='replace'))
        no_rpc = work / 'no-rpc'; shutil.copytree(package, no_rpc)
        manifest = json.loads((no_rpc / 'plugin.json').read_text()); manifest['executor']['variants'][0]['requires'] = []
        command = ' '.join(manifest['commands'][0]['path'])
        manifest['permissions'] = [p for p in manifest['permissions'] if p['side'] != 'executor']
        (no_rpc / 'plugin.json').write_text(json.dumps(manifest, separators=(',', ':')))
        members = [dict(path=str(p.relative_to(no_rpc)), bytes=p.stat().st_size, sha256=sha(p))
            for p in sorted(no_rpc.rglob('*')) if p.is_file() and p.name != 'integrity.json']
        (no_rpc / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
        def check(case):
            selected = no_rpc if case == 'rpc' else package
            options = ['--client-capability', service]
            if case == 'unknown': options = ['--client-capability', 'ui.notImplemented']
            elif case == 'duplicate': options *= 2
            digest = sha(selected / 'integrity.json')
            status, _, error = run([binary, 'relay', '--stdio', '--trust-development-digest', digest,
                '--client-digest', digest, *options, '--', selected, command])
            expected = ('Client ' + contribution + ' requires declared RPC and a capability slot').encode() if case == 'rpc' else b'unknown or duplicate Client service requirement'
            assert status == (4 if case == 'rpc' else 2) and expected in error, 'Client service rejection: ' + case
        compile()
        for case in ('unknown', 'duplicate', 'rpc'): check(case)
        groups = []
        path = source / 'stdio.c'; original = path.read_bytes()
        cases = [('allowed-service', 'strcmp(argv[at + 1], "' + service + '")', '0', 'unknown'),
                 ('declared-rpc', 'if (client_' + contribution + ') {\n        if (!rpc || config.required_count >= 16',
                  'if (client_' + contribution + ') {\n        if (false || config.required_count >= 16', 'rpc')]
        for name, old, new, case in cases if args.mutate else []:
            if original.count(old.encode()) != 1: raise RuntimeError('mutation anchor: ' + name)
            before = sha(binary)
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n/* GT_CLIENT_SERVICE_MUTATION */\n'); compile()
                if sha(binary) == before: raise RuntimeError('CLI binary unchanged')
                try: check(case)
                except AssertionError: pass
                else: raise RuntimeError('Client service regression survived: ' + name)
                if run([control, ROOT / 'core/tests/fixtures'])[0]: raise RuntimeError('same archive config control failed')
            finally: path.write_bytes(original)
            compile(); check(case)
            if path.read_bytes() != original: raise RuntimeError('source restoration mismatch')
            groups.append(name); print('PASS ' + name + ': actual compiled CLI assertion / same archive config / restore / SHA256', flush=True)
        if any(sha(ROOT / p) != h for p, h in inputs.items()): raise RuntimeError('root source changed')
        args.receipt.write_text(json.dumps(dict(accepted=True, service=service, inputs=inputs, mutations=groups,
            core=sha(core), binary=sha(binary), scope='macOS O1 ASan/UBSan CLI guards; linked Core is the explicit enabled archive, no LSan; no Native UI/device'), indent=2) + '\n')
        print('CLIENT SERVICE PASS: three actual rejection cases / ' + str(len(groups)) + ' mutations')


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
