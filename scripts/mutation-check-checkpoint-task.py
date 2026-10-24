#!/usr/bin/env python3
"""Checkpoint task cancellation/ownership mutations, isolated under root tmp/."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'core/src/session/checkpoint-task.c'
original=SOURCE.read_bytes()
sha=hashlib.sha256(original).digest()
MARK='MUTATION_CHECKPOINT_TASK'
if MARK.encode() in original: raise SystemExit(2)

def interrupted(signum,frame):
    raise KeyboardInterrupt

for sig in (signal.SIGINT,signal.SIGTERM): signal.signal(sig,interrupted)
(ROOT/'tmp').mkdir(exist_ok=True)
os.environ['TMPDIR']=str(ROOT/'tmp')
CASES=('success','cancel-running','cancel-queued','cancel-ready','failure','empty','close','invalid','allocator')
with tempfile.TemporaryDirectory(prefix='checkpoint-task-',dir=ROOT/'tmp') as directory:
    source=Path(directory)/SOURCE.name
    binary=Path(directory)/'probe'
    def build(text):
        source.write_text(text)
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-pthread',
                        '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),
                        '-I'+str(ROOT/'core/include'),str(source),
                        str(ROOT/'core/tests/test_checkpoint_task.c'),'-o',str(binary)],check=True)
    def run(mode):
        return subprocess.run([str(binary),mode],stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,timeout=10).returncode
    text=original.decode()
    variants=(
        ('pool bypasses domain','pool=a.allocate(a.context,sizeof(*pool))',
         'pool=malloc(sizeof(*pool))','allocator','invalid'),
        ('pool close wrong size','a.release(a.context,pool,sizeof(*pool));return GT_TASK_OK;',
         'a.release(a.context,pool,sizeof(*pool)-1);return GT_TASK_OK;','allocator','invalid'),
        ('input freed during cancel','cancel_locked(t);pthread_cond_signal(&pool->changed);',
         'if(t->input_owned)t->spec.release_input(t->spec.context); cancel_locked(t);pthread_cond_signal(&pool->changed);',
         'cancel-running','failure'),
        ('running cancellation ignored','bool discard=t->cancel || releasing || error!=GT_OK;',
         'bool discard=releasing || error!=GT_OK;','cancel-running','failure'),
        ('input freed too late or omitted','t->spec.release_input(t->spec.context);',
         '/* input cleanup omitted */','failure','invalid'),
        ('result leaked','if (result) t->spec.release_result(t->spec.context,result);',
         '(void)result;','failure','empty'),
        ('generation ignored','if (!pool || !result || !sequence) return GT_TASK_INVALID;',
         'if (!pool || !result || !sequence) return GT_TASK_INVALID; generation=7;','success','failure'),
        ('capacity bypassed','if (!pool->slots[i].id) {','if (!pool->slots[i].id || i==0) {','cancel-queued','invalid'),
        ('forget running task','!terminal(t->state) ? GT_TASK_BUSY','false ? GT_TASK_BUSY','cancel-running','failure'),
        ('null success published','if (error==GT_OK && !result) error=GT_ERR_INTERNAL;',
         '/* null success accepted */','empty','failure'),
        ('cancelled queue encoded','bool encode=t->state==GT_TASK_QUEUED;',
         'bool encode=t->input_owned;','cancel-queued','success'),
        ('taken result published twice','t->state=GT_TASK_TAKEN;rc=GT_TASK_OK;',
         't->state=GT_TASK_READY;rc=GT_TASK_OK;','success','failure'),
        ('borrowed result destroyed on close','pool->slots[i].state==GT_TASK_TAKEN) {',
         'pool->slots[i].state==GT_TASK_TAKEN && false) {','success','failure'),
        ('task ID reused','pool->next_id++','pool->next_id','success','failure'),
    )
    try:
        build(text)
        for mode in CASES: assert run(mode)==0,mode
        for name,before,after,target,control in variants:
            assert text.count(before)==1,name
            build(text.replace(before,after)+f'\n/* {MARK} */\n')
            assert run(target)!=0,name+': target falsely green'
            assert run(control)==0,name+': control failed'
            print(name+': target red, control green',flush=True)
        build(text)
        for mode in CASES: assert run(mode)==0,mode
    finally:
        source.write_bytes(original)
        assert hashlib.sha256(source.read_bytes()).digest()==sha
        assert hashlib.sha256(SOURCE.read_bytes()).digest()==sha
print('rollback SHA256 and all task baselines green')
