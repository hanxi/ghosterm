#!/usr/bin/env python3
"""Mutations of native request admission and common nonblocking SSH provider."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/ssh-provider-mutations'
SOURCE=ROOT/'core/src/ipc/ssh-session-provider.c'
MARK='GT_SSH_PROVIDER_MUTATION_MARK'
MODES=('requests','stale','deadline','budget','cancel','rejection','admission','clock','reentry','invalid','real')
def run(args):
    p=subprocess.Popen([str(x) for x in args],cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
    try:out,_=p.communicate(timeout=40)
    except BaseException:
        if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
        p.wait();raise
    return p.returncode,out

def main():
    WORK.mkdir(parents=True,exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp');copy=WORK/'ssh-session-provider.c';original=SOURCE.read_bytes();digest=hashlib.sha256(original).hexdigest()
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    def restore():
        copy.write_bytes(original)
        if SOURCE.read_bytes()!=original or hashlib.sha256(copy.read_bytes()).hexdigest()!=digest:raise RuntimeError('SSH provider SHA256 rollback failed')
    def interrupted(sig,frame):raise RuntimeError('SSH provider mutation interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    rc,cf=run(['pkg-config','--cflags','libssh2']);rc2,lf=run(['pkg-config','--libs','libssh2'])
    if rc or rc2:raise RuntimeError('libssh2 required')
    common=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-DGT_HAVE_LIBSSH2=1','-Icore/include','-Icore/src','-Icore/src/ipc',*shlex.split(cf)]
    def build():
        rc,out=run([*common,copy,'core/tests/test_ssh_session_provider.c','core/build/libghosterm_core.a',*shlex.split(lf),'-pthread','-o',WORK/'test'])
        if rc:raise RuntimeError('SSH provider compilation failed: '+out)
    def probe(mode,red=False):
        command=['bash','core/tests/scripts/ssh_e2e.sh',WORK/'test'] if mode=='real' else [WORK/'test',mode]
        rc,out=run(command)
        if 'SKIP' in out:raise RuntimeError('real SSH evidence missing: '+out)
        if red:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+mode+': '+out)
        elif rc:raise RuntimeError('provider baseline/control failed/'+mode+': '+out)
    specs=[
      ('request ID reused','request=++p->last_request','request=1','requests','admission'),
      ('late old completion accepted','request!=p->info.request || ','','stale','admission'),
      ('second credential completion accepted','(requested_only && p->info.phase!=GT_SSH_PROVIDER_REQUESTED)','(false && requested_only && p->info.phase!=GT_SSH_PROVIDER_REQUESTED)','stale','admission'),
      ('DNS and credential wait unbounded','if(p->info.phase!=GT_SSH_PROVIDER_ACTIVE && now-p->started','if(false && p->info.phase!=GT_SSH_PROVIDER_ACTIVE && now-p->started','deadline','admission'),
      ('expired supply not cancelled','if(!tick(p))return p->info.error;','if(false && !tick(p))return p->info.error;','deadline','admission'),
      ('cancel notification lost','p->config.cancelled(p->config.context,p->info.request);','(void)p;','requests','admission'),
      ('cancel notified more than once','p->pending=false;','(void)p;','deadline','admission'),
      ('authentication rejection retried','*error==GT_ERR_IO || *error==GT_ERR_CANCELLED','*error!=GT_OK','rejection','requests'),
      ('callback reentry accepted','if(!p || p->callback)return GT_ERR_INVALID_ARG;','if(!p)return GT_ERR_INVALID_ARG;','reentry','admission'),
      ('clock regression ignored',' || *out<p->now','','clock','admission'),
      ('failed allocation called success','if(!p)return GT_ERR_IO;','if(!p)return GT_OK;','admission','requests'),
      ('relative helper path accepted',"text[0]!='/'",'false','admission','requests'),
      ('owner storage not erased','gt__secure_erase(p,sizeof(*p));a.release','a.release','requests','admission'),
      ('proxy path not owned','strcpy(p->proxy,config->proxy);','p->proxy[0]=0;','real','admission'),
      ('daemon path not owned','strcpy(p->daemon,config->daemon);','p->daemon[0]=0;','real','admission'),
      ('runtime path not owned','strcpy(p->runtime,config->runtime);','p->runtime[0]=0;','real','admission'),
      ('unknown hostkey UI never notified','if(phase==GT_SSH_CONNECT_HOSTKEY && !p->notified)','if(false && phase==GT_SSH_CONNECT_HOSTKEY && !p->notified)','real','admission'),
      ('hostkey prompt repeated','p->notified=true;','p->notified=false;','real','admission'),
      ('invalid rejection called success','if(error==GT_OK || error>GT_ERR_INTERNAL)','if(false && (error==GT_OK || error>GT_ERR_INTERNAL))','rejection','admission'),
    ]
    try:
        restore();build()
        for mode in MODES:probe(mode)
        for label,old,new,target,control in specs:
            text=original.decode()
            if old not in text:raise RuntimeError('provider mutation anchor drift/'+label)
            try:
                copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe(control)
                print('PASS SSH provider mutation/'+label+': ordinary red, same-mutant control green',flush=True)
            finally:restore()
        build()
        for mode in MODES:probe(mode)
        print('PASS '+str(len(specs))+' SSH provider mutations, '+str(len(MODES))+' baselines, real sshd and SHA256 rollback',flush=True)
    finally:restore()
    return 0
if __name__=='__main__':sys.exit(main())
