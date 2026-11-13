#!/usr/bin/env python3
"""Actual checked-argument decoder: non-NUL member tail, scratch OOM/release and same archive config control."""
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
SOURCE = ROOT / 'core/src/extension/package-arguments.c'
CASES = [
    ('terminator', 'schema_json[schema_size] = 0;', '/* no terminator */', 'tail'),
    ('scratch-allocation', 'gt__plugin_memory_alloc(NULL, schema_size + 1)', 'malloc(schema_size + 1)', 'oom'),
    ('scratch-release', 'gt__plugin_memory_free(NULL, schema_json, schema_size + 1);', '(void)schema_json;', 'tail'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path, help='ASan/UBSan JS-enabled Core build directory')
    args = parser.parse_args(); build = args.build.resolve(strict=True)
    cache = (build / 'CMakeCache.txt').read_text()
    if 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON' not in cache or '-fsanitize=address,undefined' not in cache:
        raise RuntimeError('Require the actual sanitized JS-enabled Core archive; no fallback')
    original = SOURCE.read_bytes(); before = hashlib.sha256(original).hexdigest()
    for name, old, _, _ in CASES:
        if original.count(old.encode()) != 1: raise RuntimeError(name + ': source anchor mismatch')
    with tempfile.TemporaryDirectory(prefix='argument-schema-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); copied = work / 'core/src/extension/package-arguments.c'; copied.parent.mkdir(parents=True)
        copied.write_bytes(original)
        tests = work / 'core/tests'; tests.mkdir()
        shutil.copyfile(ROOT / 'core/tests/test_package_arguments_boundary.c', tests / 'test_package_arguments_boundary.c')
        env = dict(os.environ, TMPDIR=str(work), PKG_CONFIG_PATH=str(ROOT / 'build/host-native/lib/pkgconfig'),
            ASAN_OPTIONS='detect_leaks=0:abort_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        def run(command, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require argument-schema; shift; gt_timeout 120 "$@"'
            result = subprocess.run(['bash', '-c', shell, 'argument-schema', str(ROOT), *map(str, command)],
                cwd=ROOT, env=env, text=True, capture_output=True)
            output = result.stdout + result.stderr
            if red:
                accepted = result.returncode == 1 and output.count('FAIL arguments:') == 1
                accepted = accepted and not any(mark in output for mark in ['AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer'])
            else: accepted = result.returncode == 0
            if not accepted: raise RuntimeError(str(command) + '\n' + output)
            return output
        libs = shlex.split(run(['pkg-config', '--libs', 'libssh2']))
        flags = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-D_GNU_SOURCE', '-O1', '-g',
            '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-DGT_HAVE_PLUGIN_JS=1',
            '-I' + str(ROOT / 'core/include'), '-I' + str(ROOT / 'core/src'), '-I' + str(ROOT / 'vendor/quickjs-ng')]
        archives = [build / 'libghosterm_core.a', build / 'libghosterm_quickjs.a', *libs, '-lm', '-lpthread']
        binary = work / 'arguments'
        def compile(): run([*flags, tests / 'test_package_arguments_boundary.c', *archives, '-o', binary])
        def green():
            compile()
            for target in ['tail', 'oom', 'capacity', 'control']: run([binary, target])
        config = work / 'config'
        run([*flags, '-DGT_TEST_TMP_DIR="' + str(work) + '"', ROOT / 'core/tests/test_config.c', *archives, '-o', config])
        green(); print('BASELINE PASS: exact checked schema tail / scratch OOM / capacity / missing command', flush=True)
        for name, old, new, target in CASES:
            try:
                copied.write_bytes(original.replace(old.encode(), new.encode(), 1)); compile(); run([binary, target], red=True)
                run([config, ROOT / 'core/tests/fixtures', work]); run([binary, 'control'])
            finally: copied.write_bytes(original)
            green()
            if hashlib.sha256(copied.read_bytes()).hexdigest() != before: raise RuntimeError('Restore SHA256 mismatch')
            print('PASS ' + name + ': compiled normal assertion red / same archive config green / restored green / SHA256', flush=True)
    if hashlib.sha256(SOURCE.read_bytes()).hexdigest() != before: raise RuntimeError('Original decoder source changed')
    print('ARGUMENT SCHEMA PASS: 3 isolated ASan/UBSan groups; no LSan', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
