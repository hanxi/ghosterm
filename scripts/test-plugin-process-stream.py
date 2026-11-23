#!/usr/bin/env python3
"""Isolated native process stream: optimized ASan/UBSan and compiled mutants.

Owned test executables and disposable cwd only. No App, model, installed runtime
or user endpoint. A target red is one normal parent assertion, never a compiler
failure, crash, sanitizer report or external timeout.
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
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = 'core/src/extension/process-stream.c'
MARK = 'GT_PROCESS_STREAM_MUTATION'


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--group', action='append')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'):
        parser.error('receipt must be under project tmp')
    original = (ROOT / SOURCE).read_bytes()
    if MARK.encode() in original: raise RuntimeError('Residual process stream mutation')
    cases = [
        ('approval', 'if (!config->execution_authorized)', 'if (false)', 'denied'),
        ('stdout-limit', 'if (s->info.stdout_bytes > s->stdout_limit)', 'if (false)', 'stdout-limit'),
        ('stderr-limit', 'if (s->info.stderr_bytes > s->stderr_limit)', 'if (false)', 'stderr-limit'),
        ('deadline', 'else if (now - s->began >= s->lifetime)', 'else if (false)', 'deadline'),
        ('wait-eof', 'return io(GT_BRIDGE_WAIT, 0, GT_OK);', 'return io(GT_BRIDGE_EOF, 0, GT_OK);', 'wait'),
        ('read-bytes', 'return io(GT_BRIDGE_DATA, r.count, GT_OK);',
         'if (!writing && s->info.stdout_bytes > 1) ((uint8_t *)bytes)[0] ^= 1; return io(GT_BRIDGE_DATA, r.count, GT_OK);', 'tail'),
        ('stdout-observation', 's->info.stdout_bytes += count;', '(void)count; s->info.stdout_bytes += 0;', 'tail'),
        ('diagnostic', 'memcpy(s->info.diagnostic + s->info.diagnostic_size, bytes, keep);',
         'memset(s->info.diagnostic + s->info.diagnostic_size, 63, keep);', 'stderr'),
        ('wake', 'write(s->wake[1], &byte, 1)', '((void)s, (ssize_t)sizeof(byte))', 'wake'),
        ('kill', 'signal_process(s, SIGKILL)', 'signal_process(s, SIGTERM)', 'ignore'),
        ('group-fence', 'if (!s->info.group_fenced) {', 'if (false) {', 'orphan'),
        ('premature-close', 'return GT_BRIDGE_DRAINING;', 'return GT_BRIDGE_RELEASED;', 'ignore'),
        ('group-unknown', 's->info.group_signal_error = error;\n        } else {',
         's->info.group_signal_error = 0;\n        } else {', 'boundary:dead-group-unknown'),
        ('io-eof', 'if (s->info.error) {\n            return io(GT_BRIDGE_ERROR, 0, s->info.error);',
         'if (s->info.error) {\n            return io(GT_BRIDGE_EOF, 0, GT_OK);', 'boundary:stderr-error'),
        ('first-error', 'if (!s->info.error) {\n                s->info.reason = GT_PROCESS_STREAM_DEADLINE;',
         'if (true) {\n                s->info.reason = GT_PROCESS_STREAM_DEADLINE;', 'boundary:first-error'),
    ]
    for label, old, _, _ in cases:
        if original.decode().count(old) != 1: raise RuntimeError('Ambiguous mutation: ' + label)
    if args.group:
        if set(args.group) - {case[0] for case in cases}: parser.error('unknown mutation group')
        if not args.mutate: parser.error('--group requires --mutate')
        cases = [case for case in cases if case[0] in args.group]
    inputs = {str(p.relative_to(ROOT)): sha(p) for folder in ('core/src', 'core/include')
              for p in (ROOT / folder).rglob('*') if p.is_file()}
    for name in ('core/CMakeLists.txt', 'core/tests/test_process_stream.c', 'core/tests/test_process_stream_boundary.c',
                 'scripts/test-plugin-process-stream.py'):
        inputs[name] = sha(ROOT / name)
    results = []
    with tempfile.TemporaryDirectory(prefix='process-stream-check-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; logs = work / 'logs'; logs.mkdir()
        shutil.copytree(ROOT / 'core', source / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        for folder in ('scripts', 'packages', 'vendor', 'patches', 'build'):
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        build = work / 'build'
        env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp'),
               'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        def owned_children():
            table = subprocess.check_output(['ps', '-axo', 'pid=,lstart=,command='], text=True, timeout=5)
            return [(int(line.split(None, 1)[0]), line.split(None, 1)[1]) for line in table.splitlines()
                    if str(build / 'test_process_stream') + ' --child ' in line]
        def close_children():
            # A deliberately broken fence/close can orphan its test child.
            # Signal only this private executable plus its observed start identity.
            for pid, identity in owned_children():
                current = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=,command='], capture_output=True, text=True, timeout=5)
                if current.stdout.strip() == identity:
                    try: os.kill(pid, signal.SIGKILL)
                    except ProcessLookupError: pass
            end = time.monotonic() + 2
            while owned_children():
                if time.monotonic() >= end: raise RuntimeError('Owned test child did not exit')
                time.sleep(.01)
        def run(label, command, red=False, seconds=120):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require process-stream; shift; exec "$gt_timeout_bin" --foreground "$@"'
            process = subprocess.Popen(['bash', '-c', shell, 'process-stream', str(ROOT), str(seconds), *map(str, command)],
                cwd=ROOT, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, start_new_session=True)
            try: stdout, stderr = process.communicate(timeout=seconds + 10)
            except BaseException:
                try: os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError: pass
                try: process.communicate(timeout=8)
                except subprocess.TimeoutExpired:
                    try: os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError: pass
                    process.communicate(timeout=3)
                raise
            output = stdout + stderr; (logs / (label + '.log')).write_text(output)
            if red:
                failures = re.findall(r'^FAIL process_stream(?:_boundary)? ', output, re.M)
                crash_signals = {int(getattr(signal, name)) for name in
                    ('SIGILL', 'SIGABRT', 'SIGBUS', 'SIGFPE', 'SIGSEGV', 'SIGTRAP', 'SIGSYS', 'SIGEMT')
                    if hasattr(signal, name)}
                child_crash = any(int(number) in crash_signals for number in re.findall(r'\bsignal=(\d+)\b', output))
                if process.returncode != 1 or len(failures) != 1 or any(token in output for token in
                    ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:', 'Traceback')) or child_crash:
                    raise RuntimeError(label + ': expected one normal assertion\n' + output)
            elif process.returncode: raise RuntimeError(label + '\n' + output)
            close_children()
            return output
        def artifact_hashes():
            return {name: sha(build / name) for name in ('libghosterm_core.a', 'test_process_stream', 'test_process_stream_boundary',
                    'CMakeFiles/ghosterm_core.dir/src/extension/process-stream.c.o',
                    'CMakeFiles/test_process_stream_boundary.dir/tests/test_process_stream_boundary.c.o')}
        def compile(label):
            objects = ['CMakeFiles/ghosterm_core.dir/src/extension/process-stream.c.o',
                       'CMakeFiles/test_process_stream.dir/tests/test_process_stream.c.o',
                       'CMakeFiles/test_process_stream_boundary.dir/tests/test_process_stream_boundary.c.o']
            for name in [*objects, 'libghosterm_core.a', 'test_process_stream', 'test_process_stream_boundary', 'test_config']:
                (build / name).unlink(missing_ok=True)
            output = run(label, ['cmake', '--build', build, '--target', 'test_process_stream', 'test_process_stream_boundary', 'test_config', '-j2'])
            if any('Building C object ' + name not in output for name in objects): raise RuntimeError('Missing actual compile: ' + label)
            return artifact_hashes()
        def target(label, case, red=False):
            command = [build / 'test_process_stream_boundary', case.split(':')[1], build / 'test_process_stream', source / 'tmp'] \
                if case.startswith('boundary:') else [build / 'test_process_stream', case, source / 'tmp']
            return run(label, command, red, 30)
        try:
            run('configure', ['cmake', '-S', source / 'core', '-B', build, '-DGHOSTERM_WITH_LIBSSH2=OFF',
                '-DGHOSTERM_WITH_PLUGIN_LUA=OFF', '-DGHOSTERM_BUILD_TESTS=ON', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG',
                '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
            baseline = compile('baseline-build')
            run('baseline', ['ctest', '--test-dir', build, '-R', '^process_stream_', '--output-on-failure'])
            run('baseline-config', [build / 'test_config', ROOT / 'core/tests/fixtures'])
            if args.mutate:
                for label, old, new, case in cases:
                    path = source / SOURCE
                    try:
                        path.write_text(original.decode().replace(old, new, 1) + '\n/* ' + MARK + ' */\n')
                        changed = compile(label + '-build')
                        actual = 'test_process_stream_boundary' if case.startswith('boundary:') else 'test_process_stream'
                        object_name = 'CMakeFiles/test_process_stream_boundary.dir/tests/test_process_stream_boundary.c.o' if case.startswith('boundary:') else 'CMakeFiles/ghosterm_core.dir/src/extension/process-stream.c.o'
                        if changed[actual] == baseline[actual] or changed[object_name] == baseline[object_name]:
                            raise RuntimeError('Actual target object/binary did not change: ' + label)
                        target(label + '-red', case, True)
                        run(label + '-control', [build / 'test_config', ROOT / 'core/tests/fixtures'])
                    finally:
                        path.write_bytes(original)
                    restored = compile(label + '-restore')
                    target(label + '-green', case)
                    if sha(path) != hashlib.sha256(original).hexdigest(): raise RuntimeError('Restoration failed')
                    results.append(dict(group=label, case=case, normalAssertionRed=True, artifacts=changed, restoredArtifacts=restored))
                    print('PASS ' + label + ': compiled normal assertion red / same archive config green / restored / SHA256', flush=True)
                run('final', ['ctest', '--test-dir', build, '-R', '^process_stream_', '--output-on-failure'])
            if any(sha(ROOT / name) != digest for name, digest in inputs.items()): raise RuntimeError('Root inputs changed')
            receipt = dict(accepted=True, cases=25, groups=results, inputs=inputs, actualArtifacts=artifact_hashes(),
                sanitizer=['address', 'undefined'], optimization='O1', leakSanitizer=False,
                scope='owned host native process provider and isolated faults; no public ABI/App/model')
            if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
            print('PROCESS STREAM PASS: 25 cases; ' + str(len(results)) + ' mutation groups', flush=True)
        finally:
            (source / SOURCE).write_bytes(original)
            close_children()


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
