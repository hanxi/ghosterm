#!/usr/bin/env python3
"""Verify Linux deployment against a disposable SSH fixture, with isolated mutations."""
import argparse
import hashlib
import os
from pathlib import Path
import secrets
import shlex
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/deploy-mutations'
SOURCE=ROOT/'core/src/session/deploy.c'
MARK='GT_DEPLOY_MUTATION_MARK'

def run(args,env=None,timeout=150):
    p=subprocess.Popen([str(a) for a in args],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:out,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
        p.wait();raise
    return p.returncode,out

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--host',required=True);p.add_argument('--port',type=int,required=True);p.add_argument('--user',required=True)
    p.add_argument('--key',type=Path,required=True);p.add_argument('--bundle',type=Path,required=True);p.add_argument('--remote-root',required=True)
    args=p.parse_args();WORK.mkdir(parents=True,exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    original=SOURCE.read_bytes();digest=hashlib.sha256(original).hexdigest();copy=WORK/'deploy.c';known=WORK/'known_hosts'
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    def interrupted(sig,frame):raise RuntimeError('deployment mutation interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    def restore():
        copy.write_bytes(original)
        if SOURCE.read_bytes()!=original or hashlib.sha256(copy.read_bytes()).hexdigest()!=digest:raise RuntimeError('deployment SHA256 rollback failed')
    if not args.remote_root.startswith('/') or len(args.remote_root)>600:raise RuntimeError('absolute disposable remote root required')
    remote=args.remote_root.rstrip('/')+'/case-'+secrets.token_hex(6);garbage=[remote,remote+'-unsafe',remote+'-link']
    rc,flags=run(['pkg-config','--cflags','--libs','libssh2'])
    if rc:raise RuntimeError('libssh2 required')
    cflags_rc,cflags=run(['pkg-config','--cflags','libssh2'])
    if cflags_rc:raise RuntimeError('libssh2 headers required')
    common=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-DGT_HAVE_LIBSSH2=1','-Icore/src','-Icore/src/session','-Icore/include',*shlex.split(cflags)]
    def build():
        rc,out=run([*common,'-Dlibssh2_sftp_open_ex=gt_deploy_test_open','-Dlibssh2_sftp_close_handle=gt_deploy_test_close','-c',copy,'-o',WORK/'deploy.o'])
        if rc:raise RuntimeError('deployment compilation failed: '+out)
        rc,out=run([*common,'-DGT_DEPLOY_SHIM=1','core/tests/test_deploy.c','core/tests/deploy-io.c',WORK/'deploy.o','core/build/libghosterm_core.a',*shlex.split(flags),'-pthread','-o',WORK/'test'])
        if rc:raise RuntimeError('deployment driver compilation failed: '+out)
    def ssh(command):
        rc,out=run(['ssh','-i',args.key.resolve(),'-p',args.port,'-o','BatchMode=yes','-o','IdentitiesOnly=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+str(known),'-l',args.user,'--',args.host,command])
        if rc:raise RuntimeError('fixture command failed: '+out)
    def probe(mode,reject=False):
        target=remote
        if mode=='catalog':command=[WORK/'test','--catalog',ROOT/'tmp']
        else:
            if mode=='symlink':target=remote+'-link'
            if mode=='permissions':target=remote+'-unsafe'
            if mode=='cancel-upload':target=remote+'-cancel-'+secrets.token_hex(4);garbage.append(target)
            command=[WORK/'test',args.host,args.port,args.user,args.key.resolve(),args.bundle.resolve(),target,known]
        rc,out=run(command,dict(os.environ,GT_DEPLOY_CASE=mode))
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('needs ordinary FAIL/exit 1/'+mode+': '+out)
        elif rc:raise RuntimeError('deployment baseline/control failed/'+mode+': '+out)
    specs=[
      ('architecture mismatch ignored','strcmp(probe.arch,b->arch)','0','arch'),
      ('minimum glibc ignored','a<x || (a==x && (bv<y || (bv==y && c<z)))','false','glibc'),
      ('SHA256 verification ignored','if(strcmp(reply,"GT_VERIFY_OK\\n"))','if(false && strcmp(reply,"GT_VERIFY_OK\\n"))','flow'),
      ('remote parent symlink followed','LIBSSH2_SFTP_LSTAT','LIBSSH2_SFTP_STAT','symlink'),
      ('writable remote parents trusted','(a.permissions&0022)','false','permissions'),
      ('cancellation ignored','if(j->options.cancelled && j->options.cancelled(j->options.context))','if(false && j->options.cancelled && j->options.cancelled(j->options.context))','cancel'),
      ('wrong engine contract accepted','if(strcmp(reply,expected))','if(false && strcmp(reply,expected))','contract'),
      ('SFTP close continuation forgotten','rc=libssh2_sftp_close_handle(file);','rc=0;','cancel-upload'),
      ('catalog file names ignored',' && !strcmp(parts[0],inventory[i])','','catalog'),
      ('catalog trailing fields ignored','if(fgetc(f)!=EOF)okay=false;','(void)fgetc(f);','catalog'),
      ('catalog symlink followed','fd=openat(root,inventory[i],O_RDONLY|O_CLOEXEC|O_NOFOLLOW);','fd=openat(root,inventory[i],O_RDONLY|O_CLOEXEC);','catalog'),
      ('catalog file size ignored','(uint64_t)st.st_size==n','true','catalog'),
    ]
    try:
        restore();build();probe('catalog');probe('flow')
        ssh('ln -s '+shlex.quote(remote)+' '+shlex.quote(remote+'-link')+' && mkdir -m 777 '+shlex.quote(remote+'-unsafe')+' && chmod 777 '+shlex.quote(remote+'-unsafe'))
        for mode in ('arch','glibc','symlink','permissions','cancel','contract','cancel-upload'):probe(mode)
        for label,old,new,target in specs:
            if old not in original.decode():raise RuntimeError('mutation anchor drift/'+label)
            try:
                copy.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe('probe')
                print('PASS deployment mutation/'+label+': ordinary red, same-mutant Linux probe green',flush=True)
            finally:restore()
        build();probe('flow');probe('catalog')
        print('PASS 12 deployment mutations, real Linux SSH/SFTP and SHA256 rollback',flush=True)
    finally:
        restore()
        if known.exists():
            try:ssh('rm -rf -- '+' '.join(shlex.quote(x) for x in garbage))
            except Exception as e:print('fixture cleanup pending: '+str(e),file=sys.stderr)
    return 0
if __name__=='__main__':sys.exit(main())
