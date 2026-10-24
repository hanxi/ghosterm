#!/usr/bin/env python3
"""Reserved owner growth capacity and checkpoint admission; P0 incomplete."""
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
PREVIOUS = runpy.run_path(str(ROOT / 'scripts/probe-session-scratch-checkpoint.py'))
FULL, VT, PATCHES, PROBE = (PREVIOUS[k] for k in ('FULL', 'VT', 'PATCHES', 'PROBE'))
SOURCE = ROOT / 'terminal/session-state/reserved-budget-probe.c'
MARK = 'GT_P0_RESERVED_BUDGET_MUTATION_MARK'
CASES = ('on-demand', 'headroom', 'admission', 'mixed', 'resize-grant', 'close', 'local-limit', 'engine-growth', 'roles', 'concurrent')


def build(engine, work):
    binary = work / 'reserved-budget'
    FULL['run']([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE', '-DGHOSTERM_P0_BUILTIN_PNG',
                 '-Wall', '-Wextra', '-Werror', '-I', str(engine / 'include'), '-I', str(ROOT / 'terminal/session-state'),
                 str(SOURCE), str(work / 'checkpoint-budget.c'), str(work / 'budget-posix.c'), str(work / 'out/lib/libghostty-vt.a'),
                 *(['-lutil'] if sys.platform.startswith('linux') else []), '-lpthread', '-o', str(binary)])
    return binary


def mutations(engine, work):
    variants = (
        ('unused headroom not admitted', 'budget-posix.c', 'commit_locked(s,limit);', 'commit_locked(s,s->usage.live);', 'admission', 'roles'),
        ('task admission ignores commitments', 'budget-posix.c', 'bool admitted=limit<=b->limit-b->usage.committed;', 'bool admitted=limit<=b->limit-b->usage.live;', 'admission', 'roles'),
        ('owner growth charged twice', 'budget-posix.c', 'if(!s->reserved && n>b->limit-b->usage.committed)', 'if(n>b->limit-b->usage.committed)', 'headroom', 'on-demand'),
        ('ordinary scopes steal reserved capacity', 'budget-posix.c', 'if(!s->reserved && n>b->limit-b->usage.committed)', 'if(!s->reserved && n>b->limit-b->usage.live)', 'mixed', 'roles'),
        ('free returns live owner grant', 'budget-posix.c', 'if(!e->scope->reserved)commit_locked(e->scope,a->committed-n);', 'commit_locked(e->scope,a->committed-n);', 'close', 'admission'),
        ('close leaks owner grant', 'budget-posix.c', 'if(empty) {commit_locked(s,0);s->closed=true;', 'if(empty) {s->closed=true;', 'close', 'on-demand'),
        ('global ceiling ignores grants', 'budget-posix.c', 'bool valid=n>=b->usage.committed;', 'bool valid=n>=b->usage.live;', 'resize-grant', 'roles'),
        ('grant growth overcommits', 'budget-posix.c', 'if(valid && s->reserved && n>s->limit && n-s->limit>b->limit-b->usage.committed)valid=false;', '// grant growth unchecked', 'resize-grant', 'roles'),
        ('grant decrease not released', 'budget-posix.c', 'if(s->reserved)commit_locked(s,n);s->limit=n;', 'if(s->reserved && n>=s->limit)commit_locked(s,n);s->limit=n;', 'resize-grant', 'roles'),
        ('checkpoint source need not be admitted', 'checkpoint-budget.c', '!source->reserved || ', '', 'roles', 'engine-growth'),
        ('checkpoint task need not be admitted', 'checkpoint-budget.c', '!scratch->reserved || ', '', 'roles', 'engine-growth'),
        ('parallel admission overcommits', 'budget-posix.c', 'bool admitted=limit<=b->limit-b->usage.committed;', 'bool admitted=true;', 'concurrent', 'roles'),
    )
    for label, name, old, new, target, control in variants:
        source = work / name; original = source.read_bytes(); digest = hashlib.sha256(original).digest(); text = original.decode()
        if MARK in text or text.count(old) != 1: raise RuntimeError(f'nonunique reservation mutation: {label}')
        try:
            source.write_text(text.replace(old, new, 1) + f'\n// {MARK}\n'); binary = build(engine, work)
            PROBE(binary, target, 1); PROBE(binary, control)
            print(f'PASS mutation/{label}: target red, control green', flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest() != digest: raise RuntimeError('reservation rollback mismatch')
        binary = build(engine, work); PROBE(binary, target)
    for mode in CASES: PROBE(binary, mode)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=CASES); parser.add_argument('--mutations', action='store_true'); args = parser.parse_args()
    if args.case and args.mutations: parser.error('--case and --mutations are mutually exclusive')
    names = ('budget-posix.c', 'budget-posix.h', 'checkpoint-budget.c', 'checkpoint-budget.h')
    files = (*PATCHES, SOURCE, *(ROOT / 'terminal/session-state' / n for n in (*names,
             'resource-snapshot-probe.c', 'resource-hooks-probe.c', 'resource-io-probe.c', 'probe.c')))
    hashes = {p: hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files): return 2
    (ROOT / 'tmp').mkdir(exist_ok=True); os.environ['TMPDIR'] = str(ROOT / 'tmp')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, FULL['interrupted'])
    try:
        runpy.run_path(str(ROOT / 'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-reserved-budget-', dir=ROOT / 'tmp') as tmp:
            work = Path(tmp); engine = FULL['prepare'](work)
            for patch in PATCHES:
                FULL['run'](['git', '-C', str(engine), 'apply', '--check', str(patch)])
                FULL['run'](['git', '-C', str(engine), 'apply', str(patch)])
            VT['build'](engine, work, True, log_path=ROOT / 'tmp/session-reserved-budget-build.log')
            for name in names: shutil.copyfile(ROOT / 'terminal/session-state' / name, work / name)
            binary = build(engine, work)
            for mode in (args.case,) if args.case else CASES: PROBE(binary, mode)
            if args.mutations:
                mutations(engine, work); print('PASS 12 reserved budget mutations and rollback; P0 remains incomplete'); return 0
        print('P0 INCOMPLETE: atomic capture, native GUI budgets, production runtime and performance remain')
        return 0 if args.case else 3
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).digest() != digest: raise RuntimeError(f'repository source changed: {path}')


if __name__ == '__main__':
    try: sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError, InterruptedError) as error:
        print(f'reservation probe failed: {error}; see tmp/session-reserved-budget-build.log', file=sys.stderr); sys.exit(1)
