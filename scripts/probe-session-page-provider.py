#!/usr/bin/env python3
"""Explicit native page allocator and budget admission; P0 remains incomplete."""
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
UNIT = runpy.run_path(str(ROOT / 'scripts/probe-session-surface-input.py'))['unit_check']
PATCH = ROOT / 'patches/libghostty/8159838/experimental/0024-page-allocation-provider.patch'
PATCHES = (*PREVIOUS['PATCHES'], PATCH)
SOURCE = ROOT / 'terminal/session-state/page-provider-probe.c'
MARK = 'GT_P0_PAGE_PROVIDER_MUTATION_MARK'
CASES = ('create', 'create-oom', 'quota', 'restore', 'restore-oom', 'alternate', 'restored-alternate', 'default')


def build(engine, work):
    VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-page-provider-build.log')
    binary = work / 'page-provider'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'), str(SOURCE),
                 str(work / 'out/lib/libghostty-vt.a'), *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work):
    pages = 'src/terminal/PageList.zig'
    variants = (
        ('initial page policy bypassed', pages, 'opts.p0_page_allocator orelse pageAllocator(alloc),', 'pageAllocator(alloc),', 'create-oom', 'default'),
        ('restored page policy bypassed', pages, 'options.p0_page_allocator orelse pageAllocator(alloc),', 'pageAllocator(alloc),', 'restore', 'create'),
        ('alternate page policy bypassed', 'src/terminal/Terminal.zig',
         '// screen if we have to initialize.\n                .p0_page_allocator = primary.pages.p0_page_allocator,',
         '// screen if we have to initialize.\n                .p0_page_allocator = null,', 'restored-alternate', 'restore'),
        ('decoder policy ignored', 'src/terminal/c/snapshot.zig', '.p0_page_allocator = decoder.p0_page_allocator,', '.p0_page_allocator = null,', 'restore', 'create'),
        ('decoder policy changed after ready', 'src/terminal/c/snapshot.zig',
         'switch (decoder.state) { .configuring => {}, else => return .invalid_value }',
         'switch (decoder.state) { .configuring => {}, else => {} }', 'restored-alternate', 'create'),
        ('clone page policy bypassed', pages, 'self.p0_page_allocator orelse pageAllocator(alloc),', 'pageAllocator(alloc),', 'clone-unit', 'default'),
    )
    for label, path, old, new, target, control in variants:
        source = engine / path; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique page provider mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            if target == 'clone-unit': UNIT(engine, work, 'P0 page provider clone', 1, 'TestExpectedError')
            else: PROBE(binary, target, 1)
            PROBE(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('page provider rollback mismatch')
        binary = build(engine, work)
        if target == 'clone-unit': UNIT(engine, work, 'P0 page provider clone')
        else: PROBE(binary, target)
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
        with tempfile.TemporaryDirectory(prefix='session-page-provider-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PROBE(binary, mode)
            if args.mutations:
                UNIT(engine, work, 'P0 page provider clone')
                mutations(engine, work); print('PASS 6 page provider mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: full surface budget integration, atomic capture, total budgets and production recovery remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'pending screen probe failed: {error}; see tmp/session-page-provider-build.log', file=sys.stderr); sys.exit(1)
