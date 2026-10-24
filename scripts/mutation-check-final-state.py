#!/usr/bin/env python3
"""Terminal snapshot ownership and publication; ordinary isolated failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/final-state-mutations'
RUNS=WORK/'runs'
MARK='GT_FINAL_STATE_MUTATION_MARK'
NAMES=('session/worker.c','session/worker.h','session/creation.c','session/creation.h','session/runtime.c','session/runtime.h','session/state-owner.c','session/state-owner.h')
PATHS={name:ROOT/'core/src'/name for name in NAMES}
PATHS['runtime-test.c']=ROOT/'core/tests/test_session_runtime.c'
PATHS['unit.c']=ROOT/'core/tests/test_worker_final.c'
MODES={'unit':('flow','zero','failure','allocation','null','startup','backend','invalid-state','stop','disabled'),
       'runtime':('final-retained','final-unavailable','final-publication','final-errors','invalid')}


def run(args,timeout=20):
    p=subprocess.Popen(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:
        out,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:
            try:os.killpg(p.pid,signal.SIGKILL)
            except ProcessLookupError:pass
        p.wait();raise
    return p.returncode,out


def main():
    originals={name:path.read_bytes() for name,path in PATHS.items()}
    hashes={name:hashlib.sha256(data).digest() for name,data in originals.items()}
    if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2
    def interrupted(signum,frame):raise RuntimeError('final-state mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    for path in copies.values():path.parent.mkdir(parents=True,exist_ok=True)
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('final-state SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('final-state dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build(suite):
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',
            '-D_DARWIN_C_SOURCE' if sys.platform=='darwin' else '-D_POSIX_C_SOURCE=200809L',
            '-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/ipc'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['unit.c' if suite=='unit' else 'runtime-test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/suite)])
        if rc:raise RuntimeError('final-state compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('final-state dependency archive changed')
    def probe(suite,mode,reject=False):
        rc,out=run([str(WORK/suite),mode]+([str(RUNS)] if suite=='runtime' else []))
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('final-state mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('final-state baseline/control failed/'+mode+': '+out)
    specs=(
        ('worker terminal sink skipped','session/worker.c','if(c->final_capture) {','if(false && c->final_capture) {','unit','flow','unit','disabled'),
        ('error state exported','session/worker.c','if(rc==GT_OK && owner_ready)','if(owner_ready)','unit','backend','unit','startup'),
        ('backend still open during seal','session/worker.c','if (backend) backend->ops->close(backend);','if (backend && false) backend->ops->close(backend);','unit','flow','unit','startup'),
        ('state closed before seal','session/worker.c','if(c->final_capture) {','if(state){c->close_state(state);state=NULL;}\n  if(c->final_capture) {','unit','flow','unit','startup'),
        ('capture status falsely OK','session/worker.c','c->final_capture(c->context,captured,final)','c->final_capture(c->context,captured==GT_ERR_UNSUPPORTED?GT_OK:captured,final)','unit','failure','unit','zero'),
        ('frozen ownership lost','session/worker.c','c->final_capture(c->context,captured,final)','c->final_capture(c->context,captured,(gt_state_capture_t){0})','unit','flow','unit','failure'),
        ('terminal capture sequence lost','session/worker.c','c->final_capture(c->context,captured,final)','final.sequence=0;c->final_capture(c->context,captured,final)','unit','flow','unit','zero'),
        ('ordinary snapshot failure changes process result','session/worker.c','if(owner.failure!=GT_OK){rc=owner.failure;stage=GT_WORKER_STAGE_CAPTURE;}','if(captured!=GT_OK){rc=captured;stage=GT_WORKER_STAGE_CAPTURE;}','unit','failure','unit','startup'),
        ('NULL success hidden as healthy EOF','session/worker.c','if(owner.failure!=GT_OK){rc=owner.failure;stage=GT_WORKER_STAGE_CAPTURE;}','','unit','null','unit','allocation'),
        ('creation wrapper drops final sink','session/creation.c','wrapped.final_capture=j->original.final_capture?final_capture:NULL;','wrapped.final_capture=true?NULL:final_capture;','runtime','final-retained','unit','flow'),
        ('creation wrapper loses capture error','session/creation.c','j->original.final_capture(j->original.context,status,capture)','j->original.final_capture(j->original.context,status==GT_ERR_UNSUPPORTED?GT_OK:status,capture)','runtime','final-unavailable','unit','flow'),
        ('runtime omits final sink','session/runtime.c','.final_capture=final_capture','.final_capture=true?NULL:final_capture','runtime','final-retained','unit','flow'),
        ('retained frozen not released','session/runtime.c','if(c->final.frozen)c->runtime','if(false && c->final.frozen)c->runtime','runtime','final-retained','unit','flow'),
        ('pending worker final exposed','session/runtime.c','if(!info.has_result)return GT_RUNTIME_BUSY;','if(false && !info.has_result)return GT_RUNTIME_BUSY;','runtime','final-publication','unit','flow'),
        ('query failure leaves stale output','session/runtime.c','if(out)memset(out,0,sizeof(*out));','','runtime','final-retained','unit','flow'),
        ('query consumes final object','session/runtime.c','pthread_mutex_unlock(&c->runtime->lock);return GT_RUNTIME_OK;','c->final.frozen=NULL;pthread_mutex_unlock(&c->runtime->lock);return GT_RUNTIME_OK;','runtime','final-retained','unit','flow'),
        ('known zero folded into unknown','session/runtime.c','pthread_mutex_unlock(&c->runtime->lock);return GT_RUNTIME_OK;','out->result.has_sequence=out->result.sequence!=0;pthread_mutex_unlock(&c->runtime->lock);return GT_RUNTIME_OK;','runtime','final-errors','unit','flow'),
        ('runtime loses freeze failure','session/runtime.c','c->final_status=status','c->final_status=status==GT_ERR_UNSUPPORTED?GT_OK:status','runtime','final-unavailable','unit','flow'),
        ('runtime loses terminal sequence','session/runtime.c','c->final=capture','c->final=capture;c->final.sequence=0','runtime','final-retained','unit','flow'),
        ('release callback optional','session/runtime.c',' || !config->state.release_frozen','','runtime','invalid','unit','flow'),
    )

    try:
        restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        for spec in specs:
            label,name,old,new,suite,target,control_suite,control=spec[:8];expected=spec[8] if len(spec)>8 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('final-state mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True)
                if control_suite!=suite:build(control_suite)
                probe(control_suite,control)
                print('PASS final-state mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        print(f'PASS {len(specs)} final-state mutations, {sum(map(len,MODES.values()))} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
