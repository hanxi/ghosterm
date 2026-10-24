#!/usr/bin/env python3
"""Authority-routed UI reset. Default exit 3, P0 remains incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-input.py'))
FULL = PREVIOUS['FULL']
VT = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-hooks.py'))
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0016-authority-reset.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/full-surface/reset-probe.m'
FIXTURE = ROOT / 'terminal/session-state/full-surface/reset-fixture.c'
MARK = 'GT_P0_AUTHORITY_RESET_MUTATION_MARK'
CASES = ('queued', 'inline', 'declined', 'missing', 'stale', 'continuation', 'legacy')


def build(engine, work):
    PREVIOUS['prune_own_cache'](work)
    FULL['build'](engine, work, cache_dir=work / 'full-cache', log_path=ROOT / 'tmp/session-reset-build.log')
    binary = work / 'surface-reset'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def fixtures(engine, work):
    VT['build'](engine, work, True)
    binary = work / 'reset-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'),
                 '-I', str(ROOT / 'terminal/session-state'), str(FIXTURE),
                 str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    FULL['run']([str(binary), str(work)])
    for name in ('reset', 'continuation'):
        canonical = (work / (name + '-initial.canonical')).read_bytes()
        resources = (work / (name + '-initial.resources')).read_bytes()
        data = (b'GHOSTERM-P0-CHECKPOINT-1' + struct.pack('<QQQQ', 81, 40, 1024*1024, len(canonical)) +
                canonical + struct.pack('<Q', len(resources)) + resources)
        (work / (name + '.digest')).write_bytes(hashlib.sha256(data).digest())


def mutations(engine, work):
    variants = (
        ('UI speculatively resets', 'src/Surface.zig',
         'if (self.io.replica_generation != 0) return try self.requestReplicaReset();',
         'if (false) return try self.requestReplicaReset();', 'queued', 'legacy'),
        ('authority reset accepted without application', 'src/termio/Termio.zig',
         '.reset => self.terminal.fullReset(),', '.reset => {},', 'queued', 'missing'),
        ('request uses wrong applied sequence', 'src/Surface.zig',
         'requestHostAction(stamp.generation, stamp.sequence, 1)',
         'requestHostAction(stamp.generation, stamp.sequence + 1, 1)', 'queued', 'legacy'),
        ('missing action callback fakes success', 'src/Surface.zig',
         'requestHostAction(stamp.generation, stamp.sequence, 1) orelse return error.UnsupportedReplicaAction;',
         'requestHostAction(stamp.generation, stamp.sequence, 1) orelse return true;', 'missing', 'queued'),
        ('reset bypasses generation fence', 'src/apprt/embedded.zig',
         'replicaEventFor(generation, sequence, .reset)',
         'replicaEventFor(if (generation == 0) 0 else surface.core_surface.io.replica_generation, sequence, .reset)', 'stale', 'missing'),
        ('reset drops parser continuation', 'src/termio/Termio.zig',
         '.reset => self.terminal.fullReset(),',
         '.reset => { self.terminal.fullReset(); stream.parser.state = .ground; },', 'continuation', 'queued'),
        ('reset request leaks into input stream', 'src/Surface.zig',
         'if (self.io.replica_generation != 0) return try self.requestReplicaReset();',
         'if (self.io.replica_generation != 0) { const accepted = try self.requestReplicaReset(); self.queueIo(.{ .write_stable = "\\x1bc" }, .unlocked); return accepted; }', 'queued', 'legacy'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes()
        text = original.decode()
        digest = hashlib.sha256(original).digest()
        if MARK in text or text.count(old) != 1:
            raise RuntimeError(f'residual marker or nonunique reset anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work)
            FULL['probe'](binary, work, target, 1)
            FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('authority reset rollback mismatch')
        binary = build(engine, work)
        FULL['probe'](binary, work, target)
    FULL['probe'](binary, work)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES)
    parser.add_argument('--mutations', action='store_true')
    args = parser.parse_args()
    if args.case and args.mutations:
        parser.error('--case and --mutations are mutually exclusive')
    if sys.platform != 'darwin' or os.uname().machine != 'arm64':
        raise RuntimeError('authority reset prototype requires native Apple Silicon macOS')
    files = (*PATCHES, SOURCE, FIXTURE, ROOT / 'terminal/session-state/full-surface/probe.m',
             ROOT / 'terminal/session-state/resource-hooks-probe.c',
             ROOT / 'terminal/session-state/resource-io-probe.c', ROOT / 'terminal/session-state/probe.c')
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):
        return 2
    (ROOT / 'tmp').mkdir(exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-authority-reset-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp)
            engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            fixtures(engine, work)
            binary = build(engine, work)
            FULL['probe'](binary, work, args.case or 'all')
            if args.mutations:
                mutations(engine, work)
                print('PASS 7 authority reset mutations and rollback; P0 remains incomplete')
                return 0
        print('P0 INCOMPLETE: other state actions, worker/IPC, atomic capture and loading/animation remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'authority reset probe failed: {error}; see tmp/session-reset-build.log', file=sys.stderr)
        sys.exit(1)
