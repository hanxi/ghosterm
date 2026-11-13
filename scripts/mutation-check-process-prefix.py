#!/usr/bin/env python3
"""Compile isolated fixed-process-argument mutations; never edit the working Core."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = [
    ('empty-argument', 'process-owner.c', '(!length && i == 0)', '!length', 'roundtrip'),
    ('prefix-count-boundary', 'process-service.c', 'mappings[i].prefix_argc > GT_EXT_RUNTIME_PROCESS_PREFIX_MAX',
     'mappings[i].prefix_argc >= GT_EXT_RUNTIME_PROCESS_PREFIX_MAX', 'count-boundary'),
    ('argument-bytes', 'process-service.c', 'bytes > GT_EXT_PROCESS_ARGUMENT_MAX',
     'bytes > GT_EXT_PROCESS_ARGUMENT_MAX + 1', 'argument-bytes'),
    ('prefix-byte-boundary', 'process-service.c', 'bytes + 1 > GT_EXT_RUNTIME_PROCESS_PREFIX_BYTES_MAX - prefix_bytes',
     'bytes + 1 >= GT_EXT_RUNTIME_PROCESS_PREFIX_BYTES_MAX - prefix_bytes', 'combined-bytes'),
    ('retained-prefix', 'process-service.c', 'service->prefix_argc[i] = mappings[i].prefix_argc;',
     'service->prefix_argc[i] = 0;', 'roundtrip'),
    ('prefix-order', 'process-service.c', 'service->prefix_offsets[i][j];',
     'service->prefix_offsets[i][service->prefix_argc[i] - j - 1];', 'roundtrip'),
    ('plugin-tail', 'process-service.c', 'combined[j + service->prefix_argc[i]] = argv[j];',
     'combined[j + service->prefix_argc[i]] = argv[0];', 'roundtrip'),
    ('combined-count-boundary', 'process-service.c', 'service->prefix_argc[i] > GT_EXT_PROCESS_ARGUMENTS_MAX - argc',
     'service->prefix_argc[i] >= GT_EXT_PROCESS_ARGUMENTS_MAX - argc', 'count-boundary'),
    ('live-grant', 'process-service.c', 'gt_ext_session_access(service->session, &service->lease, "process.spawn")',
     'GT_EXT_ACCESS_ALLOWED', 'revoked'),
    ('shared-memory', 'process-service.c', 'gt__plugin_memory_calloc(1, sizeof(*service))',
     'calloc(1, sizeof(*service))', 'memory'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, required=True, help='compiled portable SDK, read-only input')
    parser.add_argument('--gterm-build', type=Path, help='native fixture build for two additional real Node pipeline mutations')
    args = parser.parse_args()
    inputs = [ROOT / 'core/src/extension' / name for name in ['process-owner.c', 'process-service.c']]
    inputs.append(ROOT / 'cli/gterm/stdio.c')
    before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    with tempfile.TemporaryDirectory(prefix='process-prefix-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'source'; build = work / 'build'
        for folder in ['core', 'cli/gterm', 'packages/plugin-sdk', 'patches/quickjs-ng', 'scripts']:
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('build', 'build-*', '__pycache__'))
        (source / 'vendor').mkdir()
        (source / 'vendor/quickjs-ng').symlink_to(ROOT / 'vendor/quickjs-ng', target_is_directory=True)
        (source / 'tmp').mkdir()
        environment = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        def run(command, red=False):
            result = subprocess.run(list(map(str, command)), env=environment, cwd=ROOT, capture_output=True, text=True, timeout=300)
            output = result.stdout + result.stderr
            if red:
                if result.returncode != 1 or 'FAIL ' not in result.stderr or any(token in output for token in ['AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer']):
                    raise RuntimeError('Expected compiled normal C assertion red:\n' + output)
            elif result.returncode: raise RuntimeError(output)
            return output
        run(['cmake', '-S', source / 'cli/gterm', '-B', build, '-DGHOSTERM_WITH_PLUGIN_JS=ON',
             '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_BUILD_TESTS=ON',
             '-DGHOSTERM_PLUGIN_SDK_DIR=' + str(args.sdk.resolve()), '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
             '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        def rebuild():
            for name in ['process-service.c', 'process-owner.c']:
                (build / 'core/CMakeFiles/ghosterm_core.dir/src/extension' / (name + '.o')).unlink(missing_ok=True)
            for name in ['libghosterm_core.a', 'test_process_prefix', 'test_config']:
                (build / 'core' / name).unlink(missing_ok=True)
            output = run(['cmake', '--build', build, '--target', 'test_process_prefix', 'test_config', '-j', '4'])
            for token in ['process-service.c.o', 'process-owner.c.o', 'Linking C static library libghosterm_core.a', 'Linking C executable test_process_prefix']:
                if token not in output: raise RuntimeError('Mutation build did not rebuild ' + token)
        rebuild()
        output = run(['ctest', '--test-dir', build / 'core', '-R', '^process_prefix_', '--no-tests=error', '--output-on-failure'])
        if '100% tests passed out of 11' not in output: raise RuntimeError('prefix baseline count mismatch:\n' + output)
        print('BASELINE PASS: 11 native prefix cases, ASan/UBSan; no LSan', flush=True)
        for name, file, old, new, target in CASES:
            path = source / 'core/src/extension' / file; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError(name + ': mutation anchor changed')
            try:
                path.write_bytes(original.replace(old.encode(), new.encode(), 1)); rebuild()
                run([build / 'core/test_process_prefix', target, source / 'tmp'], red=True)
                run([build / 'core/test_config', source / 'core/tests/fixtures', source / 'tmp'])
            finally: path.write_bytes(original)
            rebuild(); run([build / 'core/test_process_prefix', target, source / 'tmp'])
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashlib.sha256(original).hexdigest(): raise RuntimeError('restore mismatch')
            print(f'PASS {name}: compiled assertion red / independent config green / restored green / SHA256', flush=True)
        extra = 0
        if args.gterm_build:
            path = source / 'cli/gterm/stdio.c'
            for name, old, new in [
                ('gterm-fixed-argument', 'program->prefix[program->prefix_count++] = argv[at + 2];',
                 'program->prefix[program->prefix_count++] = argv[at + 1];'),
                ('gterm-selected-environment', 'default_count + extra_environment_count', 'default_count'),
            ]:
                original = path.read_bytes()
                if original.count(old.encode()) != 1: raise RuntimeError(name + ': mutation anchor changed')
                def cli_build():
                    (build / 'CMakeFiles/gterm.dir/stdio.c.o').unlink(missing_ok=True)
                    (build / 'gterm').unlink(missing_ok=True)
                    output = run(['cmake', '--build', build, '--target', 'gterm', '-j', '4'])
                    if 'stdio.c.o' not in output or 'Linking C executable gterm' not in output: raise RuntimeError('CLI did not rebuild')
                target = ['python3', '-B', ROOT / 'scripts/test-gterm-node.py', '--build', args.gterm_build.resolve(), '--binary', build / 'gterm']
                try:
                    path.write_bytes(original.replace(old.encode(), new.encode(), 1)); cli_build()
                    run(target, red=True)
                    run([build / 'core/test_config', source / 'core/tests/fixtures', source / 'tmp'])
                finally: path.write_bytes(original)
                cli_build(); run(target)
                if hashlib.sha256(path.read_bytes()).hexdigest() != hashlib.sha256(original).hexdigest(): raise RuntimeError('CLI restore mismatch')
                extra += 1
                print(f'PASS {name}: rebuilt CLI assertion red / independent config green / restored real Node pipeline / SHA256', flush=True)
    if before != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}: raise RuntimeError('working Core changed')
    print(f'PROCESS PREFIX PASS: {len(CASES) + extra} mutation groups; original Core/CLI unchanged', flush=True)


if __name__ == '__main__': main()
