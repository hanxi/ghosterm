#!/usr/bin/env python3
"""Isolated mutations of the shared local/SSH recovery coordinator."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/session-client-mutations'
SOURCE=ROOT/'core/src/ipc/session-client.c'
MARK='GT_SESSION_CLIENT_MUTATION_MARK'
MODES=('flow','uncertain','deadlines','busy','heartbeat','identity','sink','readonly','fatal','clock','allocation','cancel','network','generation','restore-deadline','compatibility','validation')
def run(args):
    p=subprocess.Popen([str(x) for x in args],cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,start_new_session=True)
    try:out,_=p.communicate(timeout=30)
    except BaseException:
        if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
        p.wait();raise
    return p.returncode,out

def main():
    WORK.mkdir(parents=True,exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp');copy=WORK/'session-client.c';original=SOURCE.read_bytes();digest=hashlib.sha256(original).hexdigest()
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    def restore():
        copy.write_bytes(original)
        if SOURCE.read_bytes()!=original or hashlib.sha256(copy.read_bytes()).hexdigest()!=digest:raise RuntimeError('session client SHA256 rollback failed')
    def interrupted(sig,frame):raise RuntimeError('session client mutation interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    def build():
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Icore/src','-Icore/src/ipc','-Icore/include',
            copy,'core/tests/test_session_client.c','core/build/libghosterm_client_protocol.a','-o',WORK/'test'])
        if rc:raise RuntimeError('mutation compilation failed: '+out)
    def probe(mode,red=False):
        rc,out=run([WORK/'test',mode])
        if red:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+mode+': '+out)
        elif rc:raise RuntimeError('baseline/control failed/'+mode+': '+out)
    specs=[
      ('transient clock fault forgotten','if(c->clock_fault || !c->config.clock','if(!c->config.clock','clock','readonly'),
      ('busy lease hidden','c->info.busy_replies++;','(void)0;','busy','readonly'),
      ('retry renews overall deadline','if(!c->recovering){c->recovery_started=c->now;c->recovering=true;}','{c->recovery_started=c->now;c->recovering=true;}','deadlines','readonly'),
      ('backoff skipped','if(c->now-c->delayed<c->config.backoff_ms)return c->info.phase;','if(false && c->now-c->delayed<c->config.backoff_ms)return c->info.phase;','flow','readonly'),
      ('ambiguous CREATE repeated','c->create_admitted?GT_MSG_QUERY_CREATE:GT_MSG_CREATE','GT_MSG_CREATE','uncertain','readonly'),
      ('CREATE admission forgotten','if(c->request==GT_MSG_CREATE)c->create_admitted=true;','if(c->request==GT_MSG_CREATE)c->create_admitted=false;','uncertain','readonly'),
      ('UNKNOWN creation treated as committed','if(r.creation.status!=GT_CREATION_ACCEPTED)','if(false && r.creation.status!=GT_CREATION_ACCEPTED)','uncertain','readonly'),
      ('instance expectation lost','memcpy(config.hello.expected_instance,c->info.instance,16);','memset(config.hello.expected_instance,0,16);','identity','fatal'),
      ('generation replay accepted','if(a->generation<=c->info.generation)','if(false && a->generation<=c->info.generation)','generation','readonly'),
      ('management heartbeat stops when live','c->control?gt__control_client_step(c->control):GT_CONTROL_CLIENT_READY','c->control && c->stage!=ATTACHED?gt__control_client_step(c->control):GT_CONTROL_CLIENT_READY','heartbeat','readonly'),
      ('management heartbeat stops while restoring','c->control?gt__control_client_step(c->control):GT_CONTROL_CLIENT_READY','c->control && c->info.phase!=GT_SESSION_RESTORING?gt__control_client_step(c->control):GT_CONTROL_CLIENT_READY','heartbeat','readonly'),
      ('clock rollback ignored',' || *now<c->last_clock','', 'clock','readonly'),
      ('overall deadline disabled during import','if(c->recovering && now-c->recovery_started','if(c->recovering && c->stage!=ATTACHED && now-c->recovery_started','restore-deadline','readonly'),
      ('live before engine barrier','c->info.phase=GT_SESSION_RESTORING;','c->info.phase=GT_SESSION_LIVE;','sink','fatal'),
      ('readonly advertised as live','phase==GT_CLIENT_READONLY?GT_SESSION_READONLY:GT_SESSION_ENDED','phase==GT_CLIENT_READONLY?GT_SESSION_LIVE:GT_SESSION_ENDED','readonly','fatal'),
      ('missing session silently continued','if(r.status!=GT_CONTROL_OK)','if(false && r.status!=GT_CONTROL_OK)','identity','readonly'),
      ('provider reset before stream disposal','c->cleaning=true;','c->cleaning=true;if(c->started && c->attachment)c->config.link.reset(c->config.link.context);','cancel','fatal'),
      ('cancel ignored','if(!c || c->stage==STOPPED)return;c->stage=STOPPED;','if(!c || c->stage==STOPPED || c->started)return;c->stage=STOPPED;','cancel','fatal'),
      ('owner payload not erased','gt__secure_erase(c,sizeof(*c));a.release','a.release','allocation','validation'),
    ]
    try:
        restore();build()
        for mode in MODES:probe(mode)
        for label,old,new,target,control in specs:
            text=original.decode()
            if old not in text:raise RuntimeError('mutation anchor drift/'+label)
            try:
                copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe(control)
                print('PASS session client mutation/'+label+': ordinary red, same-mutant control green',flush=True)
            finally:restore()
        build()
        for mode in MODES:probe(mode)
        print('PASS '+str(len(specs))+' session client mutations, '+str(len(MODES))+' baselines and SHA256 rollback',flush=True)
    finally:restore()
    return 0
if __name__=='__main__':sys.exit(main())
