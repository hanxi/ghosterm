#!/usr/bin/env python3
"""Attachment revocation/task ownership; isolated mutation copies only."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'core/src/session/attachment.c'
original=SOURCE.read_bytes();sha=hashlib.sha256(original).digest()
MARK='MUTATION_ATTACHMENT'
if MARK.encode() in original: raise SystemExit(2)

def interrupted(signum,frame):
    raise KeyboardInterrupt
for sig in (signal.SIGINT,signal.SIGTERM): signal.signal(sig,interrupted)
(ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
subprocess.run(['bash',str(ROOT/'scripts/build-core.sh')],check=True,stdout=subprocess.DEVNULL)
CASES=('nojob','race','borrowed','running','ready','failure','failure-stale','backpressure','invalid','overflow','early-ready','allocator','ended','end-wait','unknown-end')
with tempfile.TemporaryDirectory(prefix='attachment-',dir=ROOT/'tmp') as directory:
    source=Path(directory)/SOURCE.name;binary=Path(directory)/'probe'
    def build(text):
        source.write_text(text)
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror','-pthread',
                        '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),
                        '-I'+str(ROOT/'core/include'),str(source),str(ROOT/'core/tests/test_attachment.c'),
                        str(ROOT/'core/build/libghosterm_core.a'),'-o',str(binary)],check=True)
    def run(mode):
        return subprocess.run([str(binary),mode],stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,timeout=12).returncode
    text=original.decode()
    variants=(
        ('unknown end accepted after capture','a->stream.phase!=GT_RESTORE_CAPTURING', 'false','unknown-end','nojob'),
        ('unknown end admits snapshot',
         'else if(a->session_ended && !a->has_terminal_sequence)rc=GT_RESTORE_PROTOCOL;\n  else if(a->session_ended && s>a->terminal_sequence)',
         'else if(a->session_ended && s>a->terminal_sequence)','unknown-end','nojob'),
        ('unknown end converted to known zero',
         'else if(a->session_ended && !a->has_terminal_sequence)rc=GT_RESTORE_PROTOCOL;\n  else if((a->session_ended',
         'else if((a->session_ended','unknown-end','nojob'),
        ('unknown end permits data',
         'if(a->session_ended && !a->has_terminal_sequence) {pthread_mutex_unlock(&a->lock);return GT_RESTORE_PROTOCOL;}',
         '/* unknown guard omitted */','unknown-end','nojob'),
        ('unknown end not latched','a->session_ended = true;','(void)a;','unknown-end','nojob'),
        ('unknown end invents known zero','a->session_ended = true;', 'a->session_ended = true;a->has_terminal_sequence=true;','unknown-end','nojob'),
        ('stale unknown end admitted','if (!current(a,g))rc=GT_RESTORE_STALE;', 'if (false && !current(a,g))rc=GT_RESTORE_STALE;','unknown-end','nojob'),
        ('known zero downgraded unknown','a->has_terminal_sequence || a->submitted', 'a->submitted','unknown-end','nojob'),
        ('session end not latched','a->session_ended=true;',
         '/* end latch omitted */','end-wait','nojob'),
        ('stale owner ends new session','if(!current(a,g)) {rc=GT_RESTORE_STALE;}',
         'if(false && !current(a,g)) {rc=GT_RESTORE_STALE;}','end-wait','nojob'),
        ('ended session input reopened','a->active && !a->session_ended && gt__restore_input_allowed',
         'a->active && gt__restore_input_allowed','end-wait','nojob'),
        ('post terminal event accepted','  if(a->session_ended && s>a->terminal_sequence)',
         '  if(false && a->session_ended && s>a->terminal_sequence)','ended','nojob'),
        ('terminal sequence rewritten','a->session_ended && s!=a->terminal_sequence',
         'false','ended','nojob'),
        ('reattach forgets readonly','gt__restore_reset(&a->stream,a->generation);',
         'gt__restore_reset(&a->stream,a->generation);a->session_ended=false;','ended','nojob'),
        ('future cached snapshot accepted','else if(a->session_ended && s>a->terminal_sequence)',
         'else if(false && a->session_ended && s>a->terminal_sequence)','ended','nojob'),
        ('controller bypasses allocation domain','a=domain.allocate(domain.context,sizeof(*a))',
         'a=malloc(sizeof(*a))','allocator','nojob'),
        ('ring forgets allocation domain','gt__restore_init_with_allocator(&a->stream,queue_bytes,queue_events,&domain)',
         'gt__restore_init(&a->stream,queue_bytes,queue_events)','allocator','nojob'),
        ('close releases wrong object size','domain.release(domain.context,a,sizeof(*a));return GT_TASK_OK;',
         'domain.release(domain.context,a,sizeof(*a)-1);return GT_TASK_OK;','allocator','nojob'),
        ('failed encoder keeps its attachment',
         'if(info.state==GT_TASK_FAILED && current(a,j->generation))',
         'if(false && info.state==GT_TASK_FAILED && current(a,j->generation))','failure','failure-stale'),
        ('old failed encoder revokes new generation',
         'info.state==GT_TASK_FAILED && current(a,j->generation)',
         'info.state==GT_TASK_FAILED','failure-stale','failure'),
        ('failed stream keeps authorization','if(a->stream.phase==GT_RESTORE_FAILED && a->active)',
         'if(false && a->stream.phase==GT_RESTORE_FAILED && a->active)','overflow','nojob'),
        ('generation not revoked','a->active=false; /* Revoke before requesting asynchronous cleanup. */','/* revocation omitted */','nojob','invalid'),
        ('generation reused','a->generation++;','a->generation=1;','borrowed','nojob'),
        ('second attachment admitted','a->active ? GT_TASK_BUSY :','false ? GT_TASK_BUSY :','nojob','invalid'),
        ('untrusted generation used','bound.generation=generation;','/* caller generation trusted */','borrowed','nojob'),
        ('stale submit admitted','if(current(a,generation)) {\n    rc=a->submitted',
         'if(true) {\n    rc=a->submitted','running','nojob'),
        ('detach leaves job running','rc=j && !j->borrowed ? gt__tasks_cancel(a->pool,j->id) : GT_TASK_OK;',
         '(void)j;rc=GT_TASK_OK;','running','borrowed'),
        ('foreign lease released','j->generation==lease->generation &&','true &&','borrowed','ready'),
        ('old job loses generation','j->id,j->generation,info.state,info.error,j->context',
         'j->id,a->generation,info.state,info.error,j->context','running','ready'),
        ('old reap erases new checkpoint','if(a->current_task==j->id)a->current_task=0;',
         'a->current_task=0;','running','borrowed'),
        ('controller freed before reaping','if(a->jobs[i].id)busy=true;',
         'if(a->jobs[i].id)(void)0;','running','nojob'),
        ('duplicate checkpoint admitted','if(!a->submitted && a->stream.phase==GT_RESTORE_SNAPSHOT && spec->sequence==a->stream.snapshot) for','if(true) for','borrowed','nojob'),
        ('released lease still exposed','memset(lease,0,sizeof(*lease));',
         '/* stale lease retained */','running','borrowed'),
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
print('rollback SHA256 and all attachment baselines green')
