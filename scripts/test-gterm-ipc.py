#!/usr/bin/env python3
"""One-shot foreground IPC, owned project fixtures only; never a user endpoint.

The C HOST owns the real public Session/consent/RPC. Raw sockets exercise only
transport admission/pumping and are not counted as authenticated plugin work.
"""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
CALL = ['fixture', '中文', '', "';throw Error('injected');//"]
def invocation_bytes(args):
    return len(args).to_bytes(4,'big') + b''.join(len(s.encode()).to_bytes(4,'big')+s.encode() for s in args)
MAGIC, ACK = b'GTERMIP2', b'GTERMOK2'
CASES = ['roundtrip', 'deny', 'cancel', 'active-cancel', 'peer-digest',
         'bad-permit', 'partial-permit', 'directory-mode', 'directory-link',
         'permit-mode', 'permit-link', 'permit-hardlink', 'socket-link', 'socket-mode',
         'stale', 'second-listener', 'one-shot', 'replacement-cleanup', 'bad-ack',
         'duplex', 'half-close', 'tty', 'idle-cancel', 'idle-hangup', 'preview', 'setup-deadline',
         'killed-owner', 'partial-cancel', 'directory-link-suffix', 'server-permit-link', 'server-socket-link',
         'instance', 'reused-directory', 'invocation-command', 'invocation-argument', 'invocation-count']


def require(value, label):
    if not value: raise AssertionError(label)


def wait(predicate, label):
    end = time.monotonic() + 6
    while time.monotonic() < end:
        if predicate(): return
        time.sleep(.01)
    raise RuntimeError('Observation timeout: ' + label)


def terminate(process):
    if process.poll() is None:
        process.terminate()
        try: process.wait(timeout=6)
        except subprocess.TimeoutExpired:
            process.kill(); process.wait(timeout=3)
            raise RuntimeError('Owned IPC process exceeded cleanup deadline')


def package(work):
    from runpy import run_path

    path = work / 'package'
    fixture = run_path(str(ROOT / 'scripts/test-gterm-stdio.py'))
    return path, fixture['build_fixture'](work, path, 'roundtrip')



def execute(build, case, work):
    binary = build/'gterm'; endpoint=work/'e'; endpoint.mkdir(mode=0o700)
    environment={**os.environ,'TMPDIR':str(ROOT/'tmp')}
    owned=[]
    current_instance='0'*32
    fixed_arguments=None
    @contextmanager
    def listener(arguments=None, terminal=False):
        nonlocal current_instance, fixed_arguments
        if arguments is None:
            if fixed_arguments is None:
                directory,digest=package(work)
                fixed_arguments=['--trust-development-digest',digest,'--client-digest','d'*64,'--method','fixture.echo','--',str(directory),*CALL]
            arguments=fixed_arguments
        master=slave=None
        if terminal: master,slave=pty.openpty()
        process=subprocess.Popen([binary,'relay','--listen',endpoint,*arguments],
            stdin=slave if terminal else subprocess.DEVNULL, stdout=slave if terminal else subprocess.PIPE,
            stderr=subprocess.PIPE, env=environment)
        owned.append(process)
        try:
            wait(lambda: (endpoint/'permit').exists() or process.poll() is not None,'owned listener startup')
            require(process.poll() is None,'listener must be live')
            require(stat.S_IMODE(endpoint.stat().st_mode)==0o700,'private root')
            wait(lambda:(endpoint/'permit').stat().st_size==56,'complete private permit')
            require(stat.S_IMODE((endpoint/'permit').stat().st_mode)==0o600,'private permit')
            require(stat.S_ISSOCK((endpoint/'channel.sock').lstat().st_mode),'owned socket')
            current_instance=(endpoint/'permit').read_bytes()[8:24].hex()
            yield process,master
        finally:
            terminate(process)
            if master is not None: os.close(master); os.close(slave)
            if process.stdout is not None: process.stdout.close()
            process.stderr.close()
    def attach(directory=endpoint, data=b'', instance=None, call=CALL):
        return subprocess.run([binary,'relay','--attach',directory,'--instance',instance or current_instance,'--',*call],input=data,capture_output=True,env=environment,timeout=8)
    def rejected(label):
        result=attach();require(result.returncode==5 and result.stdout==b'',label)
    try:
        if case in ('instance','invocation-command','invocation-argument','invocation-count','reused-directory'):
            previous=None
            if case=='reused-directory':
                with listener() as (process,_):
                    previous=current_instance;process.terminate();require(process.wait(timeout=4)==130,'old instance revoked')
            with listener() as (process,_):
                call=list(CALL);instance=current_instance
                if case=='reused-directory':
                    require(instance!=previous,'fresh instance in reused directory');instance=previous
                elif case=='instance':instance=('1' if instance[0]!='1' else '0')+instance[1:]
                elif case=='invocation-command':call[0]='other'
                elif case=='invocation-argument':call[1]='汉字'
                else:call.append('--')
                result=attach(instance=instance,call=call)
                require(result.returncode==5 and not result.stdout,'instance / invocation mismatch rejected')
                require(process.poll() is None and (endpoint/'permit').exists(),'mismatch must not consume or activate frontend')
            return
        if case=='preview':
            directory = work / 'preview-package'
            subprocess.run(['python3', '-B', ROOT / 'scripts/gterm-plugin', 'build',
                            ROOT / 'plugins/official/preview', '--out', directory],
                           cwd=ROOT, check=True, timeout=30)
            digest = hashlib.sha256((directory / 'integrity.json').read_bytes()).hexdigest()
            workspace=work/'workspace';site=workspace/'site';site.mkdir(parents=True)
            (site/'index.md').write_text('# Preview runtime fixture\n')
            (site/'theme,dark.css').write_text('body { color: rgb(1, 2, 3); }\n')
            store=work/'store';store.mkdir(mode=0o700)
            arguments=['--trust-development-digest',digest,'--client-digest','d'*64,'--root','workspace',str(workspace),
                '--method','preview.open','--',str(directory),'preview','site/index.md','--assets','site/theme\\,dark.css']
            with listener(arguments,terminal=True) as (process,master):
                result=subprocess.run([build/'test_gterm_stdio',binary,directory,digest,'preview',workspace,store],
                    capture_output=True,env={**environment,'GT_GTERM_FIXTURE_ATTACH':str(endpoint),'GT_GTERM_FIXTURE_ATTACH_INSTANCE':current_instance},timeout=18)
                require(result.returncode==0,'Preview complete resource assertions: '+result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace'))
                require(process.wait(timeout=6)==0,'Preview foreground completion')
                require(not select.select([master],[],[],.02)[0],'Preview bytes stay outside terminal')
            return
        if case in ('setup-deadline','killed-owner','partial-cancel'):
            with listener() as (process,_):
                if case=='setup-deadline':
                    started=time.monotonic();require(process.wait(timeout=33)==5,'unclaimed endpoint expires')
                    require(28 <= time.monotonic()-started <= 33,'30 second setup budget')
                    require(not list(endpoint.iterdir()),'deadline endpoint cleanup')
                elif case=='killed-owner':
                    process.kill();require(process.wait(timeout=4)==-9,'owned fixture SIGKILL observed')
                    rejected('dead owner cannot be attached or replayed')
                    require((endpoint/'permit').exists(),'SIGKILL residue is not silently deleted')
                else:
                    with socket.socket(socket.AF_UNIX) as peer:
                        peer.settimeout(3);peer.connect(str(endpoint/'channel.sock'));peer.sendall(b'G')
                        process.terminate();require(process.wait(timeout=4)==130,'partial admission cancellation')
                        try:data=peer.recv(8)
                        except ConnectionResetError:data=b''
                        require(not data,'partial admission never ACKs')
                    require(not list(endpoint.iterdir()),'partial admission cleanup')
            return
        if case in ('roundtrip','deny','cancel','active-cancel','peer-digest'):
            directory,digest=package(work)
            arguments=['--trust-development-digest',digest,'--client-digest','d'*64,'--method','fixture.echo',
                '--',str(directory),'fixture','中文','',"';throw Error('injected');//"]
            with listener(arguments,terminal=True) as (process,master):
                env={**environment,'GT_GTERM_FIXTURE_ATTACH':str(endpoint),'GT_GTERM_FIXTURE_ATTACH_INSTANCE':current_instance}
                result=subprocess.run([build/'test_gterm_stdio',binary,directory,digest,case],capture_output=True,env=env,timeout=18)
                if result.returncode:
                    terminate(process)
                    raise AssertionError('native Session assertions: '+result.stdout.decode(errors='replace')+result.stderr.decode(errors='replace')+' backend='+str(process.returncode)+' '+process.stderr.read().decode(errors='replace'))
                expected=6 if case=='deny' else 7 if case in ('cancel','active-cancel','peer-digest') else 0
                require(process.wait(timeout=6)==expected,'foreground backend exit fact')
                require(not (endpoint/'permit').exists() and not (endpoint/'channel.sock').exists(),'endpoint revoked')
                require(not select.select([master],[],[],.02)[0],'no protocol or permit on terminal')
            return
        if case in ('directory-mode','directory-link','directory-link-suffix'):
            with listener():
                if case=='directory-mode':endpoint.chmod(0o750)
                else:
                    alias=work/'alias';alias.symlink_to(endpoint,target_is_directory=True)
                    if case=='directory-link-suffix':
                        for suffix in ['/', '/.']:
                            result=attach(str(alias)+suffix);require(result.returncode==5 and not result.stdout,'directory link suffix must fail')
                        return
                    result=attach(alias);require(result.returncode==5 and not result.stdout,'directory symlink must fail');return
                try:rejected('directory mode must fail')
                finally:endpoint.chmod(0o700)
            return
        if case in ('permit-mode','permit-hardlink','socket-mode'):
            with listener():
                name='channel.sock' if case.startswith('socket') else 'permit';path=endpoint/name
                if case.endswith('mode'):
                    old=stat.S_IMODE(path.stat().st_mode);path.chmod(0o644)
                    try:rejected('unsafe member mode must fail')
                    finally:
                        if path.exists():path.chmod(old)
                elif case=='permit-hardlink':
                    os.link(path,work/'copied-permit');rejected('hardlinked permit must fail')
                else:
                    saved=endpoint/(name+'.saved');path.rename(saved);path.symlink_to(saved.name)
                    try:rejected('member link must fail')
                    finally:path.unlink();saved.rename(path)
            return
        if case in ('server-permit-link','server-socket-link'):
            with listener():
                permit=(endpoint/'permit').read_bytes()
                name='permit' if case=='server-permit-link' else 'channel.sock'
                path=endpoint/name;saved=endpoint/(name+'.saved');path.rename(saved);path.symlink_to(saved.name)
                try:
                    with socket.socket(socket.AF_UNIX) as peer:
                        peer.settimeout(3);peer.connect(str(endpoint/('channel.sock.saved' if name=='channel.sock' else 'channel.sock')))
                        peer.sendall(permit+invocation_bytes(CALL));require(peer.recv(8)==b'','server must reject replaced entry')
                finally:
                    if path.is_symlink():path.unlink()
                    if saved.exists():saved.rename(path)
            return
        if case in ('permit-link','socket-link'):
            permit=MAGIC+os.urandom(48);current_instance=permit[8:24].hex();permit_path=endpoint/'permit';permit_path.write_bytes(permit);permit_path.chmod(0o600)
            native=socket.socket(socket.AF_UNIX);native.bind(str(endpoint/'channel.sock'));native.listen(1);native.settimeout(.05)
            (endpoint/'channel.sock').chmod(0o600)
            observed=bytearray();failures=[]
            def attempt():
                stop=threading.Event()
                def peer():
                    try:
                        while not stop.is_set():
                            try:connection=native.accept()[0]
                            except socket.timeout:continue
                            with connection:
                                connection.settimeout(3)
                                while len(observed)<56:
                                    data=connection.recv(56-len(observed))
                                    if not data:break
                                    observed.extend(data)
                                expected=invocation_bytes(CALL);prelude=bytearray()
                                while len(prelude)<len(expected):
                                    data=connection.recv(len(expected)-len(prelude));require(data,'incomplete expected invocation');prelude.extend(data)
                                require(bytes(prelude)==expected,'literal invocation prelude')
                                connection.sendall(ACK)
                            return
                    except BaseException as error:failures.append(error)
                thread=threading.Thread(target=peer,daemon=True);thread.start()
                try:return attach()
                finally:
                    stop.set();thread.join(timeout=4)
                    require(not thread.is_alive() and not failures,'owned link peer released')
            try:
                attempt();require(bytes(observed)==permit,'link peer must pass unchanged admission control')
                observed.clear()
                path=permit_path if case=='permit-link' else endpoint/'channel.sock'
                saved=endpoint/(path.name+'.saved');path.rename(saved);path.symlink_to(saved.name)
                result=attempt()
                require(result.returncode==5 and not result.stdout and not observed,'linked client entry cannot send permit')
            finally:native.close()
            return
        if case in ('bad-permit','partial-permit','one-shot'):
            arguments=None
            if case=='one-shot':
                directory,digest=package(work)
                arguments=['--trust-development-digest',digest,'--client-digest','d'*64,'--method','fixture.echo','--',str(directory),'fixture','中文','',"';throw Error('injected');//"]
            with listener(arguments) as (process,_):
                permit=(endpoint/'permit').read_bytes()
                with socket.socket(socket.AF_UNIX) as client:
                    client.settimeout(3);client.connect(str(endpoint/'channel.sock'))
                    if case=='bad-permit':client.sendall(permit[:-1]+bytes([permit[-1]^1])+invocation_bytes(CALL))
                    elif case=='partial-permit':client.sendall(permit[:12]);client.shutdown(socket.SHUT_WR)
                    else:client.sendall(permit+invocation_bytes(CALL))
                    data=client.recv(8)
                    if case=='one-shot':
                        require(data==ACK,'one-shot acknowledged')
                        require(not (endpoint/'permit').exists() and not (endpoint/'channel.sock').exists(),'admission removes endpoint before protocol')
                        rejected('second attach must fail')
                        client.shutdown(socket.SHUT_WR)
                        require(process.wait(timeout=6)==7,'transport admission cannot stand in for Session authorization')
                    else:
                        require(data==b'','bad permit cannot receive ACK')
                        require(process.poll() is None and (endpoint/'permit').read_bytes()==permit,'rejected peer cannot consume instance')
            return
        if case in ('stale','second-listener','replacement-cleanup','idle-cancel','idle-hangup'):
            if case=='stale':
                (endpoint/'permit').write_bytes(b'old-owned-fixture')
                result=subprocess.run([binary,'relay','--listen',endpoint],capture_output=True,env=environment,timeout=4)
                require(result.returncode==5 and (endpoint/'permit').read_bytes()==b'old-owned-fixture','stale entry preserved');return
            with listener() as (process,_):
                original=(endpoint/'permit').read_bytes()
                if case=='second-listener':
                    result=subprocess.run([binary,'relay','--listen',endpoint],capture_output=True,env=environment,timeout=4)
                    require(result.returncode==5 and (endpoint/'permit').read_bytes()==original,'second listener cannot replace endpoint')
                elif case=='replacement-cleanup':
                    (endpoint/'permit').rename(endpoint/'original')
                    (endpoint/'permit').write_bytes(b'replacement-owned-fixture')
                    terminate(process)
                    require(process.returncode==5 and (endpoint/'permit').exists() and (endpoint/'permit').read_bytes()==b'replacement-owned-fixture','cleanup failure must preserve replacement')
                else:
                    process.send_signal(signal.SIGHUP if case=='idle-hangup' else signal.SIGTERM)
                    require(process.wait(timeout=4)==130,'idle cancellation observed')
                    require(not list(endpoint.iterdir()),'idle cancellation cleanup')
            return
        if case=='tty':
            with listener():
                for direction in ['input','output']:
                    master,slave=pty.openpty()
                    try:
                        result=subprocess.run([binary,'relay','--attach',endpoint,'--instance',current_instance,'--',*CALL],stdin=slave if direction=='input' else subprocess.PIPE,
                            stdout=slave if direction=='output' else subprocess.PIPE,stderr=subprocess.PIPE,env=environment,timeout=4)
                        require(result.returncode==5,'TTY attach must fail')
                        require((endpoint/'permit').exists(),'TTY rejection cannot consume endpoint')
                        require(not select.select([master],[],[],.02)[0],'no protocol to TTY')
                    finally:os.close(master);os.close(slave)
            return
        if case in ('bad-ack','duplex','half-close'):
            permit=MAGIC+os.urandom(48);current_instance=permit[8:24].hex();path=endpoint/'permit';path.write_bytes(permit);path.chmod(0o600)
            listener_socket=socket.socket(socket.AF_UNIX);listener_socket.bind(str(endpoint/'channel.sock'))
            (endpoint/'channel.sock').chmod(0o600);listener_socket.listen(1);listener_socket.settimeout(5)
            incoming=bytes(i%251 for i in range(1048576));outgoing=bytes((i*7+3)%251 for i in range(1048576))
            failures=[];received=bytearray()
            def serve():
                try:
                    with listener_socket.accept()[0] as peer:
                        peer.settimeout(6);header=bytearray()
                        while len(header)<56:
                            chunk=peer.recv(56-len(header));require(chunk,'owned admission ended before complete header');header.extend(chunk)
                        require(bytes(header)==permit,'private admission bytes')
                        expected=invocation_bytes(CALL);prelude=bytearray()
                        while len(prelude)<len(expected):
                            data=peer.recv(len(expected)-len(prelude));require(data,'incomplete expected invocation');prelude.extend(data)
                        require(bytes(prelude)==expected,'literal invocation prelude')
                        peer.sendall(b'INVALID!' if case=='bad-ack' else ACK)
                        if case=='bad-ack':require(peer.recv(1)==b'','bad ACK must block protocol');return
                        writer=None
                        if case=='duplex':
                            def write():
                                try:peer.sendall(outgoing)
                                except BaseException as error:failures.append(error)
                            writer=threading.Thread(target=write);writer.start()
                        while True:
                            data=peer.recv(32768)
                            if not data:break
                            received.extend(data)
                        if writer:writer.join(timeout=6);require(not writer.is_alive(),'duplex writer release')
                        else:peer.sendall(outgoing)
                        peer.shutdown(socket.SHUT_WR)
                except BaseException as error:failures.append(error)
            thread=threading.Thread(target=serve,daemon=True);thread.start()
            try:
                result=attach(data=incoming)
                thread.join(timeout=8);require(not thread.is_alive() and not failures,'owned transport peer: '+repr(failures))
                if case=='bad-ack':require(result.returncode==5 and not result.stdout,'bad ACK must fail')
                else:require(result.returncode==0 and result.stdout==outgoing and received==incoming,'full duplex bytes and half-close')
            finally:listener_socket.close()
            return
        raise RuntimeError('Unimplemented IPC case')
    finally:
        for process in owned:terminate(process)


def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--case',choices=CASES);parser.add_argument('--receipt',type=Path);args=parser.parse_args()
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT/'tmp'):raise RuntimeError('Receipt must stay in project tmp')
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    inputs={str(p.relative_to(ROOT)):digest(p) for folder in ['cli/gterm','core/src','core/include','plugins/official/preview'] for p in (ROOT/folder).rglob('*') if p.is_file()}
    inputs[str(Path(__file__).relative_to(ROOT))]=digest(Path(__file__))
    binaries={str(p.resolve()):digest(p) for p in [args.build/'gterm',args.build/'test_gterm_stdio',*([args.build/'core/libghosterm_core.a'] if (args.build/'core/libghosterm_core.a').exists() else [])]}
    for case in [args.case] if args.case else CASES:
        scratch = Path(os.environ.get('TMPDIR', ROOT / 'tmp')).resolve()
        if not scratch.is_dir() or not scratch.is_relative_to(ROOT / 'tmp'):
            scratch = ROOT / 'tmp'
        with tempfile.TemporaryDirectory(prefix='gi-',dir=scratch) as temporary:
            execute(args.build.resolve(strict=True),case,Path(temporary))
        print('PASS IPC '+case,flush=True)
    require(all(digest(ROOT/name)==value for name,value in inputs.items()),'source inputs changed during IPC conformance')
    require(all(digest(Path(name))==value for name,value in binaries.items()),'actual binaries changed during IPC conformance')
    if args.receipt:args.receipt.write_text(json.dumps(dict(accepted=True,cases=[args.case] if args.case else CASES,inputs=inputs,binaries=binaries,scope='owned macOS foreground CLI and native Core Host; no App or SSH attach'),indent=2)+'\n')
    print('GTERM IPC PASS',flush=True)


if __name__=='__main__':
    def stop(number,frame):raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop)
    try:main()
    except AssertionError as error:
        print('FAIL IPC '+str(error),flush=True);sys.exit(1)
