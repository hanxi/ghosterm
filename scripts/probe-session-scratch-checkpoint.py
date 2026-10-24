#!/usr/bin/env python3
"""Bounded scratch checkpoint pair, rollback and source isolation; P0 incomplete."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-page-provider.py'))
FULL, VT, PROBE, UNIT = (PREVIOUS[k] for k in ('FULL', 'VT', 'PROBE', 'UNIT'))
PATCHES = (*PREVIOUS['PATCHES'], ROOT / 'patches/libghostty/8159838/experimental/0025-scratch-checkpoint-encoding.patch')
SOURCE = ROOT / 'terminal/session-state/scratch-checkpoint-probe.c'
ADAPTER = ROOT / 'terminal/session-state/checkpoint-budget.c'
HEADER = ROOT / 'terminal/session-state/checkpoint-budget.h'
MARK = 'GT_P0_SCRATCH_CHECKPOINT_MUTATION_MARK'
CASES = ('ground', 'continuation', 'utf8', 'resources', 'loading', 'compressed',
         'canonical-oom', 'continuation-oom', 'compressed-oom', 'resources-oom', 'pair-oom', 'wire-limit', 'invalid-roles', 'source-failed')
PATTERN = 'P0 SCREEN scratch allocator'


def build(engine, work, adapter, *, engine_changed=True):
    if engine_changed: VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-scratch-checkpoint-build.log')
    binary = work / 'scratch-checkpoint'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
                 str(SOURCE), str(adapter), str(ROOT / 'terminal/session-state/budget-posix.c'), str(work / 'out/lib/libghostty-vt.a'),
                 *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work, adapter):
    variants = (
        ('continuation charged to source', 'src/terminal/c/snapshot.zig', 'const continuation = continuationOwned(terminal, alloc);',
         'const continuation = continuationOwned(terminal, null);', 'continuation', 'ground'),
        ('core encoder charged to source', 'src/terminal/c/snapshot.zig', 'snapshot_core.encode(\n        alloc,\n        &writer.writer,',
         'snapshot_core.encode(\n        native.gpa(),\n        &writer.writer,', 'ground', 'invalid-roles'),
        ('resources charged to source', 'src/terminal/c/terminal.zig',
         'const alloc = lib.alloc.default(provider);\n    const bytes = @import("../p0_resource_snapshot.zig").encode',
         'const alloc = wrapper.terminal.gpa(); _ = provider;\n    const bytes = @import("../p0_resource_snapshot.zig").encode', 'resources', 'invalid-roles'),
        ('compressed history charged to source', 'src/terminal/snapshot/history.zig', 'current.pagePreservingState(destination.scratch.allocator)',
         'current.pagePreservingState(terminal_screen.alloc)', 'compressed', 'ground'),
        ('compressed SCREEN charged to source', 'src/terminal/snapshot/screen.zig', 'current.pagePreservingState(destination.scratch.allocator)',
         'current.pagePreservingState(screen.alloc)', 'screen-unit', 'ground'),
        ('partial pair leaked', None, 'p0_checkpoint_release(&staged);', '// partial pair leaked', 'pair-oom', 'ground'),
        ('canonical wire cap bypassed', None, 'if(staged.canonical_len>max_wire_bytes)', 'if(false && staged.canonical_len>max_wire_bytes)', 'wire-limit', 'ground'),
        ('source and scratch roles folded', None, 'source==scratch', 'false', 'invalid-roles', 'ground'),
        ('global ledger identity ignored', None, 'source->budget!=scratch->budget', 'false', 'invalid-roles', 'ground'),
        ('failed source encoded before check', None, 'if(!source_ready(source,t))return P0_CHECKPOINT_SOURCE_FAILED;', '// source check delayed', 'source-failed', 'ground'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path if path else adapter
        original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        expected = 2 if label == 'failed source encoded before check' else 1
        if MARK in text or text.count(old) != expected: raise RuntimeError(f'nonunique scratch mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work, adapter, engine_changed=path is not None)
            if target == 'screen-unit': UNIT(engine, work, PATTERN, 1, 'OutOfMemory')
            else: PROBE(binary, target, 1)
            PROBE(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('scratch rollback mismatch')
        binary = build(engine, work, adapter, engine_changed=path is not None)
        if target == 'screen-unit': UNIT(engine, work, PATTERN)
        else: PROBE(binary, target)
    for mode in CASES: PROBE(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, ADAPTER, HEADER, *(ROOT / 'terminal/session-state' / name for name in
             ('budget-posix.c', 'budget-posix.h', 'resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-scratch-checkpoint-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            adapter = work / ADAPTER.name; shutil.copyfile(ADAPTER, adapter); shutil.copyfile(HEADER, work / HEADER.name)
            binary = build(engine, work, adapter)
            for mode in (args.case,) if args.case else CASES: PROBE(binary, mode)
            if args.mutations:
                UNIT(engine, work, PATTERN); mutations(engine, work, adapter)
                print('PASS 10 scratch checkpoint mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: atomic capture, native GUI budgets, production runtime and performance remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'scratch checkpoint probe failed: {error}; see tmp/session-scratch-checkpoint-build.log', file=sys.stderr); sys.exit(1)
