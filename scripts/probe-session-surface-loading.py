#!/usr/bin/env python3
"""Native full surface restores in-progress Kitty chunks; P0 remains incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import signal
import struct
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-loading.py'))
FULL, VT, PATCHES = PREVIOUS['FULL'], PREVIOUS['VT'], PREVIOUS['PATCHES']
SOURCE = ROOT / 'terminal/session-state/full-surface/loading-probe.m'
FIXTURE = ROOT / 'terminal/session-state/full-surface/loading-fixture.c'
MARK = 'GT_P0_SURFACE_LOADING_MUTATION_MARK'
CASES = ('direct', 'quiet', 'no-display', 'zlib', 'png', 'continuation', 'screens', 'overwrite', 'static', 'coexist', 'implicit')


def fixtures(engine, work, *, build_vt=True):
    if build_vt: VT['build'](engine, work, True)
    binary = work / 'loading-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'),
                 '-I', str(ROOT / 'terminal/session-state'), str(FIXTURE),
                 str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    FULL['run']([str(binary), str(work)])
    for name in CASES:
        canonical = (work / (name + '-initial.canonical')).read_bytes()
        resources = (work / (name + '-initial.resources')).read_bytes()
        data = (b'GHOSTERM-P0-CHECKPOINT-1' + struct.pack('<QQQQ', 81, 40, 1024*1024, len(canonical)) +
                canonical + struct.pack('<Q', len(resources)) + resources)
        (work / (name + '.digest')).write_bytes(hashlib.sha256(data).digest())


def build(engine, work):
    PREVIOUS['PREVIOUS']['PREVIOUS']['PREVIOUS']['prune_own_cache'](work)
    FULL['build'](engine, work, cache_dir=work / 'full-cache', log_path=ROOT / 'tmp/session-loading-surface-build.log')
    binary = work / 'surface-loading'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def mutations(engine, work):
    variants = (
        ('loading state omitted from import', 'src/terminal/p0_loading_snapshot.zig',
         'return loading;', 'loading.destroy(alloc); return null;', 'direct', 'static'),
        ('implicit loading identity lost', 'src/terminal/p0_loading_snapshot.zig',
         'loading.image.metadata.implicit_id = flags & 2 != 0;',
         'loading.image.metadata.implicit_id = false;', 'implicit', 'quiet'),
        ('loading compression lost', 'src/terminal/p0_loading_snapshot.zig',
         'loading.image.compression = try enumeration(command.Transmission.Compression, r);',
         '_ = try enumeration(command.Transmission.Compression, r);', 'zlib', 'direct'),
        ('loading import corrupts an unrelated image', 'src/terminal/p0_resource_snapshot.zig',
         'storage.loading = try loading_snapshot.restore(alloc, &reader, storage.image_limits, max_bytes);',
         'storage.loading = try loading_snapshot.restore(alloc, &reader, storage.image_limits, max_bytes);\n        if (storage.loading != null) { if (storage.images.getPtr(8)) |image| @memset(@constCast(image.data.complete), 0); }',
         'coexist', 'direct'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique full loading mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            FULL['probe'](binary, work, target, 1); FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('full loading rollback mismatch')
        binary = build(engine, work); FULL['probe'](binary, work, target)
    FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64': raise RuntimeError('full loading requires native Apple Silicon macOS')
    files = (*PATCHES, SOURCE, FIXTURE, ROOT / 'terminal/session-state/full-surface/probe.m',
             *(ROOT / 'terminal/session-state' / name for name in ('resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-surface-loading-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work); binary = build(engine, work); FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                mutations(engine, work); print('PASS 4 native loading mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: frame loading/animation/pending, atomic capture, budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'full loading probe failed: {error}; see tmp/session-loading-surface-build.log', file=sys.stderr); sys.exit(1)
