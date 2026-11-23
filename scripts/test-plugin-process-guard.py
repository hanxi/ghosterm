#!/usr/bin/env python3
"""Isolated POSIX owner-death checks, O1 ASan/UBSan and compiled normal-red mutations.

Only forked disposable local fixtures. No service, user endpoint or UI window.
Fixture self-alarms are outside the unchanged two-second acceptance deadline.
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


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'):
        parser.error('receipt must be in project tmp')
    files = [p for folder in ('core/src', 'core/include', 'core/tests') for p in (ROOT / folder).rglob('*') if p.is_file()]
    marker = b'GT_PROCESS_GUARD_MUTATION'
    if any(marker in p.read_bytes() for p in files):
        raise RuntimeError('Refuse leftover process-guard mutation marker')
    files += [ROOT / 'core/CMakeLists.txt', ROOT / 'scripts/lib/gt-timeout.sh', Path(__file__).resolve()]
    inputs = {str(p.relative_to(ROOT)): sha(p) for p in files}
    guard = 'core/src/extension/process-guard.c'
    owner = 'core/src/extension/process-owner.c'
    cases = [
        ('parent-death', guard, 'if (!connected && !stopping)', 'if (false)', 'guard:kill', 2),
        ('escalation', guard, 'kill(-child, SIGKILL)', 'kill(-child, SIGTERM)', 'guard:kill', 1),
        ('group', guard, 'kill(-child, SIGKILL)', 'kill(child, SIGKILL)', 'guard:descendants', 1),
        ('exit-code', guard, 'message.exit_code = state.si_status;', 'message.exit_code = 0;', 'channel:tail', 1),
        ('exit-signal', guard, 'message.term_signal = state.si_status;', 'message.term_signal = 0;', 'channel:signal', 1),
        ('record-order', owner, '} else {\n            return GT_ERR_IO;\n        }\n        process->report_size = 0;',
         '} else {\n            /* skipped validation */\n        }\n        process->report_size = 0;', 'protocol:kind', 1),
        ('eof-idempotent', owner, 'return process_io(GT_EXT_PROCESS_EOF, 0, GT_OK);\n    }\n    ssize_t count',
         'return process_io(GT_EXT_PROCESS_ERROR, 0, GT_ERR_IO);\n    }\n    ssize_t count', 'owner:environment', 1),
    ]
    with tempfile.TemporaryDirectory(prefix='pg-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'; build = work / 'b'; logs = work / 'logs'; logs.mkdir()
        shutil.copytree(ROOT / 'core', source / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
        for name in ('scripts', 'packages', 'vendor', 'patches', 'build'):
            (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = {**os.environ, 'TMPDIR': str(ROOT / 'tmp'), 'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        commands = []

        def run(label, command, red=False):
            # Resolve the shared timeout helper, then replace the shell so the
            # timeout owns the exact child PID/process group retained by Popen.
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require process-guard; shift; exec "$gt_timeout_bin" "$@"',
                       'process-guard', ROOT, 180, *command]
            p = subprocess.Popen(list(map(str, bounded)), cwd=ROOT, env=env,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                errors='replace', start_new_session=True)
            try:
                stdout, stderr = p.communicate(timeout=180)
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
            output = stdout + stderr; (logs / (label + '.log')).write_text(output)
            commands.append(dict(label=label, returncode=p.returncode))
            if red:
                failures = re.findall(r'^(?:ASSERT:|FAIL process_channel |ASSERT: /)', output, re.M)
                if p.returncode != 1 or len(failures) != 1 or any(x in output for x in
                    ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:', 'unexpected signal')):
                    raise RuntimeError(label + ': expected one normal assertion red\n' + output)
            elif p.returncode:
                raise RuntimeError(label + '\n' + output)
            return output

        def compile(label):
            targets = ('test_process_guard', 'test_process_guard_protocol', 'test_extension_process', 'test_process_channel', 'test_process_channel_child', 'test_config')
            run(label, ['cmake', '--build', build, '--target', *targets, '-j2'])
            return {name: sha(build / name) for name in (*targets, 'libghosterm_core.a',
                'CMakeFiles/ghosterm_core.dir/src/extension/process-guard.c.o',
                'CMakeFiles/ghosterm_core.dir/src/extension/process-owner.c.o')}

        def target(label, case, red=False):
            family, mode = case.split(':')
            command = [build / 'test_process_guard_protocol', mode] if family == 'protocol' else \
                [build / 'test_process_guard', mode, source / 'tmp'] if family == 'guard' else \
                [build / 'test_extension_process', mode] if family == 'owner' else \
                [build / 'test_process_channel', mode, build / 'test_process_channel_child', source / 'tmp']
            run(label, command, red)

        run('configure', ['cmake', '-S', source / 'core', '-B', build, '-DGHOSTERM_WITH_LIBSSH2=OFF',
            '-DGHOSTERM_WITH_PLUGIN_LUA=OFF', '-DGHOSTERM_BUILD_TESTS=ON', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
            '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O1 -g -DNDEBUG',
            '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        baseline = compile('baseline-build')
        run('baseline', ['ctest', '--test-dir', build, '--output-on-failure', '-R', '^(process_guard_|extension_process_|process_channel_)'])
        groups = []
        for name, file, old, new, case, count in cases if args.mutate else []:
            path = source / file; original = path.read_bytes()
            if original.count(old.encode()) != count: raise RuntimeError(name + ': mutation anchor changed')
            try:
                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n/* ' + marker + b' */\n')
                mutant = compile(name + '-build')
                changed = 'CMakeFiles/ghosterm_core.dir/src/extension/' + Path(file).name + '.o'
                if baseline[changed] == mutant[changed]: raise RuntimeError(name + ': object unchanged')
                target(name + '-red', case, True)
                run(name + '-control', [build / 'test_config', ROOT / 'core/tests/fixtures'])
            finally: path.write_bytes(original)
            baseline = compile(name + '-restore-build'); target(name + '-restored', case)
            if sha(path) != hashlib.sha256(original).hexdigest(): raise RuntimeError('restore mismatch')
            groups.append(name); print(name + ': compiled normal assertion red / same archive configuration green / restore / SHA256', flush=True)
        run('final', ['ctest', '--test-dir', build, '--output-on-failure', '-R', '^(process_guard_|extension_process_|process_channel_)'])
        if any(sha(ROOT / p) != h for p, h in inputs.items()): raise RuntimeError('Root inputs changed')
        record = dict(accepted=True, groups=groups, inputs=inputs, commands=commands, binaries=baseline,
                      scope='O1 ASan/UBSan, LSan off; owned POSIX parent death, real exits/signals and private group; no arbitrary descendant guarantee')
        if args.receipt: args.receipt.write_text(json.dumps(record, indent=2) + '\n')
        print('PROCESS GUARD PASS: ' + str(len(groups)) + ' groups / ' + str(len(inputs)) + ' inputs', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
