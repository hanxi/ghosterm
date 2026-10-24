#!/usr/bin/env python3
"""Pending pixels and late authority producer results; P0 remains incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-frame-loading.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0022-pending-resource-checkpoint.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/pending-resource-probe.c'
MARK = 'GT_P0_PENDING_MUTATION_MARK'
CASES = ('ready', 'rgb', 'number', 'stale', 'replaced-pending', 'deleted', 'cancel', 'async', 'async-cancel', 'frame', 'frame-number', 'frame-early', 'wrong-length', 'zero-token', 'limit', 'static')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-pending-build.log')
    binary = work / 'pending'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work):
    pending = 'src/terminal/p0_pending_image.zig'; codec = 'src/terminal/p0_resource_snapshot.zig'
    variants = (
        ('origin not preserved', codec, 'img.p0_pending_origin = token;', 'img.p0_pending_origin = 0;', 'ready', 'static'),
        ('stale producer accepted', pending, 'origin(img) != token) return false;', 'false) return false;', 'replaced-pending', 'ready'),
        ('cancel ignored', pending, 'storage.delete(t.io(), t.gpa(), t, .{ .id = .{ .image_id = id, .delete = true } });', '// cancellation lost', 'cancel', 'ready'),
        ('ready ignored', pending, 'const data = try t.gpa().dupe(u8, bytes);', 'if (bytes.len > 0) return false; const data = try t.gpa().dupe(u8, bytes);', 'async', 'cancel'),
        ('length check removed', pending, 'if (bytes.len != img.data.pending) return error.InvalidData;', '// length not checked', 'wrong-length', 'ready'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != (2 if label == 'stale producer accepted' else 1): raise RuntimeError(f'nonunique frame mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            PREVIOUS['PREVIOUS']['probe'](binary, target, 1); PREVIOUS['PREVIOUS']['probe'](binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('pending resource rollback mismatch')
        binary = build(engine, work); PREVIOUS['PREVIOUS']['probe'](binary, target)
    for mode in CASES: PREVIOUS['PREVIOUS']['probe'](binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in ('frame-loading-resource-probe.c', 'animation-resource-probe.c', 'resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-pending-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PREVIOUS['PREVIOUS']['probe'](binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 5 pending resource mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: native pending events, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'pending resource probe failed: {error}; see tmp/session-pending-build.log', file=sys.stderr); sys.exit(1)
