#!/usr/bin/env python3
"""Private explicit stack/guard settings and native Darwin mappings."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/base/thread-options.h'
TEST=ROOT/'core/tests/test_thread_options.c'
MARK='GT_MUTATION_THREAD_OPTIONS'
CASES=('attributes','invalid','live')
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    files=(SOURCE,TEST);hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    original=SOURCE.read_bytes();text=original.decode()
    variants=(
        ('stack setting omitted','rc=pthread_attr_setstacksize(attr,o->stack_size);','rc=0;','attributes','invalid'),
        ('guard disabled','pthread_attr_setguardsize(attr,o->guard_size)','pthread_attr_setguardsize(attr,0)','attributes','invalid'),
        ('native guard disappears','pthread_attr_setguardsize(attr,o->guard_size)','pthread_attr_setguardsize(attr,0)','live','invalid'),
        ('attributes discarded at creation','pthread_create(thread,&attr,run,context)','pthread_create(thread,NULL,run,context)','live','attributes'),
        ('zero guard accepted','o->guard_size>0','true','invalid','attributes'),
        ('combined size overflow accepted','o->stack_size<=SIZE_MAX-o->guard_size','true','invalid','attributes'),
    )
    with tempfile.TemporaryDirectory(prefix='thread-options-',dir=ROOT/'tmp') as directory:
        work=Path(directory);(work/'base').mkdir();source=work/'base/thread-options.h';binary=work/'probe'
        def build(content):
            source.write_text(content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-pthread',
                '-I'+str(work),'-I'+str(ROOT/'core/src'),str(TEST),'-o',str(binary)],check=True)
        def run(case):return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5).returncode
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
    print('PASS 3 thread-options baselines, 6 mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
