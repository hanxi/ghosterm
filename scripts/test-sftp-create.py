#!/usr/bin/env python3
"""Run via ssh_e2e.sh with the pinned protocol-test Python."""
import importlib.util,json,os,select,signal,socket,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CASES=[(stage+'-'+mode,'create-'+stage,action) for stage in ('open','write','close')
       for mode,action in [('deny','deny'),('disconnect','disconnect'),('cancel','hold'),('timeout','hold')]]

CASES += [('open-malformed-handle','create-open','create-short-handle'),('open-malformed-status','create-open','create-short-status')]

def main():
    if len(sys.argv)!=6:raise RuntimeError('Run through ssh_e2e.sh')
    host,port,user,key,work=sys.argv[1:]
    binary=Path(os.environ.get('GHOSTERM_SFTP_CREATE_BIN',ROOT/'core/build/test_sftp_create')).resolve()
    selected=os.environ.get('SFTP_CREATE_CASE')
    spec=importlib.util.spec_from_file_location('channels',ROOT/'scripts/lib/android-ssh-channels.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    def command(args):return ['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require create; shift; gt_timeout 40 "$@"','create',str(ROOT),str(binary),*args]
    if not selected or selected=='normal':
        result=subprocess.run(command([host,port,user,key,work]),capture_output=True,text=True)
        print(result.stdout+result.stderr,end='',flush=True)
        if result.returncode or 'SFTP CREATE PASS' not in result.stdout:raise RuntimeError('Create real-file baseline failed')
        if selected:return
    if selected and selected not in [name for name,_,_ in CASES]:raise RuntimeError('Unknown case')
    with module.fixture(work,True) as data:
        peer=data['channels']
        def control(**fields):
            with socket.create_connection(('127.0.0.1',peer['controlPort']),timeout=2) as sock:
                sock.sendall((json.dumps(dict(token=peer['token'],**fields))+'\n').encode());return json.loads(sock.makefile().readline())
        for name,stage,action in CASES:
            if selected and selected!=name:continue
            directory=Path(work)/name;directory.mkdir()
            username=control(op='new',stage=stage,action=action)['user']
            process=subprocess.Popen(command(['127.0.0.1',str(peer['port']),username,key,str(directory),name]),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
            prefix=''
            try:
                if action=='hold' and name.endswith('cancel'):
                    if not select.select([process.stdout],[],[],8)[0]:raise RuntimeError('Create probe not ready')
                    prefix=process.stdout.readline()
                    if not prefix.startswith('READY '):raise RuntimeError(prefix)
                    pid=int(prefix.split()[1]);deadline=time.monotonic()+4
                    while stage not in control(op='state',user=username)['events']:
                        if time.monotonic()>deadline or process.poll() is not None:raise RuntimeError('Create barrier missing')
                        time.sleep(.01)
                    os.kill(pid,signal.SIGUSR1)
                out,error=process.communicate(timeout=15)
                if process.returncode or 'SFTP CREATE PASS' not in out:
                    print(prefix+out+error,flush=True);raise RuntimeError('Create protocol '+name+' failed')
                events=control(op='state',user=username)['events'];unique=[]
                for event in events:
                    if not unique or unique[-1]!=event:unique.append(event)
                expected=['channel','subsystem','version','create-open','create-write','create-close']
                expected=expected[:expected.index(stage)+1]
                if unique!=expected:raise RuntimeError(f'{name}: {unique!r} != {expected!r}')
            finally:
                control(op='release',user=username)
                if process.poll() is None:process.terminate();process.communicate(timeout=45)
            print('PASS create protocol '+name,flush=True)
    print('SFTP CREATE PROTOCOL PASS',flush=True)
if __name__=='__main__':main()
