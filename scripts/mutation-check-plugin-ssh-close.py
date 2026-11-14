#!/usr/bin/env python3
"""Prove SSH close continuation assertions using an isolated compiled owner.

The libssh2 close boundary is injected, not an SSH server. Old repeated I/O must
fail normally; sanitizer, compiler, crash and timeout failures never count red.
"""
from pathlib import Path
import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BACKEND = 'src/extension/backends/ssh_exec.c'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    (ROOT / 'tmp').mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='plugin-ssh-close-', dir=ROOT / 'tmp'))
    logs = work / 'logs'
    logs.mkdir()
    source, build = work / 'source', work / 'build'
    inputs = [*sorted((ROOT / 'core/src').rglob('*')),
              *sorted((ROOT / 'core/include').rglob('*')),
              *sorted((ROOT / 'core/cmake').rglob('*')),
              *sorted((ROOT / 'patches/libssh2/1.11.1').rglob('*')),
              *sorted((ROOT / 'build/host-native/lib').glob('libssh2*')),
              ROOT / 'build/host-native/receipt.json',
              ROOT / 'core/CMakeLists.txt', ROOT / 'core/tests/test_bridge_ssh_close.c',
              ROOT / 'core/tests/test_config.c', ROOT / 'scripts/lib/gt-timeout.sh',
              Path(__file__)]
    hashes = {str(p.relative_to(ROOT)): sha(p) for p in inputs if p.is_file()}
    commands = []
    env = {**os.environ, 'TMPDIR': str(work),
           'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1',
           'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}

    def run(label, argv, *, red=False, timeout=180):
        shell = ('. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-ssh-close; '
                 'shift; budget="$1"; shift; gt_timeout "$budget" "$@"')
        p = subprocess.run(['bash', '-c', shell, 'plugin-ssh-close', str(ROOT),
                            str(timeout), *map(str, argv)], cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=timeout + 10)
        output = p.stdout + p.stderr
        (logs / (label + '.log')).write_text(output)
        commands.append(dict(label=label, returncode=p.returncode))
        if red:
            if (p.returncode != 1 or 'FAIL main:' not in output
                    or ': !fixture.late_calls' not in output
                    or any(x in output for x in ('AddressSanitizer', 'runtime error:',
                                                 'UndefinedBehaviorSanitizer'))):
                raise RuntimeError(label + ': missing normal target assertion\n' + output)
        elif p.returncode:
            raise RuntimeError(label + ': failed\n' + output)
        return output

    def rebuild(label):
        objects = [('ghosterm_core', BACKEND),
                   ('test_bridge_ssh_close', 'tests/test_bridge_ssh_close.c')]
        for target, name in objects:
            (build / 'CMakeFiles' / (target + '.dir') / (name + '.o')).unlink(missing_ok=True)
        for name in ('libghosterm_core.a', 'test_bridge_ssh_close', 'test_config'):
            (build / name).unlink(missing_ok=True)
        output = run(label + '-build', ['cmake', '--build', build, '--target',
                                       'test_bridge_ssh_close', 'test_config', '-j2'])
        for target, name in objects:
            if 'Building C object CMakeFiles/' + target + '.dir/' + name + '.o' not in output:
                raise RuntimeError(label + ': actual compilation missing: ' + name)
        for marker in ('Linking C static library libghosterm_core.a',
                       'Linking C executable test_bridge_ssh_close',
                       'Linking C executable test_config'):
            if marker not in output:
                raise RuntimeError(label + ': actual link missing: ' + marker)

    shutil.copytree(ROOT / 'core', source, ignore=shutil.ignore_patterns('build', 'build-*'))
    run('configure', ['cmake', '-S', source, '-B', build, '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
                      '-DCMAKE_C_FLAGS_RELWITHDEBINFO=-O2 -g -UNDEBUG',
                      '-DGHOSTERM_WITH_LIBSSH2=ON', '-DGHOSTERM_WITH_PLUGIN_JS=OFF',
                      '-DGHOSTERM_BUILD_TESTS=ON',
                      '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
    rebuild('baseline')
    modes = ('session', 'parent', 'blocked')
    for mode in modes:
        run('baseline-' + mode, [build / 'test_bridge_ssh_close', mode])
    run('baseline-control', [build / 'test_config', source / 'tests/fixtures'])
    path = source / BACKEND
    original = path.read_bytes()
    old = '    if (!s->disposing) {\n'
    if original.decode().count(old) != 1:
        raise RuntimeError('ambiguous close guard')
    try:
        path.write_text(original.decode().replace(old, '    if (true) { /* old repeated preparation */\n'))
        rebuild('mutant')
        for mode in modes:
            run('mutant-' + mode, [build / 'test_bridge_ssh_close', mode], red=True)
        run('mutant-control', [build / 'test_config', source / 'tests/fixtures'])
    finally:
        path.write_bytes(original)
    if path.read_bytes() != original:
        raise RuntimeError('restore mismatch')
    rebuild('restored')
    for mode in modes:
        run('restored-' + mode, [build / 'test_bridge_ssh_close', mode])
    run('restored-control', [build / 'test_config', source / 'tests/fixtures'])
    if hashes != {name: sha(ROOT / name) for name in hashes}:
        raise RuntimeError('worktree inputs changed during validation')
    result = dict(accepted=True, groups=['repeated-close-preparation'], modes=list(modes),
                  inputs=hashes, commands=commands, restored_backend_sha256=sha(path),
                  scope='injected native libssh2 disposal boundary; no live SSH or CI reproduction',
                  sanitizers='optimized ASan/UBSan; leak detection disabled')
    (work / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print('PLUGIN SSH CLOSE MUTATION PASS: one group, three normal assertion reds; ' + str(work), flush=True)


if __name__ == '__main__':
    main()
