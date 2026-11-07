#!/usr/bin/env python3
"""Real managed SSH jump chains. Client may dial only the first SSH server.
No private key files, no external relay, no manual shell/tmux routing.
"""
from pathlib import Path
from contextlib import ExitStack, contextmanager
import argparse, hashlib, importlib.util, json, os, pwd, select, shlex, signal, socket, struct, subprocess, sys, tempfile, threading, time
ROOT=Path(__file__).resolve().parents[1]
sys.dont_write_bytecode=True

def require(ok,message):
    if not ok:raise AssertionError(message)

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

def stop_group(process):
    # Each fixture sshd has its own session; never signal the caller's group.
    if process.poll() is None:
        os.killpg(process.pid,signal.SIGTERM)
        try:process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid,signal.SIGKILL);process.wait()

@contextmanager
def stalled_peer():
    closing=threading.Event();banner=threading.Event();listener=socket.socket()
    listener.bind(('127.0.0.1',0));listener.listen();listener.settimeout(.1)
    peers=[]
    def run():
        while not closing.is_set():
            try:connection,_=listener.accept()
            except socket.timeout:continue
            peers.append(connection);connection.settimeout(.1);prefix=b''
            while not closing.is_set():
                try:data=connection.recv(1024)
                except socket.timeout:continue
                except OSError:break
                if not data:break
                prefix=(prefix+data)[:4096]
                if prefix.startswith(b'SSH-2.0-'):banner.set()
    worker=threading.Thread(target=run);worker.start()
    try:yield listener.getsockname()[1],banner
    finally:
        closing.set();worker.join(2);require(not worker.is_alive(),'stall fixture did not stop')
        for connection in peers:connection.close()
        listener.close()

@contextmanager
def chain(work,count,public,keys,agent,mode,*,sftp=False):
    with ExitStack() as stack:
        ports=[]
        for _ in range(count):
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1',0));ports.append(reservation.getsockname()[1])
        barrier=None
        stall=mode in ('deadline','cancel-auth')
        if stall:ports[-1],barrier=stack.enter_context(stalled_peer())
        authorized=work/'authorized_keys';authorized.write_bytes(public)
        empty=work/'empty';empty.mkdir()
        for i in range(count-(1 if stall else 0)):
            host=work/f'host-{i}.pub';host.write_bytes(keys[i])
            config=work/f'sshd-{i}.conf'
            forward=i+1<count and mode!='forward-deny' and not (mode=='middle-deny' and i==1)
            config.write_text(f'Port {ports[i]}\nListenAddress 127.0.0.1\nHostKey {host}\nHostKeyAgent {agent}\n'
                f'PidFile {work/f"sshd-{i}.pid"}\nAuthorizedKeysFile {authorized}\n'
                'PubkeyAuthentication yes\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nStrictModes no\n'
                'LogLevel ERROR\nPermitUserRC no\nAllowAgentForwarding no\nAllowStreamLocalForwarding no\n'
                f'AllowTcpForwarding {"local" if forward else "no"}\n'
                +(f'PermitOpen 127.0.0.1:{ports[i+1]}\n' if forward else '')
                +f'MaxSessions {0 if i+1<count else 10}\nPrintMotd no\nPrintLastLog no\nSetEnv HOME={empty} ZDOTDIR={empty} TMPDIR={work}\n')
            if sftp and i+1 == count:
                with config.open('a') as output: output.write(f'Subsystem sftp internal-sftp -d {shlex.quote(str(work))}\n')
            support=subprocess.run(['/usr/sbin/sshd','-T','-f',str(config),'-o','PerSourcePenalties=no'],capture_output=True,timeout=5)
            if support.returncode==0:
                with config.open('a') as output:output.write('PerSourcePenalties no\n')
            log=stack.enter_context((work/f'sshd-{i}.log').open('wb'))
            daemon=subprocess.Popen(['/usr/sbin/sshd','-D','-e','-f',str(config)],stdout=log,stderr=log,start_new_session=True)
            stack.callback(stop_group,daemon);end=time.monotonic()+5
            while True:
                require(daemon.poll() is None,'owned sshd failed to start')
                with socket.socket() as probe:
                    probe.settimeout(.05)
                    if probe.connect_ex(('127.0.0.1',ports[i]))==0:break
                require(time.monotonic()<end,'sshd startup timeout');time.sleep(.01)
        known=work/'known_hosts';data=b''
        for i in range(count):
            if i+1==count and mode in ('unknown-accept','unknown-reject'):continue
            key=keys[0] if i+1==count and mode=='wrong-pin' else keys[i]
            data+=f'[route-hop-{i}.invalid]:{ports[i]} '.encode()+key
        known.write_bytes(data)
        yield ports,known,barrier

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,default=ROOT/'core/build/test_bridge_route')
    parser.add_argument('--case',action='append');parser.add_argument('--expect-assertion',action='store_true',help=argparse.SUPPRESS)
    args=parser.parse_args();binary=args.binary.resolve();require(binary.is_file(),'build test_bridge_route first')
    helpers=load('route_ssh_helpers',ROOT/'scripts/test-plugin-ssh.py')
    streams=load('route_stream_helpers',ROOT/'scripts/test-plugin-ssh-stream.py')
    work=Path(tempfile.mkdtemp(prefix='plugin-route-',dir=ROOT/'tmp'))
    private,public=streams.memory_identity();results=[]
    modes=('echo2','echo3','wrong-pin','wrong-auth','unknown-accept','unknown-reject','credentials','forward-deny','cancel-auth','cancel-read','deadline','allocation',
           'wrong-pin3','wrong-auth3','cancel-auth3','cancel-read3','deadline3','middle-deny')
    selected=args.case or list(modes);require(all(mode in modes for mode in selected),'unknown route case')
    require(not args.expect_assertion or len(selected)==1,'one assertion target required')
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    env.pop('TMUX',None);env.pop('TMUX_PANE',None)
    with helpers.owned_agent(work,'host-agent') as agent:
        keys=[helpers.add_memory_key(agent,f'route-hop-{i}') for i in range(3)]
        require(len({key.split()[1] for key in keys})==3,'host identities collided')
        for case in selected:
            count=3 if case.endswith('3') or case=='middle-deny' else 2
            mode='echo' if case in ('echo2','echo3') else case[:-1] if case.endswith('3') else case
            folder=work/case;folder.mkdir()
            with chain(folder,count,public,keys,agent,mode) as (ports,known,barrier):
                command=shlex.join([str(binary),'--executor','echo'])
                shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-route; shift; gt_timeout 15 "$@"'
                process=subprocess.Popen(['bash','-c',shell,'plugin-route',str(ROOT),str(binary),mode,str(count),pwd.getpwuid(os.getuid()).pw_name,str(known),command,*map(str,ports)],
                    cwd=ROOT,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
                prefix=b''
                try:
                    process.stdin.write(struct.pack('>I',len(private))+private);process.stdin.flush()
                    if barrier:
                        require(select.select([process.stdout],[],[],8)[0],'route stall barrier timeout')
                        prefix=process.stdout.readline()
                        if prefix==b'ROUTE_STALL_BARRIER\n':
                            require(barrier.wait(5),'target did not receive actual inner SSH banner')
                            process.stdin.write(b'R');process.stdin.flush()
                    process.stdin.close();process.stdin=None
                    stdout,stderr=process.communicate(timeout=20)
                finally:
                    if process.poll() is None:process.kill();process.wait()
                output=(prefix+stdout+stderr).decode(errors='replace');(folder/'client.log').write_text(output)
                if args.expect_assertion:
                    require(process.returncode==1 and 'FAIL ' in output and not any(x in output for x in ('AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:')),
                            case+': expected normal C assertion; rc='+str(process.returncode)+'\n'+output)
                else:require(process.returncode==0 and f'PASS bridge_route_{mode}_{count}' in output,case+': '+output)
                results.append(case);print(('ASSERTION RED ' if args.expect_assertion else 'PASS bridge_route_')+case,flush=True)
    for path in work.rglob('*'):
        if path.is_file():require(b'PRIVATE KEY-----' not in path.read_bytes() and b'openssh-key-v1\0' not in path.read_bytes(),'private fixture serialized')
    (work/'result.json').write_text(json.dumps(dict(accepted=True,cases=results,expected_assertion=args.expect_assertion,client_native_dials=1,topology='two/three loopback sshd; explicit direct-tcpip; native inner dials denied',binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest()),indent=2)+'\n')
    print('PLUGIN ROUTE PASS: '+str(len(results))+' cases; logs: '+str(work),flush=True)

if __name__=='__main__':
    def interrupted(sig,frame):raise KeyboardInterrupt('signal '+str(sig))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted);main()
