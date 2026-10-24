#!/usr/bin/env python3
"""Authority clear-screen state and sole prompt-repaint owner; P0 incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-reset.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0017-authority-clear-screen.patch')
SOURCE = ROOT / 'terminal/session-state/full-surface/clear-probe.m'
FIXTURE = ROOT / 'terminal/session-state/full-surface/clear-fixture.c'
MARK = 'GT_P0_AUTHORITY_CLEAR_MUTATION_MARK'
CASES = ('normal', 'prompt', 'alternate', 'keep-history', 'declined', 'missing', 'stale', 'mismatch', 'legacy', 'legacy-prompt')


def build(engine, work):
    PREVIOUS['PREVIOUS']['prune_own_cache'](work)
    FULL['build'](engine, work, cache_dir=work / 'full-cache', log_path=ROOT / 'tmp/session-clear-build.log')
    binary = work / 'surface-clear'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def authority(engine, work, dest, expected=0):
    VT['build'](engine, work, True)
    binary = work / 'clear-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'),
                 '-I', str(ROOT / 'terminal/session-state'), str(FIXTURE),
                 str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    dest.mkdir(exist_ok=True)
    result = subprocess.run([str(binary), str(dest)], capture_output=True, text=True, timeout=15)
    sys.stdout.write(result.stdout); sys.stderr.write(result.stderr)
    if result.returncode != expected:
        raise RuntimeError(f'clear authority: expected {expected}, got {result.returncode}')
    if expected and 'replies.calls==' not in result.stderr:
        raise RuntimeError('expected the specific authority FF count assertion')


def fixtures(engine, work):
    authority(engine, work, work)
    for name in ('normal', 'prompt', 'alternate', 'keep-history'):
        canonical = (work / (name + '-initial.canonical')).read_bytes()
        resources = (work / (name + '-initial.resources')).read_bytes()
        data = (b'GHOSTERM-P0-CHECKPOINT-1' + struct.pack('<QQQQ', 81, 40, 1024*1024, len(canonical)) +
                canonical + struct.pack('<Q', len(resources)) + resources)
        (work / (name + '.digest')).write_bytes(hashlib.sha256(data).digest())


def mutations(engine, work):
    variants = (
        ('UI clears before authority event', 'src/Surface.zig',
         'if (self.io.replica_generation != 0) return try self.requestReplicaAction(2);',
         'if (false) return try self.requestReplicaAction(2);', 'normal', 'legacy'),
        ('clear accepted without application', 'src/termio/Termio.zig',
         'const observed = @import("../terminal/p0_host_clear.zig").apply(&self.terminal, action.history);',
         'const observed: @import("../terminal/p0_host_clear.zig").Outcome = .cleared;', 'normal', 'missing'),
        ('history clear lost', 'src/terminal/p0_host_clear.zig',
         'if (history) t.eraseDisplay(.scrollback, false);', 'if (history) {}', 'normal', 'keep-history'),
        ('image deletion lost', 'src/terminal/p0_host_clear.zig',
         'if (comptime options.kitty_graphics) t.screens.active.kitty_images.delete(',
         'if (false) t.screens.active.kitty_images.delete(', 'normal', 'prompt'),
        ('replica duplicates prompt FF', 'src/termio/Termio.zig',
         'if (@intFromEnum(observed) != action.outcome) {',
         'if (observed == .redraw) self.backend.host.callbacks.write(self.backend.host.callbacks.userdata, &.{0x0c}, 1);\n            if (@intFromEnum(observed) != action.outcome) {', 'prompt', 'normal'),
        ('clear outcome divergence hidden', 'src/termio/Termio.zig',
         'if (@intFromEnum(observed) != action.outcome) {',
         'if (@intFromEnum(observed) != action.outcome and false) {', 'mismatch', 'missing'),
        ('clear bypasses generation', 'src/apprt/embedded.zig',
         'replicaEventFor(generation, sequence, .{ .clear_screen =',
         'replicaEventFor(if (generation == 0) 0 else surface.core_surface.io.replica_generation, sequence, .{ .clear_screen =', 'stale', 'missing'),
        ('active erase skips shared history page', 'src/terminal/PageList.zig',
         'self.eraseRows(.{ .active = .{} }, .{ .active = .{ .y = y } });',
         'if (self.getTopLeft(.active).y != 0) return; self.eraseRows(.{ .active = .{} }, .{ .active = .{ .y = y } });', 'keep-history', 'normal'),
        ('clear action confused with reset', 'src/Surface.zig',
         'return try self.requestReplicaAction(2);', 'return try self.requestReplicaAction(1);', 'normal', 'legacy'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes(); text = original.decode(); digest = hashlib.sha256(original).digest()
        if MARK in text or text.count(old) != 1:
            raise RuntimeError(f'residual marker or nonunique clear anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work)
            FULL['probe'](binary, work, target, 1); FULL['probe'](binary, work, control)
            if path == 'src/terminal/PageList.zig':
                PREVIOUS['PREVIOUS']['unit_check'](engine, work, 'eraseActive preserves history prefix', 1, 'TestExpectedEqual')
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('authority clear rollback mismatch')
        binary = build(engine, work); FULL['probe'](binary, work, target)
    source = engine / 'src/terminal/c/terminal.zig'
    original = source.read_bytes(); text = original.decode(); digest = hashlib.sha256(original).digest()
    old = 'if (observed == .redraw) Effects.writePtyTrampoline(&wrapper.stream.handler, &.{0x0c});'
    if MARK in text or text.count(old) != 1:
        raise RuntimeError('nonunique authority FF anchor')
    try:
        source.write_text(text.replace(old, 'if (false) Effects.writePtyTrampoline(&wrapper.stream.handler, &.{0x0c});', 1) + f'\n// {MARK}\n')
        authority(engine, work, work / 'owner-negative', 1)
        binary = build(engine, work); FULL['probe'](binary, work, 'normal')
        print('PASS mutation/authority FF omitted: owner red, replica control green', flush=True)
    finally:
        source.write_bytes(original)
        if hashlib.sha256(source.read_bytes()).digest() != digest:
            raise RuntimeError('owner FF rollback mismatch')
    authority(engine, work, work / 'owner-restored')
    binary = build(engine, work); FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64': raise RuntimeError('authority clear requires native Apple Silicon macOS')
    files = (*PATCHES, SOURCE, FIXTURE, ROOT / 'terminal/session-state/full-surface/probe.m', ROOT / 'terminal/session-state/resource-hooks-probe.c', ROOT / 'terminal/session-state/resource-io-probe.c', ROOT / 'terminal/session-state/probe.c')
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-authority-clear-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)]); FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work); PREVIOUS['PREVIOUS']['unit_check'](engine, work, 'eraseActive preserves history prefix'); binary = build(engine, work); FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                mutations(engine, work); print('PASS 10 authority clear mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: worker/IPC, atomic capture, other host actions, loading/animation and budgets remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'authority clear probe failed: {error}; see tmp/session-clear-build.log', file=sys.stderr); sys.exit(1)
