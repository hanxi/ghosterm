#!/usr/bin/env python3
"""Native frame chunk continuation with valid/stale reference fencing; P0 incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-frame-loading.py'))
FULL, VT, PATCHES = PREVIOUS['FULL'], PREVIOUS['VT'], PREVIOUS['PATCHES']
PRUNE = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-input.py'))['prune_own_cache']
SOURCE = ROOT / 'terminal/session-state/full-surface/frame-loading-probe.m'
FIXTURE = ROOT / 'terminal/session-state/full-surface/frame-loading-fixture.c'
MARK = 'GT_P0_SURFACE_FRAME_LOADING_MUTATION_MARK'
CASES = ('append', 'number', 'edit', 'stale', 'tick-after', 'missing', 'blend', 'overwrite', 'background', 'png', 'zlib', 'quiet', 'continuation', 'negative-gap', 'static')


def fixtures(engine, work, *, build_vt=True):
    if build_vt: VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-frame-fixture-build.log')
    binary = work / 'frame-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(FIXTURE),
                 str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    FULL['run']([str(binary), str(work)])
    for name in CASES:
        canonical = (work / (name + '-initial.canonical')).read_bytes(); resources = (work / (name + '-initial.resources')).read_bytes()
        limit, steps = struct.unpack('<QQ', (work / (name + '-manifest.bin')).read_bytes())
        if not 1 <= steps <= 5: raise RuntimeError('invalid frame fixture step count')
        data = (b'GHOSTERM-P0-CHECKPOINT-1' + struct.pack('<QQQQ', 81, 40, limit, len(canonical)) + canonical + struct.pack('<Q', len(resources)) + resources)
        (work / (name + '.digest')).write_bytes(hashlib.sha256(data).digest())


def build(engine, work):
    PRUNE(work)
    FULL['build'](engine, work, cache_dir=work / 'full-cache', log_path=ROOT / 'tmp/session-frame-surface-build.log')
    binary = work / 'surface-frame'; frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'), '-lc++', *frameworks, '-o', str(binary)])
    return binary


def mutations(engine, work):
    module = 'src/terminal/p0_loading_snapshot.zig'
    variants = (
        ('frame generation not rebound', module, 'loading.frame.?.image_generation = image.generation;',
         'loading.frame.?.image_generation = image.generation - image.generation;', 'append', 'stale'),
        ('stale frame revived', module, 'if (loading.frame != null and view.matched) {\n        const target',
         'if (loading.frame != null) {\n        const target', 'stale', 'append'),
        ('captured reference marked stale', module, 'context.image_generation == image.generation else false',
         'context.image_generation == image.generation and false else false', 'append', 'missing'),
        ('number frame rejected', module,
         'if (loading.image.id == 0 and (loading.frame == null or loading.image.number == 0)) return error.InvalidData;',
         'if (loading.image.id == 0) return error.InvalidData;', 'number', 'append'),
        ('composition lost', 'src/terminal/p0_frame_loading_snapshot.zig',
         'f.composition_mode = try enumeration(command.CompositionMode, r);', '_ = try enumeration(command.CompositionMode, r);', 'overwrite', 'blend'),
        ('frame completion leaks host input', 'src/termio/Termio.zig', '.output => |bytes| stream.nextSlice(bytes),',
         '.output => |bytes| { stream.nextSlice(bytes); if (std.mem.startsWith(u8, bytes, "\\x1b_Gm=0;")) self.backend.host.callbacks.write(self.backend.host.callbacks.userdata, &.{0x0c}, 1); },', 'append', 'static'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique native frame mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            FULL['probe'](binary, work, target, 1); FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('native frame rollback mismatch')
        binary = build(engine, work); FULL['probe'](binary, work, target)
    FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64': raise RuntimeError('native frame requires Apple Silicon macOS')
    files = (*PATCHES, SOURCE, FIXTURE, ROOT / 'terminal/session-state/full-surface/probe.m',
             *(ROOT / 'terminal/session-state' / name for name in ('resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-surface-frame-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work); binary = build(engine, work); FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                mutations(engine, work); print('PASS 6 native frame mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: async pending, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'native frame probe failed: {error}; see tmp/session-frame-surface-build.log', file=sys.stderr); sys.exit(1)
