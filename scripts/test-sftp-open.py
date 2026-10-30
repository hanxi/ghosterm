#!/usr/bin/env python3
"""Run via ssh_e2e.sh using the pinned SSH channel Python environment.
Normal SFTP uses OpenSSH; fault cases observe exact protocol stages on a test peer.
"""
import importlib.util,json,os,select,signal,socket,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
CASES=[('normal','normal',None,None),('version-allow','admit','version','allow')]+[
    (stage+'-'+mode,mode,stage,action)
    for stage in ('channel','subsystem','version')
    for mode,action in [('cancel','hold'),('timeout','hold'),('io','disconnect')]]
CASES += [('version-partial-cancel','cancel','version','partial'),('version-partial-timeout','timeout','version','partial'),('version-header-eof','io','version','header-eof'),('version-eof','io','version','eof'),('channel-deny','io','channel','deny'),('subsystem-deny','io','subsystem','deny')]

def main():
    if len(sys.argv)!=6:raise RuntimeError('Run through core/tests/scripts/ssh_e2e.sh')
    host,port,user,key,work=sys.argv[1:]
    binary=Path(os.environ.get('GHOSTERM_SFTP_OPEN_BIN',ROOT/'core/build/test_sftp_open')).resolve()
    spec=importlib.util.spec_from_file_location('channels',ROOT/'scripts/lib/android-ssh-channels.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    selected=os.environ.get('SFTP_OPEN_CASE')
    if selected and selected not in [c[0] for c in CASES]:raise RuntimeError('Unknown SFTP_OPEN_CASE')
    def control(peer,**fields):
        with socket.create_connection(('127.0.0.1',peer['controlPort']),timeout=2) as sock:
            sock.sendall((json.dumps(dict(token=peer['token'],**fields))+'\n').encode())
            return json.loads(sock.makefile().readline())
    def command(args):
        return ['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-open; shift; gt_timeout 20 "$@"','sftp-open',str(ROOT),str(binary),*args]
    with module.fixture(work,True) as data:
        peer=data['channels']
        for name,mode,stage,action in CASES:
            if selected and name!=selected:continue
            if stage is None:
                result=subprocess.run(command([host,port,user,key,work]),capture_output=True,text=True)
                if result.returncode or 'SFTP OPEN PASS' not in result.stdout:
                    print(result.stdout+result.stderr);raise RuntimeError('FAIL: normal SFTP')
            else:
                case=Path(work)/name;case.mkdir(exist_ok=True)
                username=control(peer,op='new',stage=stage,action=action)['user']
                process=subprocess.Popen(command(['127.0.0.1',str(peer['port']),username,key,str(case),mode]),stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
                prefix=''
                try:
                    if mode=='cancel':
                        if not select.select([process.stdout],[],[],8)[0]:raise RuntimeError('FAIL: SFTP probe never ready')
                        prefix=process.stdout.readline()
                        if not prefix.startswith('READY '):raise RuntimeError('FAIL: SFTP probe exited before admission: '+prefix)
                        pid=int(prefix.split()[1]);deadline=time.monotonic()+2
                        while stage not in control(peer,op='state',user=username)['events']:
                            if time.monotonic()>deadline or process.poll() is not None:raise RuntimeError('FAIL: SFTP barrier not reached')
                            time.sleep(.01)
                        os.kill(pid,signal.SIGUSR1)
                    out,err=process.communicate(timeout=12)
                    if process.returncode or 'SFTP OPEN PASS' not in out:
                        print(prefix+out+err,flush=True);raise RuntimeError('FAIL: SFTP '+name)
                    seen=control(peer,op='state',user=username)['events']
                    expected=['channel','subsystem','version']
                    expected=expected[:expected.index(stage)+1]
                    if seen!=expected:raise RuntimeError(f'FAIL: {name} protocol events {seen!r}, expected {expected!r}')
                finally:
                    control(peer,op='release',user=username)
                    if process.poll() is None:
                        process.terminate();process.communicate(timeout=25)
            print('PASS SFTP '+name,flush=True)
    print('SFTP OPEN PROTOCOL PASS',flush=True)

if __name__=='__main__':main()
