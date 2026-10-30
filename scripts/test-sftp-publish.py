#!/usr/bin/env python3
"""Real OpenSSH and protocol fault cases; run via ssh_e2e.sh."""
import importlib.util,json,os,select,signal,socket,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
STAGES=('publish-source','publish-target','publish-recheck','publish-rename')
CASES=[('normal','normal',None,None,0,4,0),('allow','allow','publish-rename','allow',0,4,0)]
CASES += [(stage+'-'+mode,mode,stage,action,8 if mode=='cancel' else 3,
           (3 if mode=='deny' else 2) if stage=='publish-rename' else 0,3 if mode=='deny' else 0)
          for stage in STAGES for mode,action in [('cancel','hold'),('timeout','hold'),('io','disconnect'),('deny','deny'),('eof','eof')]]
CASES += [('conflict','io','publish-target','publish-conflict',3,1,0)]
CASES += [(stage+'-'+action,'io',stage,'publish-'+action,7 if action in ('missing-type','link') else 3,0,0)
          for stage in ('publish-source','publish-recheck') for action in ('missing-size','missing-type','link','size')]
CASES += [('recheck-'+action,'io','publish-recheck','publish-'+action,3,0,0) for action in ('mtime','missing-mtime')]
CASES += [(stage+'-'+action,'io',stage,'publish-'+action,7 if stage=='publish-rename' and action=='unsupported' else 3,
           (3 if action in ('unsupported','race') else 2) if stage=='publish-rename' else 0,code)
          for stage in ('publish-target','publish-rename') for action,code in [('unsupported',8),('unknown',0xdeadbeef),('connection-lost',7),('status-eof',1),('race',4),('malformed',0)]]
CASES += [('total-budget','timeout','publish-rename','publish-total-budget',3,0,0)]

def main():
    if len(sys.argv)!=6: raise RuntimeError('Run via ssh_e2e.sh')
    host,port,user,key,work=sys.argv[1:]
    binary=Path(os.environ.get('GHOSTERM_SFTP_PUBLISH_BIN',ROOT/'core/build/test_sftp_publish')).resolve()
    spec=importlib.util.spec_from_file_location('channels',ROOT/'scripts/lib/android-ssh-channels.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    selected=os.environ.get('SFTP_PUBLISH_CASE')
    if selected and selected not in [c[0] for c in CASES]:raise RuntimeError('Unknown case')
    with module.fixture(work,True) as data:
        peer=data['channels']
        def control(**fields):
            with socket.create_connection(('127.0.0.1',peer['controlPort']),timeout=2) as sock:
                sock.sendall((json.dumps(dict(token=peer['token'],**fields))+'\n').encode())
                return json.loads(sock.makefile().readline())
        for name,mode,stage,action,status,state,remote in CASES:
            if selected and name!=selected:continue
            directory=Path(work)/name;directory.mkdir()
            username=control(op='new',stage=stage,action=action)['user'] if stage else user
            args=['127.0.0.1',str(peer['port']),username,key,str(directory),mode,str(status),str(state),str(remote)] if stage else [host,port,user,key,str(directory)]
            command=['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require publish; shift; gt_timeout 20 "$@"','publish',str(ROOT),str(binary),*args]
            process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);prefix=''
            try:
                if mode=='cancel':
                    if not select.select([process.stdout],[],[],8)[0]:raise RuntimeError('Publish probe not ready')
                    prefix=process.stdout.readline()
                    if not prefix.startswith('READY '):raise RuntimeError('Publish probe not admitted: '+prefix)
                    pid=int(prefix.split()[1]);deadline=time.monotonic()+3
                    while stage not in control(op='state',user=username)['events']:
                        if time.monotonic()>deadline or process.poll() is not None:raise RuntimeError('Publish barrier not reached')
                        time.sleep(.01)
                    os.kill(pid,signal.SIGUSR1)
                out,error=process.communicate(timeout=12)
                if process.returncode or 'SFTP PUBLISH PASS' not in out:
                    print(prefix+out+error,flush=True);raise RuntimeError('FAIL: SFTP publish '+name)
                if stage:
                    events=control(op='state',user=username)['events'];steps=['channel','subsystem','version',*STAGES]
                    end='publish-target' if name=='total-budget' else stage
                    expected=steps if mode=='allow' else steps[:steps.index(end)+1]
                    if events!=expected:raise RuntimeError(f'FAIL: {name} events {events!r}, expected {expected!r}')
            finally:
                if stage:control(op='release',user=username)
                if process.poll() is None:process.terminate();process.communicate(timeout=25)
            print('PASS SFTP publish '+name,flush=True)
    print('SFTP PUBLISH PROTOCOL PASS',flush=True)
if __name__=='__main__':main()
