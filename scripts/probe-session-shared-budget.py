#!/usr/bin/env python3
"""Shared POSIX mmap budget and sticky per-owner resource errors; P0 incomplete."""
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
FULL, VT, PATCHES, PROBE = (PREVIOUS[k] for k in ('FULL', 'VT', 'PATCHES', 'PROBE'))
SOURCE = ROOT / 'terminal/session-state/shared-budget-probe.c'
ADAPTER = ROOT / 'terminal/session-state/budget-posix.c'
HEADER = ROOT / 'terminal/session-state/budget-posix.h'
MARK = 'GT_P0_SHARED_BUDGET_MUTATION_MARK'
CASES = ('categories', 'isolation', 'alignment', 'overflow', 'engine', 'restore',
         'feed-failure', 'pixel-failure', 'resize-failure', 'admission', 'scratch-failure', 'concurrent')


def build(engine, work, adapter):
    binary = work / 'shared-budget'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
                 str(SOURCE), str(adapter), str(work / 'out/lib/libghostty-vt.a'),
                 *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work, adapter):
    variants = (
        ('global limit bypassed', 'if(!s->reserved && n>b->limit-b->usage.committed)', 'if(false && !s->reserved && n>b->limit-b->usage.committed)', 'categories', 'alignment'),
        ('scope limit bypassed', 'if(n>s->limit-s->usage.live)', 'if(false && n>s->limit-s->usage.live)', 'isolation', 'categories'),
        ('fault cleared by increased capacity', 'if(s->closed || s->fault!=P0_BUDGET_HEALTHY)', 'if(s->closed)', 'categories', 'engine'),
        ('resource error hidden by success', 'if(fault!=P0_BUDGET_HEALTHY)return GHOSTTY_LIMIT_EXCEEDED;', 'if(false && fault!=P0_BUDGET_HEALTHY)return GHOSTTY_LIMIT_EXCEEDED;', 'pixel-failure', 'engine'),
        ('alignment trim not released', 'release_locked(e,total-needed);', 'release_locked(e,0);', 'alignment', 'categories'),
        ('allocation categories collapsed', '.kind=(p0_budget_kind)k', '.kind=P0_BUDGET_GENERAL', 'categories', 'alignment'),
        ('concurrent admission overcommits', 'if(!s->reserved && n>b->limit-b->usage.committed)', 'if(b->scopes<8 && !s->reserved && n>b->limit-b->usage.committed)', 'concurrent', 'categories'),
        ('feed continues after resource error',
         'if(!data && n)return GHOSTTY_INVALID_VALUE;\n    GhosttyResult rc=terminal_ready(s,t);if(rc!=GHOSTTY_SUCCESS)return rc;',
         'if(!data && n)return GHOSTTY_INVALID_VALUE;', 'pixel-failure', 'engine'),
        ('resize continues after resource error',
         'GhosttyResult rc=terminal_ready(s,t);if(rc!=GHOSTTY_SUCCESS)return rc;\n    rc=ghostty_terminal_resize',
         'GhosttyResult rc=ghostty_terminal_resize', 'resize-failure', 'engine'),
        ('overflow not classified before mmap',
         'if(alignment>=sizeof(size_t)*CHAR_BIT || !round_size(n,b->page_size,&needed))',
         'if((alignment>=sizeof(size_t)*CHAR_BIT || !round_size(n,b->page_size,&needed)) && false)', 'overflow', 'categories'),
        ('live allocator descriptor closed', 'bool empty=!s->closed && s->usage.live==0;', 'bool empty=!s->closed;', 'categories', 'engine'),
    )
    for label, old, new, target, control in variants:
        original = adapter.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique shared budget mutation: {label}')
        try:
            adapter.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n')
            binary = build(engine, work, adapter)
            PROBE(binary, target, 1); PROBE(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            adapter.write_bytes(original)
            if hashlib.sha256(adapter.read_bytes()).digest() != digest: raise RuntimeError('shared budget rollback mismatch')
        binary = build(engine, work, adapter); PROBE(binary, target)
    for mode in CASES: PROBE(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    files = (*PATCHES, SOURCE, ADAPTER, HEADER, *(ROOT / 'terminal/session-state' / name for name in
             ('resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-shared-budget-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-shared-budget-build.log')
            adapter = work / ADAPTER.name; shutil.copyfile(ADAPTER, adapter); shutil.copyfile(HEADER, work / HEADER.name)
            binary = build(engine, work, adapter)
            for mode in (args.case,) if args.case else CASES: PROBE(binary, mode)
            if args.mutations:
                mutations(engine, work, adapter); print('PASS 11 shared budget mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: snapshot scratch allocator, native GUI budgets, atomic capture and production runtime remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'shared budget probe failed: {error}; see tmp/session-shared-budget-build.log', file=sys.stderr); sys.exit(1)
