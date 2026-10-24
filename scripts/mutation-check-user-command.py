#!/usr/bin/env python3
"""Worker USER commit, metadata and result semantics; isolated ordinary failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/user-command-mutations'
RUNS=WORK/'runs'
MARK='GT_USER_COMMAND_MUTATION_MARK'
NAMES=('worker.c','worker.h','manager.c','manager.h','creation.c','creation.h','runtime.c','runtime.h','attachment.c','attachment.h','restore-stream.c','restore-stream.h')
PATHS={name:ROOT/'core/src/session'/name for name in NAMES}
PATHS['runtime-test.c']=ROOT/'core/tests/test_session_runtime.c'
PATHS['unit.c']=ROOT/'core/tests/test_worker_authorization.c'
MODES={'unit':('metadata','denial','capability','backend','cancel','allocation'),'runtime':('user-policy','user-stale-queue','user-readonly')}


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
    if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.glob('*.[ch]')):
        return 2
    def interrupted(signum,frame):raise RuntimeError('user-command mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('user-command SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('user-command dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build(suite):
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['unit.c' if suite=='unit' else 'runtime-test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/suite)])
        if rc:raise RuntimeError('user-command compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('user-command dependency archive changed')
    def probe(suite,mode,reject=False):
        rc,out=run([str(WORK/suite),mode]+([str(RUNS)] if suite=='runtime' else []))
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('user-command mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('user-command baseline/control failed/'+mode+': '+out)
    specs=(
        ('policy never consulted','worker.c','if(cmd.user) {','if(false && cmd.user) {','unit','denial','unit','capability'),
        ('policy denial ignored','worker.c','rejected=result!=GT_OK;','rejected=false;','unit','denial','unit','metadata'),
        ('stamp discarded','worker.c','.stamp=stamp?*stamp:(gt_worker_stamp_t){0}','.stamp=(gt_worker_stamp_t){0}','unit','metadata','unit','capability'),
        ('completion stamp discarded','worker.c','.stamp=cmd->stamp,.status=rc','.stamp={0},.status=rc','unit','metadata','unit','capability'),
        ('attempted failure falsely not executed','worker.c','if(!rejected)cmd.delivery=GT_USER_OUTCOME_UNKNOWN;','','unit','backend','unit','capability'),
        ('success never confirmed','worker.c','if(!rejected && result==GT_OK)cmd.delivery=GT_USER_OPERATION_OK;','','unit','metadata','unit','cancel'),
        ('denial falsely unknown','worker.c','if(!rejected)cmd.delivery=GT_USER_OUTCOME_UNKNOWN;','cmd.delivery=GT_USER_OUTCOME_UNKNOWN;','unit','denial','unit','backend'),
        ('denial stops session','worker.c','result != GT_OK && !rejected && cmd.kind','result != GT_OK && cmd.kind','unit','denial','unit','backend'),
        ('backend auth failure treated as rejection','worker.c','result != GT_OK && !rejected && cmd.kind','result != GT_OK && result!=GT_ERR_AUTH && !rejected && cmd.kind','unit','backend','unit','capability'),
        ('unknown policy status escapes','worker.c','if((unsigned)result>GT_ERR_INTERNAL)result=GT_ERR_INTERNAL;','','unit','denial','unit','metadata'),
        ('missing policy accepted','worker.c','else if(stamp && !w->config.authorize)result=GT_WORKER_UNSUPPORTED;','','unit','capability','unit','cancel'),
        ('capture rewritten as input','worker.c','if(!stamp || !stamp->generation','if(kind==GT_WORKER_CAPTURE)kind=GT_WORKER_INPUT;\n  if(!stamp || !stamp->generation','unit','capability','unit','metadata'),
        ('zero generation admitted','worker.c',' || !stamp->generation','','unit','capability','unit','metadata'),
        ('zero command admitted','worker.c',' || !stamp->command_id','','unit','capability','unit','metadata'),
        ('missing completion admitted','worker.c',' || !callback || kind==GT_WORKER_CAPTURE',' || kind==GT_WORKER_CAPTURE','unit','capability','unit','metadata'),
        ('payload not erased','worker.c','gt__secure_erase(cmd->payload,cmd->length);','','unit','metadata','runtime','user-readonly'),
        ('worker not erased','worker.c','gt__secure_erase(w,sizeof(*w));','','unit','metadata','runtime','user-readonly'),
        ('USER byte limit omitted','worker.c','length > INPUT_CAPACITY - w->bytes','false','unit','allocation','unit','metadata'),
        ('USER payload bypasses domain','worker.c','length ? w->allocator.allocate(w->allocator.context,length) : NULL','length ? malloc(length) : NULL','unit','allocation','runtime','invalid'),
        ('manager drops applied sequence','manager.c','e->worker,kind,stamp,bytes,n,user_callback','e->worker,kind,&(gt_worker_stamp_t){stamp->generation,stamp->command_id,0},bytes,n,user_callback','runtime','user-policy','unit','metadata'),
        ('manager changes generation','manager.c','e->worker,kind,stamp,bytes,n,user_callback','e->worker,kind,&(gt_worker_stamp_t){stamp->generation+1,stamp->command_id,stamp->applied_sequence},bytes,n,user_callback','runtime','user-policy','unit','metadata'),
        ('manager drops command ID','manager.c','e->worker,kind,stamp,bytes,n,user_callback','e->worker,kind,&(gt_worker_stamp_t){stamp->generation,1,stamp->applied_sequence},bytes,n,user_callback','runtime','user-policy','unit','metadata'),
        ('NULL stamp falls back to internal','manager.c','if(!stamp){if(result)*result=GT_WORKER_INVALID;return GT_MANAGER_INVALID;}','','runtime','user-policy','unit','metadata'),
        ('creation wrapper drops policy','creation.c','wrapped.authorize=j->original.authorize?authorize:NULL;','(void)authorize;wrapped.authorize=NULL;','runtime','user-policy','unit','metadata'),
        ('runtime does not install policy','runtime.c','.authorize=authorize','.authorize=true?NULL:authorize','runtime','user-policy','unit','metadata'),
        ('runtime loses command ID','runtime.c','stamp->generation,stamp->command_id,stamp->applied_sequence','stamp->generation,1,stamp->applied_sequence','runtime','user-policy','unit','metadata'),
        ('replayed command permitted','attachment.c','if(id<=a->last_command)rc=GT_ERR_PROTOCOL;','if(false && id<=a->last_command)rc=GT_ERR_PROTOCOL;','runtime','user-policy','unit','metadata'),
        ('rejected command ID reusable','attachment.c','a->last_command=id;','if(!a->session_ended && gt__restore_input_allowed(&a->stream,g,s))a->last_command=id;','runtime','user-policy','unit','metadata'),
        ('new generation retains old command IDs','attachment.c','a->last_command=0;','','runtime','user-policy','unit','metadata'),
        ('stale generation poisons new command IDs','attachment.c','gt_status_t rc=GT_ERR_AUTH;\n  if(current(a,g))','gt_status_t rc=GT_ERR_AUTH;\n  if(true)','runtime','user-stale-queue','unit','metadata'),
        ('readonly latch bypassed','attachment.c','if(!a->session_ended && gt__restore_input_allowed(&a->stream,g,s))rc=GT_OK;','if(gt__restore_input_allowed(&a->stream,g,s))rc=GT_OK;','runtime','user-readonly','unit','metadata'),
        ('restore barrier bypassed','attachment.c','if(!a->session_ended && gt__restore_input_allowed(&a->stream,g,s))rc=GT_OK;','(void)s;if(!a->session_ended)rc=GT_OK;','runtime','user-policy','unit','metadata'),
        ('applied sequence before L allowed','restore-stream.c',' && sequence>=s->cut','','runtime','user-policy','unit','metadata'),
        ('future applied sequence allowed','restore-stream.c',' && sequence<=s->dequeued','','runtime','user-policy','unit','metadata'),
    )

    try:
        restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        for spec in specs:
            label,name,old,new,suite,target,control_suite,control=spec[:8];expected=spec[8] if len(spec)>8 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('user-command mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True)
                if control_suite!=suite:build(control_suite)
                probe(control_suite,control)
                print('PASS user-command mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        print(f'PASS {len(specs)} user-command mutations, {sum(map(len,MODES.values()))} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
