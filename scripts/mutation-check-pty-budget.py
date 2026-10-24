#!/usr/bin/env python3
"""Real PTY allocation domain, transactional backlog and startup failure."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/io/pty_posix.c'
TEST=ROOT/'core/tests/test_pty_budget.c'
MARK='GT_MUTATION_PTY_BUDGET'
CASES=('flow','failures','pressure','invalid','thread')
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    files=(SOURCE,TEST,ROOT/'core/src/io/backend.h',ROOT/'core/tests/allocation-test.h')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    subprocess.run(['bash',str(ROOT/'scripts/build-core.sh')],check=True,stdout=subprocess.DEVNULL)
    original=SOURCE.read_bytes();text=original.decode()
    variants=[
        ('invalid options fork before rejecting','if(thread_options) {',
         'if(false && thread_options) {','invalid','flow'),
        ('waiter thread replaced with caller','*out=t->waiter;return GT_OK;',
         '(void)t;*out=pthread_self();return GT_OK;','thread','flow'),
        ('non PTY falsely supported','if(backend->ops!=&pty_ops)return GT_ERR_UNSUPPORTED;',
         'if(backend->ops!=&pty_ops)return GT_OK;','invalid','flow'),
        ('allocation domain bypassed','h=a->allocate(a->context,sizeof(*h)+bytes)',
         'h=malloc(sizeof(*h)+bytes)','failures','invalid'),
        ('release size wrong','a.release(a.context,h,sizeof(*h)+h->meta.bytes);',
         'a.release(a.context,h,sizeof(*h)+h->meta.bytes-1);','failures','invalid'),
        ('old backlog content discarded','memcpy(next,p,h->meta.bytes<bytes?h->meta.bytes:bytes);',
         'memset(next,0,h->meta.bytes<bytes?h->meta.bytes:bytes);','flow','invalid'),
        ('allocation failure destroys old backlog','void *next=pty_allocate(a,bytes);if(!next)return NULL;',
         'void *next=pty_allocate(a,bytes);if(!next){pty_free(p);return NULL;}','pressure','invalid'),
        ('backlog cap ignored','len > GT_PTY_WTAIL_MAX - t->wtail_len',
         'false','pressure','flow'),
        ('parent environment array leaked','  pty_free(env);\n  pty_free(term_kv);',
         '  /* env leaked */\n  pty_free(term_kv);','failures','invalid'),
        ('backend object leaked on close','  pty_free(t->wtail);\n  pty_free(t);',
         '  pty_free(t->wtail); /* object leaked */','failures','invalid'),
    ]
    if sys.platform=='darwin':
        variants.append(('waiter stack options omitted','gt__thread_create(&t->waiter,pty_waiter,t,thread_options)',
                         'gt__thread_create(&t->waiter,pty_waiter,t,((void)thread_options,NULL))','thread','flow'))
        variants.append(('login OOM silently falls back','if(is_shell && !dash0)',
                         'if(false && is_shell && !dash0)','failures','flow'))
    with tempfile.TemporaryDirectory(prefix='pty-budget-',dir=ROOT/'tmp') as directory:
        work=Path(directory);source=work/SOURCE.name;binary=work/'probe'
        def build(content):
            source.write_text(content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',
                '-D_DARWIN_C_SOURCE' if sys.platform=='darwin' else '-D_GNU_SOURCE',
                '-Wall','-Wextra','-Werror','-pthread','-I'+str(ROOT/'core/src'),
                '-I'+str(ROOT/'core/src/io'),'-I'+str(ROOT/'core/include'),str(source),str(TEST),
                str(ROOT/'core/build/libghosterm_core.a'),'-o',str(binary)],check=True)
        def run(case):return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15).returncode
        try:
            build(text)
            for case in CASES:assert run(case)==0,case
            for label,old,new,target,control in variants:
                assert text.count(old)==1,label
                build(text.replace(old,new)+f'\n/* {MARK} */\n')
                assert run(target)!=0,label+': falsely green'
                assert run(control)==0,label+': control failed'
                print(label+': target red, control green',flush=True)
            build(text)
            for case in CASES:assert run(case)==0,case
        finally:
            source.write_bytes(original)
            assert hashlib.sha256(source.read_bytes()).digest()==hashes[SOURCE]
            for path,digest in hashes.items():assert hashlib.sha256(path.read_bytes()).digest()==digest,path
    print(f'PASS {len(CASES)} PTY-budget baselines, {len(variants)} mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
