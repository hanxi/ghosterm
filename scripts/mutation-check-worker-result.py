#!/usr/bin/env python3
"""Durable worker terminal results through real PTY fault adapters."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import sys
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/session/worker.c'
TEST=ROOT/'core/tests/test_worker_result.c'
MARK='GT_MUTATION_WORKER_RESULT'
CASES=('state-open','backend-open','read','input','resize','apply','reply','capture','capture-null','stop','eof-known','eof-unknown','eof-signal','eof-unavailable','eof-invalid','eof-zero','invalid')
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    pty=ROOT/'core/src/io/pty_posix.c';pty_original=pty.read_bytes();pty_text=pty_original.decode()
    files=(SOURCE,TEST,ROOT/'core/src/session/worker.h',pty,ROOT/'core/src/io/backend.h')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    subprocess.run(['bash',str(ROOT/'scripts/build-core.sh')],check=True,stdout=subprocess.DEVNULL)
    original=SOURCE.read_bytes();text=original.decode()
    variants=(
        ('reply failure hides applied event','if (w->config.event) w->config.event',
         'if (result==GT_OK && w->config.event) w->config.event','reply','read'),
        ('reply failure hides output','owner.sequence!=previous && c->output',
         'rc==GT_OK && owner.sequence!=previous && c->output','reply','apply'),
        ('failed reply event published twice',
         '  /* Successful apply remains observable even when its reply could not be sent. */',
         '  if (result!=GT_OK && w->config.event) w->config.event(w->config.context,event,owner->sequence);',
         'reply','apply'),
        ('invalid state not announced','c->state_invalidated(state,owner.failure,owner.sequence);',
         '(void)state; /* invalidation omitted */','apply','read'),
        ('invalid state error erased','c->state_invalidated(state,owner.failure,owner.sequence);',
         'c->state_invalidated(state,GT_OK,owner.sequence);','apply','read'),
        ('failed apply counted as valid','c->state_invalidated(state,owner.failure,owner.sequence);',
         'c->state_invalidated(state,owner.failure,owner.sequence+1);','apply','read'),
        ('healthy state invalidated on backend failure','state && owner_ready && owner.failure!=GT_OK && c->state_invalidated',
         'state && owner_ready && rc!=GT_OK && c->state_invalidated','read','stop'),
        ('invalidation delivered after destruction',
         '    c->state_invalidated(state,owner.failure,owner.sequence);\n  if (state) c->close_state(state);',
         '    c->close_state(state);\n  if (state && owner_ready && owner.failure!=GT_OK && c->state_invalidated)c->state_invalidated(state,owner.failure,owner.sequence);',
         'capture-null','state-open'),
        ('zero exit treated unknown','observed.kind==GT_IO_EXIT_CODE && observed.code>=0',
         'observed.kind==GT_IO_EXIT_CODE && observed.code>0','eof-zero','eof-known'),
        ('unsupported observation folded into unknown','termination_status=GT_ERR_UNSUPPORTED;',
         'termination_status=GT_ERR_NOT_FOUND;','eof-unavailable','eof-unknown'),
        ('signal folded into exit code','has_exit_code=observed.kind==GT_IO_EXIT_CODE;',
         'has_exit_code=true;','eof-signal','eof-known'),
        ('invalid observation accepted','} else termination_status=GT_ERR_PROTOCOL;',
         '} else termination_status=GT_ERR_NOT_FOUND;','eof-invalid','eof-unknown'),
        ('error status erased','.status=rc,','.status=GT_OK,','input','stop'),
        ('error stage erased','.stage=rc==GT_OK?GT_WORKER_STAGE_NONE:stage',
         '.stage=GT_WORKER_STAGE_NONE','read','stop'),
        ('last applied sequence lost','.sequence=owner_ready?owner.sequence:0',
         '.sequence=0','read','stop'),
        ('initialized state marked unknown','.has_sequence=owner_ready',
         '.has_sequence=false','backend-open','state-open'),
        ('EOF folded into stop','.kind=rc==GT_OK?end:GT_WORKER_END_ERROR',
         '.kind=((void)end,GT_WORKER_END_STOPPED)','eof-known','stop'),
        ('unknown exit invented','.has_exit_code=has_exit_code',
         '.has_exit_code=has_exit_code || end==GT_WORKER_END_EOF','eof-unknown','eof-known'),
        ('known exit discarded','has_exit_code=observed.kind==GT_IO_EXIT_CODE;',
         'has_exit_code=false;','eof-known','eof-unknown'),
        ('result not ready before callback','w->result_ready=true;',
         'w->result_ready=false;','read','invalid'),
        ('pending reported ready','return ready?GT_WORKER_RESULT_READY:GT_WORKER_RESULT_PENDING;',
         'return ((void)ready,GT_WORKER_RESULT_READY);','read','invalid'),
        ('local capture failure ends session','result != GT_OK && cmd.kind != GT_WORKER_CAPTURE',
         'result != GT_OK','capture','capture-null'),
        ('fatal capture ignored','if (owner.failure != GT_OK) { rc = owner.failure; break; }',
         'if (false && owner.failure != GT_OK) { rc = owner.failure; break; }','capture-null','capture'),
    )
    with tempfile.TemporaryDirectory(prefix='worker-result-',dir=ROOT/'tmp') as directory:
        work=Path(directory);source=work/SOURCE.name;pty_copy=work/pty.name;binary=work/'probe'
        def build(content,pty_content=pty_text):
            source.write_text(content);pty_copy.write_text(pty_content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-D_DARWIN_C_SOURCE' if sys.platform=='darwin' else '-D_GNU_SOURCE','-Wall','-Wextra','-Werror','-pthread',
                '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),'-I'+str(ROOT/'core/src/io'),'-I'+str(ROOT/'core/include'),
                str(source),str(pty_copy),str(TEST),str(ROOT/'core/build/libghosterm_core.a'),'-o',str(binary)],check=True)
        def run(case):
            try:return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=8).returncode
            except subprocess.TimeoutExpired:return 'timeout'
        try:
            build(text)
            for case in CASES:assert run(case)==0,case
            for label,old,new,target,control in variants:
                assert text.count(old)==1,label
                build(text.replace(old,new)+f'\n/* {MARK} */\n')
                assert run(target)!=0,label+': falsely green'
                assert run(control)==0,label+': control failed'
                print(label+': target red, control green',flush=True)
            pty_variants=(
                ('signal reported as normal exit','WIFEXITED(status)?GT_IO_EXIT_CODE:GT_IO_EXIT_SIGNAL',
                 'GT_IO_EXIT_CODE','eof-signal','eof-known'),
                ('signal number legacy encoded','WIFEXITED(status)?WEXITSTATUS(status):WTERMSIG(status)',
                 'WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status)','eof-signal','eof-known'),
                ('PTY observation unavailable','if(!atomic_load(&t->exited))return GT_ERR_NOT_FOUND;',
                 'if(true)return GT_ERR_UNSUPPORTED;','eof-known','eof-unknown'),
            )
            for label,old,new,target,control in pty_variants:
                assert pty_text.count(old)==1,label
                build(text,pty_text.replace(old,new)+f'\n/* {MARK} */\n')
                assert run(target)!=0,label+': falsely green'
                assert run(control)==0,label+': control failed'
                print(label+': target red, control green',flush=True)
            build(text)
            for case in CASES:assert run(case)==0,case
        finally:
            source.write_bytes(original);pty_copy.write_bytes(pty_original)
            assert hashlib.sha256(pty_copy.read_bytes()).digest()==hashes[pty]
            assert hashlib.sha256(source.read_bytes()).digest()==hashes[SOURCE]
            for path,digest in hashes.items():assert hashlib.sha256(path.read_bytes()).digest()==digest,path
    print(f'PASS {len(CASES)} worker-result baselines, {len(variants)+len(pty_variants)} mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
