#!/usr/bin/env python3
"""Real engine CPU versus activation clocks; controlled native wait and same archive config control."""
import argparse
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'core/src/extension/runtime-quickjs.inc'
CASES = [
    ('cpu-wall-clock', 'clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)', 'clock_gettime(CLOCK_MONOTONIC, &t)', 'wait'),
    ('activation-cpu-clock', 'clock_gettime(CLOCK_MONOTONIC, &t)', 'clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t)', 'wall'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=Path, help='compiled portable SDK directory')
    parser.add_argument('--build', required=True, type=Path, help='ASan/UBSan JS-enabled Core build directory')
    args = parser.parse_args(); build = args.build.resolve(strict=True); sdk = args.sdk.resolve(strict=True)
    cache = (build / 'CMakeCache.txt').read_text()
    if 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON' not in cache or '-fsanitize=address,undefined' not in cache:
        raise RuntimeError('Require the actual sanitized JS-enabled Core archive; no fallback')
    original = SOURCE.read_bytes(); before = hashlib.sha256(original).hexdigest()
    for name, old, _, _ in CASES:
        if original.count(old.encode()) != 1: raise RuntimeError(name + ': source anchor mismatch')
    with tempfile.TemporaryDirectory(prefix='runtime-clock-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); copied = work / 'core/src/extension/runtime-quickjs.inc'; copied.parent.mkdir(parents=True)
        copied.write_bytes(original)
        tests = work / 'core/tests'; tests.mkdir()
        shutil.copyfile(ROOT / 'core/src/extension/runtime.c', copied.parent / 'runtime.c')
        for name in ['test_runtime_cpu_clock.c', 'test_runtime.c', 'test_ext_rpc.c', 'test_ext_session.c']:
            shutil.copyfile(ROOT / 'core/tests' / name, tests / name)
        env = dict(os.environ, TMPDIR=str(work), PKG_CONFIG_PATH=str(ROOT / 'build/host-native/lib/pkgconfig'),
            ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        def run(command, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require runtime-clock; shift; gt_timeout 120 "$@"'
            result = subprocess.run(['bash', '-c', shell, 'runtime-clock', str(ROOT), *map(str, command)],
                cwd=ROOT, env=env, text=True, capture_output=True)
            output = result.stdout + result.stderr
            if red:
                accepted = result.returncode == 1 and output.count('FAIL ') == 1
                accepted = accepted and not any(mark in output for mark in ['AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'])
            else: accepted = result.returncode == 0
            if not accepted: raise RuntimeError(str(command) + '\n' + output)
            return output
        libs = shlex.split(run(['pkg-config', '--libs', 'libssh2']))
        flags = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-O1', '-g',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DGT_HAVE_PLUGIN_JS=1', '-DGT_TEST_PLUGIN_JS=1', '-I' + str(work / 'core/src'),
            '-I' + str(ROOT / 'core/include'), '-I' + str(ROOT / 'core/src'), '-I' + str(ROOT / 'vendor/quickjs-ng')]
        archives = [build / 'libghosterm_core.a', build / 'libghosterm_quickjs.a', *libs, '-lm', '-lpthread']
        binary = work / 'clock'
        def compile(): run([*flags, tests / 'test_runtime_cpu_clock.c', *archives, '-o', binary])
        def green():
            compile()
            for target in ['wait', 'wall', 'cpu', 'thread-handoff']: run([binary, target, sdk])
        config = work / 'config'
        run([*flags, '-DGT_TEST_TMP_DIR="' + str(work) + '"', ROOT / 'core/tests/test_config.c', *archives, '-o', config])
        green(); print('BASELINE PASS: native wait / wall activation / infinite CPU / thread handoff', flush=True)
        for name, old, new, target in CASES:
            try:
                copied.write_bytes(original.replace(old.encode(), new.encode(), 1)); compile(); run([binary, target, sdk], red=True)
                run([config, ROOT / 'core/tests/fixtures', work]); run([binary, 'cpu', sdk])
            finally: copied.write_bytes(original)
            green()
            if hashlib.sha256(copied.read_bytes()).hexdigest() != before: raise RuntimeError('Restore SHA256 mismatch')
            print('PASS ' + name + ': compiled normal assertion red / same archive config green / restored green / SHA256', flush=True)
    if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != before: raise RuntimeError('Original runtime source changed')
    print('RUNTIME CLOCK PASS: 2 isolated ASan/UBSan groups; no LSan', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
