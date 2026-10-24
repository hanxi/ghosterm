#!/usr/bin/env python3
"""Pending resource allocation failures and malformed records; P0 remains incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-pending-screen.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PROBE = PREVIOUS['PROBE']
PATCHES = PREVIOUS['PATCHES']
SOURCE = ROOT / 'terminal/session-state/pending-failure-probe.c'
MARK = 'GT_P0_PENDING_FAILURE_MUTATION_MARK'
BAD = ('bad-kind', 'bad-token', 'bad-complete-token', 'bad-length', 'bad-long-length', 'bad-width', 'bad-id', 'bad-format', 'bad-flags', 'bad-total', 'bad-limit', 'bad-animation')
CASES = ('import-oom', 'ready-oom', *BAD, 'control')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-pending-failure-build.log')
    binary = work / 'pending-failure'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work):
    pending = 'src/terminal/p0_pending_image.zig'; codec = 'src/terminal/p0_resource_snapshot.zig'
    variants = (
        ('preflight skipped', codec, 'try preflight(t, data[16..], max_bytes, expected_limit);', '_ = expected_limit; // preflight skipped', BAD, 'ready-oom'),
        ('partial storage leaked', codec, 'defer for (&slots, 0..) |*slot, index| if (slot.*) |*storage| storage.deinit(alloc, t.screens.get(if (index == 0) .primary else .alternate).?);', '// partial slots leaked', ('import-oom',), 'ready-oom'),
        ('ready OOM reported success', pending, 'const data = try t.gpa().dupe(u8, bytes);', 'const data = t.gpa().dupe(u8, bytes) catch return true;', ('ready-oom',), 'import-oom'),
        ('cancel allocates for stale token', pending, 'pub fn cancel(t: *Terminal, id: u32, token: u64) !bool {', 'pub fn cancel(t: *Terminal, id: u32, token: u64) !bool { _ = try t.gpa().dupe(u8, &.{1});', ('ready-oom',), 'import-oom'),
    )
    for label, path, old, new, targets, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique pending failure mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            for target in targets: PROBE(binary, target, 1)
            PROBE(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('pending failure rollback mismatch')
        binary = build(engine, work)
        for target in targets: PROBE(binary, target)
    for mode in CASES: PROBE(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in ('pending-resource-probe.c', 'frame-loading-resource-probe.c', 'animation-resource-probe.c', 'resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-pending-failure-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PROBE(binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 4 pending failure mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: native failure injection, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'pending screen probe failed: {error}; see tmp/session-pending-failure-build.log', file=sys.stderr); sys.exit(1)
