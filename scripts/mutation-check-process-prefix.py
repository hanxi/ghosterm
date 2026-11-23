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
    parser.parse_args()
    archive = os.environ.get('GHOSTERM_LUA_ARCHIVE')
    if not archive or not Path(archive).is_file():
        parser.error('GHOSTERM_LUA_ARCHIVE must point to lua-5.5.1.tar.gz')
    inputs = [ROOT / 'core/src/extension' / name for name in ['process-owner.c', 'process-service.c']]
    inputs.append(ROOT / 'cli/gterm/stdio.c')
    before = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    with tempfile.TemporaryDirectory(prefix='process-prefix-mutations-', dir=os.environ['TMPDIR']) as temporary:
        work = Path(temporary); source = work / 'source'; build = work / 'build'
        shutil.copytree(ROOT / 'core', source / 'core',
                        ignore=shutil.ignore_patterns('build', 'build-*', '__pycache__'))
        (source / 'tmp').mkdir()
        environment = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary,
                           ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',
                           UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        def run(command, red=False):
            result = subprocess.run(list(map(str, command)), env=environment, cwd=ROOT, capture_output=True, text=True, timeout=300)
            output = result.stdout + result.stderr
            if red:
                if result.returncode != 1 or 'FAIL ' not in result.stderr or any(token in output for token in ['AddressSanitizer', 'runtime error:', 'UndefinedBehaviorSanitizer']):
                    raise RuntimeError('Expected compiled normal C assertion red:\n' + output)
            elif result.returncode: raise RuntimeError(output)
            return output
        run(['cmake', '-S', source / 'core', '-B', build, '-DGHOSTERM_WITH_PLUGIN_LUA=ON',
             '-DGHOSTERM_LUA_ARCHIVE=' + str(Path(archive).resolve()),
             '-DGHOSTERM_WITH_LIBSSH2=OFF', '-DGHOSTERM_BUILD_TESTS=ON',
             '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
             '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        def rebuild():
            for name in ['process-service.c', 'process-owner.c']:
                (build / 'CMakeFiles/ghosterm_core.dir/src/extension' / (name + '.o')).unlink(missing_ok=True)
            for name in ['libghosterm_core.a', 'test_process_prefix', 'test_config']:
                (build / name).unlink(missing_ok=True)
            output = run(['cmake', '--build', build, '--target', 'test_process_prefix', 'test_config', '-j', '4'])
            for token in ['process-service.c.o', 'process-owner.c.o', 'Linking C static library libghosterm_core.a', 'Linking C executable test_process_prefix']:
                if token not in output: raise RuntimeError('Mutation build did not rebuild ' + token)
        rebuild()
        package = work / 'package'
        run(['python3', '-B', ROOT / 'scripts/gterm-plugin', 'build',
             ROOT / 'plugins/official/ai-claude', '--out', package])
        modes = ('roundtrip', 'count', 'pointer', 'null-item', 'argument-bytes', 'total-bytes',
                 'combined-count', 'combined-bytes', 'count-boundary', 'revoked', 'memory')
        for mode in modes:
            run([build / 'test_process_prefix', mode, source / 'tmp', package])
        print('BASELINE PASS: 11 native prefix cases, ASan/UBSan; no LSan', flush=True)
        for name, file, old, new, target in CASES:
            path = source / 'core/src/extension' / file; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError(name + ': mutation anchor changed')
            try:
                path.write_bytes(original.replace(old.encode(), new.encode(), 1)); rebuild()
                run([build / 'test_process_prefix', target, source / 'tmp', package], red=True)
                run([build / 'test_config', source / 'core/tests/fixtures', source / 'tmp'])
            finally: path.write_bytes(original)
            rebuild(); run([build / 'test_process_prefix', target, source / 'tmp', package])
            if hashlib.sha256(path.read_bytes()).hexdigest() != hashlib.sha256(original).hexdigest(): raise RuntimeError('restore mismatch')
            print(f'PASS {name}: compiled assertion red / independent config green / restored green / SHA256', flush=True)
    if before != {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}: raise RuntimeError('working Core changed')
    print(f'PROCESS PREFIX PASS: {len(CASES)} mutation groups; original Core/CLI unchanged', flush=True)


if __name__ == '__main__': main()
