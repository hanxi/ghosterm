#!/usr/bin/env python3
"""Run the exact native provider cases on an owned no-window Android AVD."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args, serial):
    build = args.build.resolve(strict=True)
    cache = (build / 'CMakeCache.txt').read_text()
    if not re.search(r'ANDROID_ABI:[^=]+=arm64-v8a\n', cache): raise RuntimeError('Explicit arm64 NDK build required')
    artifacts = {name: sha(build / name) for name in ('test_process_stream', 'test_process_stream_boundary', 'libghosterm_core.a')}
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('core/src', 'core/include')
              for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('core/CMakeLists.txt', 'core/tests/test_process_stream.c', 'core/tests/test_process_stream_boundary.c',
                 'scripts/test-plugin-process-stream-android.py', 'scripts/test-plugin-codex-patch-android.py'):
        inputs[name] = sha(ROOT / name)
    env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp')}
    adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]
    with tempfile.TemporaryDirectory(prefix='process-stream-device-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        def run(label, command, seconds=45, allow_error=False):
            p = subprocess.Popen(list(map(str, command)), cwd=ROOT, env=env, stdout=subprocess.PIPE,
                stderr=subprocess.PIPE, text=True, start_new_session=True)
            try: stdout, stderr = p.communicate(timeout=seconds)
            except BaseException:
                try: os.killpg(p.pid, signal.SIGTERM)
                except ProcessLookupError: pass
                try: p.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    try: os.killpg(p.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    p.communicate(timeout=3)
                raise
            output = stdout + stderr; (work / (label + '.log')).write_text(output)
            if p.returncode and not allow_error: raise RuntimeError(label + '\n' + output)
            return (p.returncode, output) if allow_error else output
        def shell(label, *arguments, seconds=45, allow_error=False):
            return run(label, [*adb, 'shell', shlex.join(list(map(str, arguments)))], seconds, allow_error)
        api = shell('api', 'getprop', 'ro.build.version.sdk').strip()
        abi = shell('abi', 'getprop', 'ro.product.cpu.abi').strip()
        page = shell('page', 'getconf', 'PAGE_SIZE').strip()
        if api != args.headless_api or abi != 'arm64-v8a' or page not in ('4096', '16384'):
            raise RuntimeError('Owned device profile mismatch')
        cases = json.loads(run('cases', ['ctest', '--test-dir', build, '--show-only=json-v1']))['tests']
        cases = [case for case in cases if case['name'].startswith('process_stream_')]
        if len(cases) != 25: raise RuntimeError('Expected exact 25 native CTest cases')
        remote = '/data/local/tmp/ghosterm-process-stream-' + secrets.token_hex(12)
        if not re.fullmatch(r'/data/local/tmp/ghosterm-process-stream-[a-f0-9]{24}', remote): raise RuntimeError('Invalid owned test path')
        shell('mkdir', 'mkdir', '-m', '700', remote)
        results = []
        try:
            shell('cwd', 'mkdir', '-m', '700', remote + '/cwd')
            for name in ('test_process_stream', 'test_process_stream_boundary'):
                run('push-' + name, [*adb, 'push', build / name, remote + '/' + name])
                shell('chmod-' + name, 'chmod', '700', remote + '/' + name)
                if shell('sha-' + name, 'sha256sum', remote + '/' + name).split()[0] != artifacts[name]:
                    raise RuntimeError('Device executable SHA256 mismatch')
            for index, case in enumerate(cases):
                command = case['command']
                if Path(command[0]).name == 'test_process_stream':
                    arguments = [remote + '/test_process_stream', command[1], remote + '/cwd']
                    expected = 'PASS process_stream_' + command[1]
                elif Path(command[0]).name == 'test_process_stream_boundary':
                    arguments = [remote + '/test_process_stream_boundary', command[1], remote + '/test_process_stream', remote + '/cwd']
                    expected = 'PASS process_stream_boundary_' + command[1]
                else: raise RuntimeError('Unexpected CTest executable')
                output = shell(str(index), 'env', 'TMPDIR=' + remote + '/cwd', *arguments)
                if expected not in output or any(token in output for token in ('FAIL process_stream', 'AddressSanitizer', 'runtime error:')):
                    raise RuntimeError(case['name'] + '\n' + output)
                results.append(case['name']); print('PASS Android ' + case['name'], flush=True)
        finally:
            # Fault fixtures have an independent alarm; still inspect and close
            # only this unique device path before removing its executables.
            def children():
                def table():
                    output = shell('children', 'ps', '-A', '-o', 'PID,STAT,ARGS')
                    return {int(fields[0]): fields[1:] for line in output.splitlines()[1:]
                            if len(fields := line.split(None, 2)) == 3 and 'test_process_stream' in fields[2]}
                found = {}
                for pid, (state, command) in table().items():
                    if state.startswith('Z'): continue
                    code, executable = shell('exe-' + str(pid), 'readlink', f'/proc/{pid}/exe', allow_error=True)
                    if code:
                        current = table().get(pid)
                        if current is None or current[0].startswith('Z') or current[1] != command: continue
                        raise RuntimeError('Cannot verify live Native test identity: ' + str(pid) + '\n' + executable)
                    # Android ps strips argv[0]'s path. Verify the kernel's
                    # executable identity instead of trusting that basename.
                    executable = executable.strip()
                    if executable in (remote + '/test_process_stream', remote + '/test_process_stream_boundary'):
                        found[pid] = (command, executable)
                return found
            for pid, identity in children().items():
                if children().get(pid) == identity:
                    shell('kill-' + str(pid), 'kill', '-9', pid)
                    print('CLEANUP owned Native process ' + str(pid), flush=True)
            deadline = time.monotonic() + 2
            while children():
                if time.monotonic() >= deadline: raise RuntimeError('Owned device test child remained live')
                time.sleep(.01)
            shell('remove', 'rm', '-rf', remote)
            shell('removed', 'test', '!', '-e', remote)
        if any(sha(ROOT / name) != value for name, value in inputs.items()) or any(sha(build / name) != value for name, value in artifacts.items()):
            raise RuntimeError('Frozen device inputs or actual binaries changed')
        receipt = dict(accepted=True, api=api, abi=abi, pageSize=int(page), noWindow=True, cases=results,
                       inputs=inputs, actualArtifacts=artifacts, deviceCleanupComplete=True,
                       scope='owned API/arm64 native C provider; no JNI/App/Node/model')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print('ANDROID PROCESS STREAM PASS: 25 cases; API ' + api + '; ' + page + ' byte pages', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--headless-api', choices=('29', '36'), required=True)
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): parser.error('receipt must be under project tmp')
    spec = importlib.util.spec_from_file_location('process_stream_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    with helper.headless(args.headless_api) as serial: execute(args, serial)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
