#!/usr/bin/env python3
"""Real sshd tests of the dedicated SSH owner, isolated source mutations."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/ssh-link-mutations'
SOURCE=ROOT/'core/src/ipc/ssh-link.c'
MARK='GT_SSH_LINK_MUTATION_MARK'

def run(args,timeout=50):
    p=subprocess.Popen(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:out,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
        p.wait();raise
    return p.returncode,out

def main():
    original=SOURCE.read_bytes();digest=hashlib.sha256(original).hexdigest()
    WORK.mkdir(parents=True,exist_ok=True);copy=WORK/'ssh-link.c'
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    def restore():
        copy.write_bytes(original)
        if SOURCE.read_bytes()!=original or hashlib.sha256(copy.read_bytes()).hexdigest()!=digest:raise RuntimeError('SHA256 rollback failed')
    def interrupted(signum,frame):raise RuntimeError('SSH owner mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp')
    rc,flags=run(['pkg-config','--cflags','--libs','libssh2'])
    if rc:raise RuntimeError('libssh2 required')
    common=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-DGT_HAVE_LIBSSH2=1','-Icore/src','-Icore/include','-Icore/src/ipc']
    def build():
        for source,name in [('core/tests/test_ssh_link.c','test'),('core/tests/test_known_hosts.c','control'),('core/tests/test_ssh_proxy_status.c','status')]:
            rc,out=run([*common,source,str(copy),'core/build/libghosterm_core.a',*shlex.split(flags),'-pthread','-o',str(WORK/name)])
            if rc:raise RuntimeError('compilation failed: '+out)
    def probe(name,reject=False):
        rc,out=run(['bash','core/tests/scripts/ssh_e2e.sh',str(WORK/name)])
        if 'SKIP' in out:raise RuntimeError('real SSH evidence missing: '+out)
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('needs ordinary FAIL/exit 1: '+out)
        elif rc:raise RuntimeError('baseline/control failed: '+out)
    specs=[
        ('binary queue bytes corrupted','memcpy(q->bytes+tail,bytes,first);','memset(q->bytes+tail,0,first);'),
        ('stderr never drained','diagnostic=libssh2_channel_read_stderr(s->channel,(char *)bytes,sizeof(bytes));','diagnostic=0;'),
        ('second stream starved','unsigned i=(l->turn+n)%2;','unsigned i=0;(void)n;'),
        ('unsafe shell quoting',"out[(*used)++]='\\'';",'out[(*used)++]=\' \';'),
        ('cancellation ignored','if(l && !l->failed)fail_link(l,GT_ERR_CANCELLED);','if(false && l && !l->failed)fail_link(l,GT_ERR_CANCELLED);'),
        ('known error exit forgotten','s->info.has_exit_code=code!=0;','s->info.has_exit_code=false;'),
        ('unknown zero exit invented','s->info.has_exit_code=code!=0;','s->info.has_exit_code=true;'),
    ]
    try:
        restore();build();probe('test');probe('status');probe('control')
        for label,old,new in specs:
            text=original.decode()
            if old not in text:raise RuntimeError('mutation anchor drift: '+label)
            try:
                copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n')
                build();probe('test',True);probe('control')
                print('PASS SSH owner mutation/'+label+': ordinary red, same-mutant control green',flush=True)
            finally:restore()
        for label,old,new in [('typed proxy errors ignored','if(!s->detached && gt__proxy_exit_status(code,&error))','if(false && gt__proxy_exit_status(code,&error))'),('typed failure reason forgotten','if(l->failed)return l->error;','if(l->failed)return GT_ERR_IO;'),('intentional detach poisons owner','if(!s->detached && gt__proxy_exit_status(code,&error))','if(gt__proxy_exit_status(code,&error))')]:
            text=original.decode()
            if old not in text:raise RuntimeError('mutation anchor drift: '+label)
            try:
                copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n')
                build();probe('status',True);probe('control')
                print('PASS SSH owner mutation/'+label+': ordinary red, same-mutant control green',flush=True)
            finally:restore()
        build();probe('test');probe('status');probe('control')
        print(f'PASS {len(specs)+3} SSH owner mutations, real sshd baselines and SHA256 rollback',flush=True)
    finally:restore()
    return 0
if __name__=='__main__':sys.exit(main())
