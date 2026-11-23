#!/usr/bin/env python3
"""Verify the native Lua view registry and isolated compiled mutations."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VIEWS = 'src/extension/runtime-views.inc'
LUA = 'src/extension/runtime-lua.inc'
CASES = {
    'role': (VIEWS, 'if (r->role != GT_EXT_HOST)', 'if (false)', 'executor'),
    'grant': (VIEWS, 'gt_ext_session_access(r->session, &r->rpc.lease, capability) != GT_EXT_ACCESS_ALLOWED',
              'false', 'ungranted'),
    'snapshot-revocation': (VIEWS, 'if ((r->views->kinds & (1u << i)) && strcmp(runtime_view_access(r, kind), "ok"))',
                            'if (false && (r->views->kinds & (1u << i)) && strcmp(runtime_view_access(r, kind), "ok"))', 'roundtrip'),
    'direction': (VIEWS, 'methods[i].receiver == GT_BRIDGE_EXECUTOR', 'true', 'direction'),
    'capacity': (LUA, 'if (!entry->handle[0] && !slot)', 'if (i < 7 && !entry->handle[0] && !slot)', 'limit'),
    'duplicate': (LUA, 'entry->handle[0] && !strcmp(entry->id, candidate.id)',
                  'false && entry->handle[0] && !strcmp(entry->id, candidate.id)', 'duplicate'),
    'close': (VIEWS, 'gt__secure_erase(view, sizeof(*view));', '(void)view;', 'native-close'),
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--lua-archive', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args()
    archive = args.lua_archive.resolve(strict=True)
    receipt = args.receipt.resolve()
    if not receipt.is_relative_to(ROOT / 'tmp'):
        parser.error('receipt must be under project tmp')
    core = ROOT / 'core'
    paths = [core / name for name in (VIEWS, LUA, 'CMakeLists.txt', 'tests/test_runtime_views.c')]
    inputs = {str(path): sha(path) for path in [archive, Path(__file__), *paths]}
    with tempfile.TemporaryDirectory(prefix='views-proof-', dir=os.environ['TMPDIR']) as temporary:
        work = Path(temporary)
        source, build = work / 'core', work / 'build'
        (work / 'tmp').mkdir()
        (work / 'patches').symlink_to(ROOT / 'patches', target_is_directory=True)
        shutil.copytree(core, source, ignore=shutil.ignore_patterns('build', 'build-*'))
        env = {**os.environ, 'TMPDIR': temporary, 'TMP': temporary, 'TEMP': temporary,
               'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}
        binary = build / 'test_runtime_views'

        def run(*command, red=False):
            child = subprocess.run(command, env=env, capture_output=True, text=True, timeout=180)
            output = child.stdout + child.stderr
            if red:
                assert child.returncode == 1 and child.stderr.count('FAIL ') == 1, output
                assert not any(item in output for item in ('AddressSanitizer', 'UndefinedBehaviorSanitizer', 'runtime error:')), output
            elif child.returncode:
                raise RuntimeError(output)
            return output

        def compile_core():
            for path in (build/'CMakeFiles/ghosterm_core.dir/src/extension/runtime.c.o',
                         build/'libghosterm_core.a', binary):
                path.unlink(missing_ok=True)
            run('cmake', '--build', build, '--parallel', '4', '--target', 'test_runtime_views', 'test_config')
            return sha(binary)

        def baseline():
            output = run('ctest', '--test-dir', build, '-R', '^lua_views_', '--no-tests=error', '--output-on-failure')
            assert '100% tests passed out of 11' in output, output

        run('cmake', '-S', source, '-B', build, '-DGHOSTERM_BUILD_TESTS=ON',
            '-DGHOSTERM_WITH_PLUGIN_LUA=ON', '-DGHOSTERM_WITH_LIBSSH2=OFF',
            f'-DGHOSTERM_LUA_ARCHIVE={archive}', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
            '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all')
        compile_core()
        baseline()
        groups = []
        for name, (relative, old, new, mode) in CASES.items() if args.mutate else ():
            target = source / relative
            original = target.read_text()
            assert original.count(old) == 1, name
            baseline_sha = sha(binary)
            try:
                target.write_text(original.replace(old, new, 1))
                assert compile_core() != baseline_sha, name
                run(binary, mode, red=True)
                run(build / 'test_config', ROOT / 'core/tests/fixtures')
            finally:
                target.write_text(original)
            compile_core()
            run(binary, mode)
            assert sha(target) == inputs[str(core / relative)], name
            groups.append(name)
            print(f'PASS {name}: compiled assertion / Core control / restore / SHA256', flush=True)
        baseline()
        assert all(sha(Path(path)) == digest for path, digest in inputs.items())
        receipt.write_text(json.dumps(dict(accepted=True, groups=groups, nativeCases=11,
                                           inputs=inputs, binarySHA256=sha(binary)), indent=2) + '\n')
        print(f'LUA VIEWS PASS: 11 native cases; {len(groups)} groups', flush=True)


if __name__ == '__main__':
    main()
