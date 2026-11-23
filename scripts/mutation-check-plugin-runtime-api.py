#!/usr/bin/env python3
"""Verify public Lua runtime adoption in isolated sanitizer builds."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--lua-archive', type=Path, required=True)
parser.add_argument('--case', action='append', choices=['outgoing-owner', 'taken-owner', 'untaken-peer', 'resource-owner'])
parser.add_argument('--receipt', type=Path)
args = parser.parse_args()
archive = args.lua_archive.resolve(strict=True)
archive_sha = hashlib.sha256(archive.read_bytes()).digest()
if args.receipt and not args.receipt.resolve().is_relative_to(root / 'tmp'):
    parser.error('receipt must be under project tmp')
with tempfile.TemporaryDirectory(prefix='lua-ownership-', dir=os.environ['TMPDIR']) as directory:
    work = Path(directory)
    source = work / 'source'
    build = work / 'build'
    shutil.copytree(root / 'core', source / 'core', ignore=shutil.ignore_patterns('build', 'build-*'))
    (source / 'tmp').mkdir()
    (source / 'patches').symlink_to(root / 'patches', target_is_directory=True)
    env = {**os.environ, 'TMPDIR': str(work), 'TMP': str(work), 'TEMP': str(work),
           'ASAN_OPTIONS': 'detect_leaks=0:abort_on_error=1', 'UBSAN_OPTIONS': 'halt_on_error=1'}

    def run(*args, red=False):
        result = subprocess.run(args, capture_output=True, text=True, env=env, timeout=90)
        if red:
            assert result.returncode == 1 and result.stderr.count('FAIL ') == 1, result.stderr
            assert not any(word in result.stderr for word in ('AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:')), result.stderr
        else:
            assert result.returncode == 0, result.stdout + result.stderr

    def compile_core(owner=None):
        if owner:
            (build / 'CMakeFiles/ghosterm_core.dir/src/extension' / (owner + '.o')).unlink(missing_ok=True)
            (build / 'libghosterm_core.a').unlink(missing_ok=True)
            (build / 'test_ext_runtime_ownership').unlink(missing_ok=True)
        run('cmake', '--build', build, '--parallel', '5', '--target', 'test_ext_runtime_ownership', 'test_config')
        return hashlib.sha256((build / 'test_ext_runtime_ownership').read_bytes()).digest()

    run('cmake', '-S', source/'core', '-B', build, '-DGHOSTERM_BUILD_TESTS=ON',
        '-DGHOSTERM_WITH_PLUGIN_LUA=ON', '-DGHOSTERM_WITH_LIBSSH2=OFF',
        f'-DGHOSTERM_LUA_ARCHIVE={archive}', '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all')
    compile_core()
    run('ctest','--test-dir', build,'-R','^lua_runtime_', '--output-on-failure')
    cases = [
        ('outgoing-owner', 'rpc.c', 'r->out[i].used || (r->in[i].used && r->in[i].taken && !r->in[i].done)', '(r->in[i].used && r->in[i].taken && !r->in[i].done)'),
        ('taken-owner', 'rpc.c', 'r->out[i].used || (r->in[i].used && r->in[i].taken && !r->in[i].done)', 'r->out[i].used'),
        ('untaken-peer', 'rpc.c', 'r->out[i].used || (r->in[i].used && r->in[i].taken && !r->in[i].done)', 'r->out[i].used || (r->in[i].used && !r->in[i].done)'),
        ('resource-owner', 'session.c', '!gt__bridge_transfer_has_owner(s->resources);', 'true;'),
    ]
    groups = []
    for mode, name, old, new in cases:
        if args.case and mode not in args.case:
            continue
        relative = Path('core/src/extension') / name
        path = source / relative
        before = (root / relative).read_bytes()
        assert path.read_bytes() == before and before.decode().count(old) == 1
        unmodified = compile_core()
        try:
            path.write_text(before.decode().replace(old, new, 1))
            assert compile_core(name) != unmodified
            run(build/'test_ext_runtime_ownership', mode, source/'tmp', red=True)
            run(build/'test_config', root/'core/tests/fixtures')
        finally:
            path.write_bytes(before)
        compile_core(name)
        run(build/'test_ext_runtime_ownership', mode, source/'tmp')
        assert hashlib.sha256(path.read_bytes()).digest() == hashlib.sha256((root/relative).read_bytes()).digest()
        groups.append(mode)
        print('PASS', mode, 'compiled assertion / Core control / restore / SHA256', flush=True)
    run('ctest','--test-dir', build,'-R','^lua_runtime_', '--output-on-failure')
    assert archive_sha == hashlib.sha256(archive.read_bytes()).digest()
    if args.receipt:
        args.receipt.write_text(json.dumps(dict(accepted=True, nativeCases=4, groups=groups,
                                             luaArchiveSHA256=archive_sha.hex()), indent=2) + '\n')
    print(f'LUA RUNTIME OWNERSHIP PASS: {len(groups)} groups', flush=True)
