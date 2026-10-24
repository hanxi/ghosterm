#!/usr/bin/env python3
"""Real SSH relay mutations. Inherit disposable fixture env; pass relay args.
The supplied GT_TEST_REMOTE_RUNTIME is a prefix: each run gets a unique suffix.
Remote test daemons intentionally persist; the fixture owner must clean these
reported runtimes after this test (never use a production installation).
"""
import hashlib
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/session-network-mutations'
SOURCE=ROOT/'scripts/probe-session-network.py'
MARK='GT_SESSION_NETWORK_MUTATION_MARK'
def run(args,env=None,timeout=130):
    p=subprocess.Popen([str(x) for x in args],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:out,_=p.communicate(timeout=timeout)
    except BaseException:
        # Relay owns a separate driver process group. Let its finally block
        # stop that group and remove its private socket before force-killing.
        if p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:p.communicate(timeout=6)
            except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
        raise
    return p.returncode,out

def main():
    WORK.mkdir(parents=True,exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp');copy=WORK/'relay.py';original=SOURCE.read_bytes();digest=hashlib.sha256(original).hexdigest()
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    prefix=os.environ.get('GT_TEST_REMOTE_RUNTIME','')
    if not prefix.startswith('/') or len(prefix)>80:raise RuntimeError('short absolute disposable GT_TEST_REMOTE_RUNTIME required')
    runtimes=[]
    def restore():
        copy.write_bytes(original)
        if SOURCE.read_bytes()!=original or hashlib.sha256(copy.read_bytes()).hexdigest()!=digest:raise RuntimeError('network relay SHA256 rollback failed')
    def interrupted(sig,frame):raise RuntimeError('network mutation interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    def probe(script,red=False):
        runtime=prefix+'-'+secrets.token_hex(3);runtimes.append(runtime)
        rc,out=run(['python3',script,*sys.argv[1:]],dict(os.environ,GT_TEST_REMOTE_RUNTIME=runtime))
        if red:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required: '+out)
        elif rc:raise RuntimeError('real SSH network baseline failed: '+out)
        (WORK/('run-'+str(len(runtimes))+'.log')).write_text(out)
    specs=[
        ('RST injection disabled','self.close_pair(p,True)','None'),
        ('existing connection not blackholed',"for p in self.connections:p['blocked']=True;","for p in self.connections:p['blocked']=False;"),
        ('old upstream closed before lease',"if p['blocked']:\n                            # Withhold FIN","if False and p['blocked']:\n                            # Withhold FIN"),
    ]
    try:
        restore();probe(SOURCE)
        for label,old,new in specs:
            if old not in original.decode():raise RuntimeError('network mutation anchor drift/'+label)
            try:
                text=original.decode().replace('ROOT=Path(__file__).resolve().parents[1]','ROOT=Path('+repr(str(ROOT))+')')
                copy.write_text(text.replace(old,new)+'\n# '+MARK+'\n');probe(copy,True)
                rc,out=run([ROOT/'core/build/test_session_client','readonly'])
                if rc:raise RuntimeError('same-mutant readonly control failed: '+out)
                print('PASS network mutation/'+label+': ordinary red, readonly control green',flush=True)
            finally:restore()
        probe(SOURCE);print('PASS 3 real SSH network mutations and SHA256 rollback',flush=True)
    finally:
        restore();(WORK/'runtimes.txt').write_text('\n'.join(runtimes)+'\n')
        print('Disposable runtimes for fixture-owner cleanup: '+str(WORK/'runtimes.txt'),flush=True)
    return 0
if __name__=='__main__':sys.exit(main())
