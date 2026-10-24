#!/usr/bin/env python3
"""Private ordered configuration command: isolated mutations under repo tmp/."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/session/worker.c'
TEST=ROOT/'core/tests/test_worker_scheme.c'
MARK='GT_MUTATION_WORKER_SCHEME'
CASES=('flow','unsupported','failure','invalid')
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    files=(SOURCE,TEST,ROOT/'core/src/session/worker.h')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    subprocess.run(['bash',str(ROOT/'scripts/build-core.sh')],check=True,stdout=subprocess.DEVNULL)
    text=SOURCE.read_text()
    variants=(
        ('unsupported adapter accepts configuration','kind==GT_WORKER_COLOR_SCHEME && !w->config.color_scheme_supported','false','unsupported','invalid'),
        ('invalid color value accepted','if(scheme.dark>1)','if(false && scheme.dark>1)','invalid','unsupported'),
        ('configuration written as user input','if (cmd.kind == GT_WORKER_INPUT)', 'if (cmd.kind == GT_WORKER_INPUT || cmd.kind==GT_WORKER_COLOR_SCHEME)','flow','unsupported'),
        ('wrong state event kind','{GT_STATE_COLOR_SCHEME,cmd.payload,cmd.length}','{GT_STATE_RESIZE,cmd.payload,cmd.length}','flow','unsupported'),
        ('configuration value lost','{GT_STATE_COLOR_SCHEME,cmd.payload,cmd.length}','{GT_STATE_COLOR_SCHEME,&(gt_state_color_scheme_t){0},sizeof(gt_state_color_scheme_t)}','flow','unsupported'),
        ('configuration failure acknowledged OK','complete(w, &cmd, result, capture);','complete(w, &cmd,cmd.kind==GT_WORKER_COLOR_SCHEME?GT_OK:result,capture);','failure','flow'),
        ('successful configuration not published','if (w->config.event) w->config.event','if (event->kind!=GT_STATE_COLOR_SCHEME && w->config.event) w->config.event','flow','unsupported'),
        ('queued capture skips configuration','*cmd = w->queue[w->head];','*cmd = w->queue[(w->head+w->count-1)%QUEUE_CAPACITY];','flow','unsupported'),
    )
    with tempfile.TemporaryDirectory(prefix='worker-scheme-',dir=ROOT/'tmp') as directory:
        source=Path(directory)/SOURCE.name;binary=Path(directory)/'probe'
        def build(content):
            source.write_text(content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-pthread',
                '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),'-I'+str(ROOT/'core/include'),str(source),str(TEST),str(ROOT/'core/build/libghosterm_core.a'),'-o',str(binary)],check=True)
        def run(case):return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=10).returncode
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
            source.write_text(text)
            assert hashlib.sha256(source.read_bytes()).digest()==hashes[SOURCE]
            for p,digest in hashes.items():assert hashlib.sha256(p.read_bytes()).digest()==digest,p
    print('PASS 4 worker scheme baselines, 8 mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
