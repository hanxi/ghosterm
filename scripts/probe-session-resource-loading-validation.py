#!/usr/bin/env python3
"""Reject malformed loading records before allocation; P0 remains incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-resource-loading.py'))
FULL, VT = PREVIOUS['FULL'], PREVIOUS['VT']
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0019-loading-id-validation.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/loading-validation-probe.c'
MARK = 'GT_P0_LOADING_VALIDATION_MUTATION_MARK'
CASES = ('zero-id', 'presence', 'format', 'compression', 'flags', 'quiet', 'display', 'wide-field', 'cursor', 'virtual', 'dimension', 'length', 'truncated', 'static')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-loading-validation-build.log')
    binary = work / 'loading-validation'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                 '-DGHOSTERM_P0_BUILTIN_PNG', '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'),
                 '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-o', str(binary)])
    return binary


def mutations(engine, work):
    module = 'src/terminal/p0_loading_snapshot.zig'
    variants = (
        ('zero identity accepted', module, 'if (loading.image.id == 0) return error.InvalidData;',
         'if (false) return error.InvalidData;', 'zero-id', 'presence'),
        ('preflight skipped before existing images', 'src/terminal/p0_resource_snapshot.zig',
         'try preflight(t, data[16..], max_bytes, expected_limit);', '_ = expected_limit; // preflight skipped', 'zero-id', 'static'),
        ('invalid enum replaced by default', module, 'std.enums.fromInt(T, try word(r)) orelse error.InvalidData',
         'std.enums.fromInt(T, try word(r)) orelse @enumFromInt(0)', 'format', 'zero-id'),
        ('reserved flags accepted', module, 'if (flags > 3) return error.InvalidData;',
         'if (false) return error.InvalidData;', 'flags', 'zero-id'),
        ('oversized dimensions accepted', module, 'loading.image.width > 10000',
         'false', 'dimension', 'zero-id'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique loading validation mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            PREVIOUS['probe'](binary, target, 1); PREVIOUS['probe'](binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('loading validation rollback mismatch')
        binary = build(engine, work); PREVIOUS['probe'](binary, target)
    for mode in CASES: PREVIOUS['probe'](binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / name for name in
             ('resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-loading-validation-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PREVIOUS['probe'](binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 5 loading validation mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: animated/pending state, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'loading validation probe failed: {error}; see tmp/session-loading-validation-build.log', file=sys.stderr); sys.exit(1)
