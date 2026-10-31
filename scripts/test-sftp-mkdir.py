#!/usr/bin/env python3
"""Real OpenSSH and protocol fault cases; run via ssh_e2e.sh."""
import importlib.util,json,os,select,signal,socket,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
STAGES=('mkdir-parent','mkdir-parent-real','mkdir-target','mkdir-create','mkdir-child',
        'mkdir-child-real','mkdir-open','mkdir-read','mkdir-eof','mkdir-close')
CASES=[('normal','normal',None,None,0,5,0),('allow','allow','mkdir-close','allow',0,5,0)]
CASES += [(stage+'-'+mode,mode,stage,action,8 if mode=='cancel' else 3,
           0 if index<3 else (3 if mode=='deny' else 2) if index==3 else 4,3 if mode=='deny' else 0)
          for index,stage in enumerate(STAGES) for mode,action in [('cancel','hold'),('timeout','hold'),('io','disconnect'),('deny','deny'),('eof','eof')]]
CASES += [('root','root','mkdir-close','allow',0,5,0)]
CASES += [('conflict','io','mkdir-target','mkdir-conflict',3,1,0)]
CASES += [(stage+'-'+action,'io',stage,'mkdir-'+action,7,0 if stage=='mkdir-parent' else 4,0)
          for stage in ('mkdir-parent','mkdir-child') for action in ('missing-type','link','file')]
CASES += [(stage+'-'+action,'io',stage,'mkdir-'+action,3,0 if stage=='mkdir-parent-real' else 4,0)
          for stage in ('mkdir-parent-real','mkdir-child-real') for action in ('alias','relative','nul')]
CASES += [(action,'io','mkdir-read','mkdir-'+action,3,4,0) for action in ('nonempty','duplicate-dot')]
CASES += [(stage+'-'+action,'io',stage,'mkdir-'+action,7 if stage=='mkdir-create' and action=='unsupported' else 3,
           (3 if action in ('unsupported','race') else 2) if stage=='mkdir-create' else 0,code)
          for stage in ('mkdir-target','mkdir-create') for action,code in [('unsupported',8),('unknown',0xdeadbeef),('lost',7),('status-eof',1),('race',4),('malformed',0)]]
CASES += [('total-budget','timeout','mkdir-close','mkdir-total-budget',3,0,0)]

def main():
    if len(sys.argv)!=6: raise RuntimeError('Run via ssh_e2e.sh')
    host,port,user,key,work=sys.argv[1:]
    binary=Path(os.environ.get('GHOSTERM_SFTP_MKDIR_BIN',ROOT/'core/build/test_sftp_mkdir')).resolve()
    spec=importlib.util.spec_from_file_location('channels',ROOT/'scripts/lib/android-ssh-channels.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    selected=os.environ.get('SFTP_MKDIR_CASE')
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
            command=['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require mkdir; shift; gt_timeout 20 "$@"','mkdir',str(ROOT),str(binary),*args]
            process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True);prefix=''
            try:
                if mode=='cancel':
                    if not select.select([process.stdout],[],[],8)[0]:raise RuntimeError('Mkdir probe not ready')
                    prefix=process.stdout.readline()
                    if not prefix.startswith('READY '):raise RuntimeError('Mkdir probe not admitted: '+prefix)
                    pid=int(prefix.split()[1]);deadline=time.monotonic()+3
                    while stage not in control(op='state',user=username)['events']:
                        if time.monotonic()>deadline or process.poll() is not None:raise RuntimeError('Mkdir barrier not reached')
                        time.sleep(.01)
                    os.kill(pid,signal.SIGUSR1)
                out,error=process.communicate(timeout=12)
                if process.returncode or 'SFTP MKDIR PASS' not in out:
                    print(prefix+out+error,flush=True);raise RuntimeError('FAIL: SFTP mkdir '+name)
                if stage:
                    events=control(op='state',user=username)['events'];steps=['channel','subsystem','version',*STAGES]
                    end='mkdir-parent-real' if name=='total-budget' else stage
                    expected=steps if mode=='allow' else steps[:steps.index(end)+1]
                    if events!=expected:raise RuntimeError(f'FAIL: {name} events {events!r}, expected {expected!r}')
            finally:
                if stage:control(op='release',user=username)
                if process.poll() is None:process.terminate();process.communicate(timeout=25)
            print('PASS SFTP mkdir '+name,flush=True)
    print('SFTP MKDIR PROTOCOL PASS',flush=True)
if __name__=='__main__':main()
