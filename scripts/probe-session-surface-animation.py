#!/usr/bin/env python3
"""Native animation checkpoint with authority-only clock; P0 remains incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-animation.py'))
FULL, VT, PATCHES = PREVIOUS['FULL'], PREVIOUS['VT'], PREVIOUS['PATCHES']
PRUNE = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-input.py'))['prune_own_cache']
SOURCE = ROOT / 'terminal/session-state/full-surface/animation-probe.m'
FIXTURE = ROOT / 'terminal/session-state/full-surface/animation-fixture.c'
MARK = 'GT_P0_SURFACE_ANIMATION_MUTATION_MARK'
FIXTURE_CASES = ('running', 'stopped', 'finite', 'loading', 'gapless', 'static', 'screens', 'continuation')
CASES = (*FIXTURE_CASES, 'import-only', 'clock-only')


def fixtures(engine, work, *, build_vt=True):
    if build_vt: VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-animation-fixture-build.log')
    binary = work / 'animation-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(FIXTURE),
                 str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    FULL['run']([str(binary), str(work)])
    for name in FIXTURE_CASES:
        canonical = (work / (name + '-initial.canonical')).read_bytes(); resources = (work / (name + '-initial.resources')).read_bytes()
        data = (b'GHOSTERM-P0-CHECKPOINT-1' + struct.pack('<QQQQ', 81, 40, 1024*1024, len(canonical)) + canonical + struct.pack('<Q', len(resources)) + resources)
        (work / (name + '.digest')).write_bytes(hashlib.sha256(data).digest())


def build(engine, work):
    PRUNE(work)
    FULL['build'](engine, work, cache_dir=work / 'full-cache', log_path=ROOT / 'tmp/session-animation-surface-build.log')
    binary = work / 'surface-animation'; frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'), '-lc++', *frameworks, '-o', str(binary)])
    return binary


def mutations(engine, work):
    variants = (
        ('renderer advances replica clock', 'src/renderer/generic.zig',
         'if (state.terminal.p0_external_animation_clock) break :next null;', 'if (false) break :next null;', 'running', 'stopped'),
        ('replica clock policy not set', 'src/termio/Termio.zig',
         'term.p0_external_animation_clock = decoded != null;', 'term.p0_external_animation_clock = false;', 'running', 'stopped'),
        ('tick acknowledged without application', 'src/termio/Termio.zig',
         '.animation_tick => |now_ms| { _ = @import("../terminal/p0_animation_tick.zig").apply(&self.terminal, now_ms) catch return false; },',
         '.animation_tick => |now_ms| { _ = now_ms; },', 'running', 'import-only'),
        ('tick bypasses generation', 'src/apprt/embedded.zig',
         'replicaEventFor(generation, sequence, .{ .animation_tick = now_ms })',
         'replicaEventFor(if (generation == 0) 0 else surface.core_surface.io.replica_generation, sequence, .{ .animation_tick = now_ms })', 'running', 'clock-only'),
        ('replica clock moves backward', 'src/terminal/p0_animation_tick.zig',
         'if (now_ms < previous) return error.InvalidClock;', 'if (now_ms < previous and false) return error.InvalidClock;', 'running', 'import-only'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique native animation mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            FULL['probe'](binary, work, target, 1); FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('native animation rollback mismatch')
        binary = build(engine, work); FULL['probe'](binary, work, target)
    FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64': raise RuntimeError('native animation requires Apple Silicon macOS')
    files = (*PATCHES, SOURCE, FIXTURE, ROOT / 'terminal/session-state/full-surface/probe.m',
             *(ROOT / 'terminal/session-state' / name for name in ('resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-surface-animation-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work); binary = build(engine, work); FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                mutations(engine, work); print('PASS 5 native animation mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: frame loading/pending, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'native animation probe failed: {error}; see tmp/session-animation-surface-build.log', file=sys.stderr); sys.exit(1)
