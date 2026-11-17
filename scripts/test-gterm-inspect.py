#!/usr/bin/env python3
"""Live foreground declaration over owned private IPC; no user endpoint."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import pty
import select
import socket
import subprocess
import sys
import tempfile
import threading
import time

from importlib.machinery import SourceFileLoader
ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
ipc = SourceFileLoader('gterm_ipc_fixture', str(ROOT/'scripts/test-gterm-ipc.py')).load_module()
CASES = ['roundtrip', 'declaration-only', 'repeat', 'wrong-instance', 'bad-permit',
         'dead-owner', 'tty-input', 'tty-output', 'private-fields', 'invalid-utf8',
         'duplicate-method', 'attempt-budget', 'deadline', 'bad-magic', 'oversize',
         'truncated', 'trailing', 'wire-utf8', 'wire-count', 'wire-digest', 'wire-duplicate']
require = ipc.require


def payload():
    def text(s):
        b = s.encode()+b'\0'
        return len(b).to_bytes(4, 'big')+b
    def vector(values):
        return len(values).to_bytes(4, 'big')+b''.join(text(s) for s in values)
    return b''.join(text(s) for s in ['portable-js', 'a'*64, 'd'*64, 'fixture']) + vector(['中文', '', "';throw Error('injected');//"]) + vector(['fixture.echo']) + vector([]) + vector([])


def execute(build, case, work):
    binary = build/'gterm'
    endpoint = work/'e'; endpoint.mkdir(mode=0o700)
    env = {**os.environ, 'TMPDIR': str(work)}
    processes = []
    instance = None
    @contextmanager
    def listener(extra=None, nonexistent=False):
        nonlocal instance
        package, digest = ipc.package(work)
        master, slave = pty.openpty()
        argv = ['--trust-development-digest', digest, '--client-digest', 'd'*64, '--method', 'fixture.echo']
        if extra: argv += extra
        argv += ['--', str(work/'missing') if nonexistent else str(package), *ipc.CALL]
        process = subprocess.Popen([binary, 'relay', '--listen', endpoint, *argv], stdin=slave, stdout=slave, stderr=subprocess.PIPE, env=env)
        processes.append(process)
        try:
            ipc.wait(lambda: (endpoint/'permit').exists() and (endpoint/'permit').stat().st_size == 56 or process.poll() is not None, 'inspector listener ready')
            require(process.poll() is None, 'inspection foreground is live')
            permit = (endpoint/'permit').read_bytes(); instance = permit[8:24].hex()
            yield process, master, package, digest, permit
        finally:
            ipc.terminate(process); os.close(master); os.close(slave)
            process.stderr.close()

    def query(public=None, **kwargs):
        return subprocess.run([binary, 'relay', '--inspect', endpoint, '--instance', public or instance],
                              input=b'', capture_output=True, env=env, timeout=8, **kwargs)

    def accepted(result):
        require(result.returncode == 0, 'live inspection succeeds: '+result.stderr.decode(errors='replace'))
        require(all(b < 127 and (b >= 32 or b == 10) for b in result.stdout), 'JSON never emits raw terminal controls / unicode')
        value = json.loads(result.stdout)
        require(value['schemaVersion'] == 1 and value['authorizationGranted'] is False and value['instance'] == instance, 'declaration exact instance without grant')
        require(value['command'] == ipc.CALL[0] and value['arguments'] == ipc.CALL[1:], 'inspection literal command argv')
        return value

    try:
        if case.startswith('wire-') or case in ['bad-magic', 'oversize', 'truncated', 'trailing']:
            permit = b'GTERMIP2'+bytes(range(48)); instance = permit[8:24].hex()
            (endpoint/'permit').write_bytes(permit); (endpoint/'permit').chmod(0o600)
            server = socket.socket(socket.AF_UNIX); server.bind(str(endpoint/'channel.sock')); (endpoint/'channel.sock').chmod(0o700); server.listen(1); server.settimeout(8)
            failure = []
            def serve():
                try:
                    peer, _ = server.accept()
                    with peer:
                        peer.settimeout(3); data = b''
                        while len(data) < 56:
                            part = peer.recv(56-len(data))
                            if not part: break
                            data += part
                        require(data == b'GTERMQ01'+permit[8:], 'query uses secret only over selected socket')
                        body = payload(); magic = b'GTERMD01'; length = len(body)
                        if case == 'bad-magic': magic = b'GTERMOK2'
                        elif case == 'oversize': length = 24577
                        elif case == 'truncated': body = body[:-1]
                        elif case == 'trailing': body += b'X'
                        elif case == 'wire-utf8': body = body.replace('中文'.encode(), b'\xc0\x80ABCD')
                        elif case == 'wire-count': body = body[:-4]+(5).to_bytes(4, 'big')
                        elif case == 'wire-digest': body = body.replace(b'a'*64, b'G'*64)
                        elif case == 'wire-duplicate': body = body[:-8]+(1).to_bytes(4,'big')+(13).to_bytes(4,'big')+b'fixture.echo\0'+body[-4:]; length = len(body)
                        peer.sendall(magic+length.to_bytes(4,'big')+body)
                except Exception as e: failure.append(e)
                finally: server.close()
            thread = threading.Thread(target=serve); thread.start()
            try:
                result = query(); require(result.returncode == 5 and not result.stdout, 'malformed live declaration rejected before JSON')
            finally: thread.join(timeout=10)
            require(not thread.is_alive() and not failure, 'fake owned query peer completed: '+str(failure))
            return
        extra = []
        if case == 'private-fields':
            extra = ['--runtime', 'exec', '--allow-process-execution', '--root', 'workspace', '/not-opened-secret-root',
                     '--process-program', 'helper', '/secret-program', 'v1', 'e'*64,
                     '--process-argument', 'helper', 'secret-fixed-argument', '--process-environment', 'SECRET_ENV']
            env['SECRET_ENV'] = 'secret-value'
        elif case == 'duplicate-method': extra = ['--host-method', 'fixture.echo']
        with listener(extra, nonexistent=case=='declaration-only') as (process, master, package, digest, permit):
            start = time.monotonic()
            inode = (endpoint/'permit').stat().st_ino
            if case == 'wrong-instance':
                result = query('f'*32 if instance != 'f'*32 else '0'*32)
                require(result.returncode == 5 and not result.stdout, 'inspection requires exact public instance')
            elif case == 'bad-permit':
                with socket.socket(socket.AF_UNIX) as peer:
                    peer.settimeout(3); peer.connect(str(endpoint/'channel.sock'))
                    peer.sendall(b'GTERMQ01'+permit[8:-1]+bytes([permit[-1]^1]))
                    require(not peer.recv(12), 'query requires full private permit')
                accepted(query())
            elif case == 'dead-owner':
                process.kill(); require(process.wait(timeout=4) == -9, 'owned SIGKILL observed separately')
                result = query(); require(result.returncode == 5 and not result.stdout, 'stale permit never proves live instance')
            elif case in ['tty-input', 'tty-output']:
                m, s = pty.openpty()
                try:
                    options = dict(stdin=s, stdout=subprocess.PIPE) if case=='tty-input' else dict(stdin=subprocess.PIPE, stdout=s)
                    result = subprocess.run([binary, 'relay', '--inspect', endpoint, '--instance', instance], stderr=subprocess.PIPE, env=env, timeout=5, **options)
                    require(result.returncode == 5, 'inspection refuses terminal descriptor')
                    require(not select.select([m],[],[],.03)[0], 'no declaration on terminal')
                finally: os.close(m); os.close(s)
            elif case == 'invalid-utf8':
                # Listener's argument bytes are replaced in a separate owned subprocess below.
                process.terminate(); require(process.wait(timeout=4)==130, 'original listener stopped')
                argv=[os.fsencode(binary), b'relay', b'--listen', os.fsencode(endpoint), b'--trust-development-digest', digest.encode(), b'--client-digest', b'd'*64, b'--', os.fsencode(package), b'fixture', b'\xc0\x80']
                malformed=subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env); processes.append(malformed)
                ipc.wait(lambda:(endpoint/'permit').exists() and (endpoint/'permit').stat().st_size==56, 'malformed listener ready')
                instance=(endpoint/'permit').read_bytes()[8:24].hex()
                result=query(); require(result.returncode==5 and not result.stdout, 'invalid UTF-8 never aliases JSON values')
                ipc.terminate(malformed); malformed.stdout.close(); malformed.stderr.close()
                return
            elif case == 'duplicate-method':
                result=query(); require(result.returncode==5 and not result.stdout, 'overlapping declaration methods rejected')
            elif case == 'attempt-budget':
                for _ in range(16): accepted(query())
                require(process.wait(timeout=4)==5, 'queries count toward finite admission attempts')
                require(not list(endpoint.iterdir()), 'attempt exhaustion revokes endpoint'); return
            else:
                value = accepted(query())
                require(value['executorDigest']==digest and value['clientDigest']=='d'*64, 'fixed digests preserved')
                if case == 'repeat':
                    for _ in range(3): require(accepted(query())==value, 'inspection is repeatable without activation')
                if case == 'private-fields':
                    require(value['runtime']=='exec', 'declared external runtime visible without execution')
                    require(not any(b in json.dumps(value) for b in ['secret', 'SECRET', str(package), str(endpoint)]), 'private paths environment and program arguments excluded')
                if case == 'deadline':
                    time.sleep(2); accepted(query())
                    require(process.wait(timeout=31)==5 and time.monotonic()-start < 31.5, 'inspection never extends original 30s deadline'); return
                if case == 'roundtrip':
                    result=subprocess.run([build/'test_gterm_stdio',binary,package,digest,'roundtrip'],capture_output=True,
                        env={**env,'GT_GTERM_FIXTURE_ATTACH':str(endpoint),'GT_GTERM_FIXTURE_ATTACH_INSTANCE':instance},timeout=18)
                    require(result.returncode==0,'inspection followed by real Core consent/RPC: '+result.stderr.decode(errors='replace'))
                    require(process.wait(timeout=6)==0,'inspected foreground completed normally')
                    require(not list(endpoint.iterdir()),'attached endpoint revoked'); return
            if case != 'dead-owner':
                require(process.poll() is None and (endpoint/'permit').read_bytes()==permit and (endpoint/'permit').stat().st_ino==inode,
                        'inspection neither consumes capability nor activates endpoint')
            require(not select.select([master],[],[],.03)[0], 'inspection never writes business bytes to foreground TTY')
    finally:
        for process in processes: ipc.terminate(process)


def main():
    parser=argparse.ArgumentParser(description=__doc__); parser.add_argument('--build',type=Path,required=True)
    parser.add_argument('--case',choices=CASES); parser.add_argument('--receipt',type=Path); args=parser.parse_args()
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT/'tmp'): raise RuntimeError('Receipt must remain in project tmp')
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    inputs={str(p.relative_to(ROOT)):digest(p) for folder in ['cli/gterm','core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file()}
    for p in [Path(__file__), ROOT/'scripts/test-gterm-ipc.py']: inputs[str(p.relative_to(ROOT))]=digest(p)
    binaries={str((args.build/name).resolve()):digest(args.build/name) for name in ['gterm','test_gterm_stdio','core/libghosterm_core.a']}
    cases=[args.case] if args.case else CASES
    parent=ROOT/'tmp/p2-inspect'; parent.mkdir(exist_ok=True)
    for case in cases:
        with tempfile.TemporaryDirectory(prefix='i-',dir=parent) as work: execute(args.build.resolve(),case,Path(work))
        print('PASS inspection '+case,flush=True)
    require(all(digest(ROOT/name)==value for name,value in inputs.items()),'inspection source inputs unchanged')
    require(all(digest(Path(name))==value for name,value in binaries.items()),'actual inspection binaries unchanged')
    if args.receipt: args.receipt.write_text(json.dumps(dict(accepted=True,cases=cases,inputs=inputs,binaries=binaries),indent=2)+'\n')


if __name__=='__main__':
    try: main()
    except AssertionError as error: print('FAIL inspection '+str(error),flush=True); raise SystemExit(1)
