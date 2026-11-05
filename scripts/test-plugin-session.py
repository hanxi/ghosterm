#!/usr/bin/env python3
"""Real C BridgeSession over managed two/three-host SSH routes, no gateway.
The client native dial boundary admits only the first host. Keys stay in memory.
"""
from pathlib import Path
from contextlib import ExitStack
import argparse,hashlib,importlib.util,json,os,pwd,shlex,signal,struct,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1];sys.dont_write_bytecode=True
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--binary',type=Path,default=ROOT/'core/build/test_bridge_session');parser.add_argument('--case',action='append');parser.add_argument('--rpc',action='store_true');parser.add_argument('--resources',action='store_true');parser.add_argument('--source',action='store_true')
    parser.add_argument('--public-session',action='store_true')
    args=parser.parse_args();binary=args.binary.resolve();assert binary.is_file()
    if sum((args.rpc,args.resources,args.source))>1:parser.error('choose one application fixture')
    bulk=args.resources or args.source
    route=load('session_route_fixture',ROOT/'scripts/test-plugin-route.py');helpers=load('session_key_fixture',ROOT/'scripts/test-plugin-ssh.py');streams=load('session_stream_fixture',ROOT/'scripts/test-plugin-ssh-stream.py')
    if args.public_session and (args.rpc or bulk):parser.error('public Session lifecycle fixture has no application classes yet')
    label='ext_session' if args.public_session else 'bridge_source' if args.source else 'bridge_transfer' if args.resources else 'bridge_rpc' if args.rpc else 'bridge_session';work=Path(tempfile.mkdtemp(prefix='plugin-source-ssh-' if args.source else 'plugin-transfer-ssh-' if args.resources else 'plugin-rpc-ssh-' if args.rpc else 'plugin-session-ssh-',dir=ROOT/'tmp'));private,public=streams.memory_identity();cases=args.case or ['allow2','allow3','deny2','deny3'];assert all(case in ['allow2','allow3','deny2','deny3'] for case in cases)
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'};env.pop('TMUX',None);env.pop('TMUX_PANE',None)
    with helpers.owned_agent(work,'host-agent') as agent:
        keys=[helpers.add_memory_key(agent,'session-hop-'+str(i)) for i in range(3)];assert len({k.split()[1] for k in keys})==3
        for case in cases:
            count=int(case[-1]);mode='ssh-deny' if case.startswith('deny') else 'ssh-allow';folder=work/case;folder.mkdir()
            with route.chain(folder,count,public,keys,agent,'echo') as (ports,known,barrier):
                command=shlex.join([str(binary),'--executor',*([str(folder)] if bulk else [])]);budget=60 if bulk else 20;shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-session; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
                p=subprocess.run(['bash','-c',shell,'plugin-session',str(ROOT),str(budget),str(binary),mode,str(count),pwd.getpwuid(os.getuid()).pw_name,str(known),command,*map(str,ports),*([str(folder)] if bulk else [])],cwd=ROOT,env=env,input=struct.pack('>I',len(private))+private,capture_output=True,timeout=budget+5)
                output=(p.stdout+p.stderr).decode(errors='replace');(folder/'client.log').write_text(output)
                if p.returncode or f'PASS {label}_{mode}_{count}' not in output:raise RuntimeError(case+': '+output)
                print('PASS '+label+'_'+case,flush=True)
    for p in work.rglob('*'):
        if p.is_file() and (b'PRIVATE KEY-----' in p.read_bytes() or b'openssh-key-v1\0' in p.read_bytes()):raise RuntimeError('private fixture serialized')
    (work/'result.json').write_text(json.dumps(dict(accepted=True,cases=cases,binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),scope='two/three loopback sshd; scoped file producers in both directions, actual EOF/hash/256 KiB snapshots/receipts and 80 confirmed RPC echoes per allow case' if args.source else 'two/three loopback sshd; bidirectional 256 KiB resources with actual byte verification/receipts and 80 confirmed RPC echoes per allow case' if args.resources else 'two/three loopback sshd; 80 confirmed C RPC echo calls per allow case' if args.rpc else 'two/three loopback sshd; C Session handshake/consent/revoke; no App or business dispatch'),indent=2)+'\n')
    print('PLUGIN SESSION SSH PASS; logs: '+str(work),flush=True)
if __name__=='__main__':
    def interrupted(number,frame):raise KeyboardInterrupt('signal '+str(number))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted);main()
