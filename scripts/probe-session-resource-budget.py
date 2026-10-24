#!/usr/bin/env python3
"""Demonstrate that raw resource events omit final quota decisions. Default exit 3."""
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
ROOT=Path(__file__).resolve().parents[1]
BASE=runpy.run_path(str(ROOT/'scripts/probe-session-resource-hooks.py'))
SOURCE=ROOT/'terminal/session-state/resource-budget-probe.c'
MARK='GT_P0_RESOURCE_BUDGET_MUTATION_MARK'

def compile_probe(engine,work,source):
    binary=work/'budget-probe'
    BASE['run']([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-D_DEFAULT_SOURCE',
        '-Wall','-Wextra','-Werror','-I',str(engine/'include'),'-I',str(ROOT/'terminal/session-state'),
        str(source),str(work/'out/lib/libghostty-vt.a'),*(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(binary)])
    return binary

def mutations(engine,work):
    original=SOURCE.read_bytes();text=original.decode();source=work/'budget-mutant.c'
    variants=(('source quota rejection lost','aligned_accept?4:3','4','divergent','aligned-accept'),
              ('replica quota difference hidden','divergent?4:source_limit','source_limit','divergent','aligned-accept'),
              ('resolved pixels corrupted','  feed(replica,command);','  event.data[0]^=1;\n  feed(replica,command);','divergent','aligned-reject'))
    for label,old,new,target,control in variants:
        if text.count(old)!=1:raise RuntimeError(f'nonunique budget mutation anchor: {label}')
        try:
            source.write_text(text.replace(old,new,1)+f'\n/* {MARK} */\n')
            binary=compile_probe(engine,work,source);BASE['probe'](binary,work,target,1);BASE['probe'](binary,work,control)
            print(f'PASS mutation/{label}: target red, control green',flush=True)
        finally:
            source.write_bytes(original)
            if hashlib.sha256(source.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('budget fixture rollback mismatch')
        binary=compile_probe(engine,work,source);BASE['probe'](binary,work,target,3)

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mutations',action='store_true');p.add_argument('--case',choices=('divergent','aligned-accept','aligned-reject'));args=p.parse_args()
    if args.mutations and args.case:p.error('--case and --mutations are mutually exclusive')
    files=(SOURCE,ROOT/'terminal/session-state/resource-hooks-probe.c',ROOT/'terminal/session-state/resource-io-probe.c',BASE['PATCH'])
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,BASE['interrupted'])
    try:
        runpy.run_path(str(ROOT/'scripts/probe-session-state.py'))['build_engine']()
        with tempfile.TemporaryDirectory(prefix='session-resource-budget-',dir=ROOT/'tmp') as tmp:
            work=Path(tmp);engine=BASE['prepare'](work);BASE['build'](engine,work)
            binary=compile_probe(engine,work,SOURCE)
            for case in (args.case,) if args.case else ('aligned-accept','aligned-reject','divergent'):
                BASE['probe'](binary,work,case,3 if case=='divergent' else 0)
            if args.mutations:
                mutations(engine,work);print('PASS 3 quota evidence mutations and rollback; final commit replication remains unresolved');return 0
        return 0 if args.case and args.case!='divergent' else 3
    finally:
        for p,digest in hashes.items():
            if hashlib.sha256(p.read_bytes()).digest()!=digest:raise RuntimeError(f'repository source changed: {p}')
if __name__=='__main__':
    try:sys.exit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError,InterruptedError) as error:
        print(f'resource budget probe failed: {error}',file=sys.stderr);sys.exit(1)
