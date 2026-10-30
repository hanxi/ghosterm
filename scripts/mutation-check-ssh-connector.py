#!/usr/bin/env python3
"""Isolated SSH connector mutations with real sshd and timeout fixtures."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/ssh-connector-mutations'
SOURCE=ROOT/'core/src/ssh.c'
MARK='GT_SSH_CONNECTOR_MUTATION_MARK'
MODES=('deadline','cancel','clock','real-deadline','allocation','secrets','real')
def run(args,timeout=40):
    p=subprocess.Popen([str(x) for x in args],cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
    try:out,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
        p.wait();raise
    return p.returncode,out

def main():
    WORK.mkdir(parents=True,exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp');copy=WORK/'ssh.c';original=SOURCE.read_bytes();digest=hashlib.sha256(original).hexdigest()
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    def restore():
        copy.write_bytes(original)
        if SOURCE.read_bytes()!=original or hashlib.sha256(copy.read_bytes()).hexdigest()!=digest:raise RuntimeError('SSH connector SHA256 rollback failed')
    def interrupted(sig,frame):raise RuntimeError('SSH connector mutation interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    rc,cf=run(['pkg-config','--cflags','libssh2']);rc2,lf=run(['pkg-config','--libs','libssh2'])
    if rc or rc2:raise RuntimeError('libssh2 required')
    common=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-D_POSIX_C_SOURCE=200809L','-D_DARWIN_C_SOURCE=1','-D_DEFAULT_SOURCE=1','-Icore/include','-Icore/src',*shlex.split(cf)]
    def build():
        rc,out=run([*common,'-DGT_HAVE_LIBSSH2=1','-Dgt__secure_erase=gt_connect_test_erase','-c',copy,'-o',WORK/'ssh.o'])
        if rc:raise RuntimeError('connector compilation failed: '+out)
        rc,out=run([*common,'-DGT_CONNECT_ERASE_TEST=1','core/tests/test_ssh_connector.c',WORK/'ssh.o','core/build/libghosterm_core.a',*shlex.split(lf),'-pthread','-o',WORK/'test_ssh_connector'])
        if rc:raise RuntimeError('driver compilation failed: '+out)
    def probe(mode,red=False):
        args=['bash','core/tests/scripts/ssh_e2e.sh',WORK/'test_ssh_connector'] if mode=='real' else [WORK/'test_ssh_connector',mode]
        rc,out=run(args)
        if 'SKIP' in out:raise RuntimeError('real SSH evidence missing: '+out)
        if red:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+mode+': '+out)
        elif rc:raise RuntimeError('baseline/control failed/'+mode+': '+out)
    specs=[
      ('total authentication deadline ignored','if(now-c->started>=c->config.timeout_ms)','if(false && now-c->started>=c->config.timeout_ms)','deadline','allocation'),
      ('clock rollback ignored',' || now<c->now','', 'clock','allocation'),
      ('socket is blocking','fcntl(fd,F_SETFL,flags|O_NONBLOCK)','fcntl(fd,F_SETFL,flags)','cancel','allocation'),
      ('socket leaks across exec','fcntl(fd,F_SETFD,descriptor|FD_CLOEXEC)','fcntl(fd,F_SETFD,descriptor)','cancel','allocation'),
      ('resolved address ignored','memcpy(&address.sin_addr,c->config.address,4);','memset(&address.sin_addr,0x7f,4);','cancel','allocation'),
      ('cancel ignored','if(!c || (c->info.phase>=GT_SSH_CONNECT_TAKEN && c->info.phase!=GT_SSH_CONNECT_CREDENTIALS))return;','if(!c || c->ssh || (c->info.phase>=GT_SSH_CONNECT_TAKEN && c->info.phase!=GT_SSH_CONNECT_CREDENTIALS))return;','cancel','allocation'),
      ('unknown hostkey auto trusted','if(result==KH_MATCH){c->stage=CONNECT_METHODS;','if(result==KH_MATCH || result==KH_UNKNOWN){c->stage=CONNECT_METHODS;','real','allocation'),
      ('changed hostkey accepted','if(result==KH_MISMATCH){connector_fail','if(false && result==KH_MISMATCH){connector_fail','real','allocation'),
      ('trust identity changed to network address','kh_check(s->kh_path,s->host,s->port,name,c->host_key)','kh_check(s->kh_path,"127.0.0.1",s->port,name,c->host_key)','real','allocation'),
      ('trust write failure ignored','gt_status_t rc=kh_append(c->ssh->kh_path,c->ssh->host,c->ssh->port,c->info.key_type,c->host_key);','gt_status_t rc=GT_OK;','real','allocation'),
      ('authentication rejection called success','if(!rc){s->connected=true;c->info.phase=GT_SSH_CONNECT_READY;}','if(rc!=LIBSSH2_ERROR_EAGAIN){s->connected=true;c->info.phase=GT_SSH_CONNECT_READY;}','real','allocation'),
      ('hostkey decision renews expired authentication','if(!connector_tick(c))return c->info.error;','if(false && !connector_tick(c))return c->info.error;','real','allocation'),
      ('failed admission called success','if(!c)return GT_ERR_IO;','if(!c)return GT_OK;','allocation','cancel'),
      ('owner storage not cleared','gt__secure_erase(c,sizeof(*c));a.release','a.release','cancel','allocation'),
      ('credential buffers not cleared','gt__secure_erase(value,strlen(value));','(void)value;','secrets','allocation'),
      ('unverified handshake called complete','return ssh->handshake_verified?GT_OK:GT_ERR_PROTOCOL;','return GT_OK;','real','allocation'),
      ('authentication bypasses host verification','if (!ssh->session || !ssh->handshake_verified)','if (!ssh->session)','real','allocation'),
      ('fingerprint includes base64 padding',"while(n && text[n-1]=='=')text[--n]=0;","while(n && text[n-1]=='!')text[--n]=0;",'real','allocation'),
    ]
    try:
        restore();build()
        for mode in MODES:probe(mode)
        for label,old,new,target,control in specs:
            text=original.decode()
            if old not in text:raise RuntimeError('connector mutation anchor drift/'+label)
            try:
                copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe(control)
                print('PASS SSH connector mutation/'+label+': ordinary red, same-mutant control green',flush=True)
            finally:restore()
        build()
        for mode in MODES:probe(mode)
        print('PASS '+str(len(specs))+' SSH connector mutations, '+str(len(MODES))+' baselines, real sshd and SHA256 rollback',flush=True)
    finally:restore()
    return 0
if __name__=='__main__':sys.exit(main())
