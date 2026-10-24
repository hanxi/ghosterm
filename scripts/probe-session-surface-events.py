#!/usr/bin/env python3
"""Ordered authority output/resize prototype; default exit 3 (P0 incomplete)."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-relative.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0013-ordered-replica-events.patch')
SOURCE = ROOT / 'terminal/session-state/full-surface'
MARK = 'GT_P0_ORDERED_EVENT_MUTATION_MARK'
CASES = ('resize', 'output', 'mixed', 'reorder', 'raw', 'import-only')


def build(engine, work):
    FULL['build'](engine, work)
    binary = work / 'surface-events'
    frameworks = [x for name in FULL['FRAMEWORKS'] for x in ('-framework', name)]
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-fobjc-arc', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), str(SOURCE / 'event-probe.m'),
                 str(engine / 'macos/GhosttyKit.xcframework/macos-arm64/libghostty-internal.a'),
                 '-lc++', *frameworks, '-o', str(binary)])
    return binary


def fixtures(engine, work):
    VT['build'](engine, work, True)
    binary = work / 'event-fixtures'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror',
                 '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
                 str(SOURCE / 'event-fixture.c'), str(work / 'out/lib/libghostty-vt.a'), '-o', str(binary)])
    FULL['run']([str(binary), str(work)])


def mutations(engine, work):
    termio = 'src/termio/Termio.zig'
    variants = (
        ('resize accepted without application', termio,
         '.resize => |size| self.terminal.resize(self.alloc, .{',
         '.resize => |size| if (false) self.terminal.resize(self.alloc, .{', 'resize', 'output'),
        ('output accepted without application', termio,
         '.output => |bytes| stream.nextSlice(bytes),',
         '.output => |bytes| { _ = bytes; },', 'output', 'resize'),
        ('sequence fence omitted', termio,
         'if (self.replica_sequence == std.math.maxInt(u64) or sequence != self.replica_sequence + 1) return false;',
         'if (false) return false;', 'reorder', 'resize'),
        ('applied sequence not published', termio,
         'self.replica_sequence = sequence;', 'self.replica_sequence += 0;', 'resize', 'import-only'),
        ('unsequenced output accepted', termio,
         'if (self.replica_sequence != 0) {', 'if (false) {', 'raw', 'output'),
        ('input ignores applied mode', 'src/input/paste.zig',
         '.bracketed = t.modes.get(.bracketed_paste),',
         '.bracketed = blk: { _ = t; break :blk false; },', 'output', 'resize'),
        ('invalid resize falsely accepted', 'src/apprt/embedded.zig',
         'if (invalid) return false;', 'if (invalid) return true;', 'reorder', 'output'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path
        original = source.read_bytes()
        text = original.decode()
        digest = hashlib.sha256(original).digest()
        if MARK in text or text.count(old) != 1:
            raise RuntimeError(f'residual marker or nonunique event anchor: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work)
            FULL['probe'](binary, work, target, 1)
            FULL['probe'](binary, work, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest:
                raise RuntimeError('ordered event rollback mismatch')
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
        raise RuntimeError('ordered full surface events require native Apple Silicon macOS')
    files = (*PATCHES, SOURCE / 'event-probe.m', SOURCE / 'event-fixture.c', SOURCE / 'probe.m',
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
        with tempfile.TemporaryDirectory(prefix='session-ordered-events-', dir=ROOT / 'tmp') as tmp:
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
                print('PASS 7 ordered event mutations and rollback; P0 remains incomplete')
                return 0
        print('P0 INCOMPLETE: generation fencing, capture binding, loading/animation and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'ordered event probe failed: {error}; see tmp/session-surface-build.log', file=sys.stderr)
        sys.exit(1)
