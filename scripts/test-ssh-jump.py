#!/usr/bin/env python3
"""Native PTY/SFTP through owned loopback SSH chains; private keys stay in memory."""
import argparse, hashlib, importlib.util, json, os, pwd, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
from contextlib import ExitStack
sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]

def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT/'scripts'/file)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--case', choices=['pty', 'pty-echo', 'pty-idle', 'pty-cancel', 'sftp', 'cancel', 'open-cancel', 'wait-error', 'read-wait-error'], action='append')
    parser.add_argument('--hops', type=int, choices=[2, 3], action='append')
    parser.add_argument('--public-route-owner', action='store_true')
    parser.add_argument('--route-owner', action='store_true')
    parser.add_argument('--expect-assertion', action='store_true')
    args = parser.parse_args(); binary = args.binary.resolve(strict=True)
    if args.expect_assertion and (not args.case or len(args.case) != 1 or not args.hops or len(args.hops) != 1): parser.error('assertion mode requires exactly one hop count and case')
    sources = [ROOT/'core/src/ssh.c', ROOT/'core/src/sftp.c', ROOT/'core/tests/test_ssh_jump.c', Path(__file__).resolve()] + [ROOT/'scripts'/name for name in ['test-plugin-route.py', 'test-plugin-ssh.py', 'test-plugin-ssh-stream.py']]
    sources += [ROOT/'core/CMakeLists.txt'] + [p for folder in ['core/src', 'core/include'] for p in (ROOT/folder).rglob('*') if p.is_file()]
    hashes = {str(p.relative_to(ROOT)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    work = Path(tempfile.mkdtemp(prefix='sj-', dir=ROOT/'tmp')); logs = work/'logs'; logs.mkdir()
    routes = load('jump_routes', 'test-plugin-route.py')
    helpers = load('jump_keys', 'test-plugin-ssh.py'); streams = load('jump_streams', 'test-plugin-ssh-stream.py')
    private, public = streams.memory_identity(); results = []
    with ExitStack() as stack:
        agent = stack.enter_context(helpers.owned_agent(work, 'agent'))
        keys = [helpers.add_memory_key(agent, 'jump-'+str(i)) for i in range(3)]
        for count in args.hops or [2, 3]:
            folder = work/str(count); folder.mkdir()
            ports, known, _ = stack.enter_context(routes.chain(folder, count, public, keys, agent, 'echo', sftp=True))
            fixture = folder/'payload'; fixture.write_bytes(bytes((i * 17 + 3) & 255 for i in range(262144)))
            echo = folder/'payload.echo'
            echo.write_text('#!'+sys.executable+'\nimport os, tty, sys, time\ntty.setraw(0)\nos.write(1,b"JUMP_READY\\n")\nif sys.argv[1]=="idle": time.sleep(30)\ndata=bytearray()\nwhile len(data)<262144:\n chunk=os.read(0,262144-len(data))\n if not chunk: raise SystemExit(2)\n data.extend(chunk)\nwhile data:\n n=os.write(1,data)\n del data[:n]\n')
            echo.chmod(0o700)
            for case in args.case or ['pty', 'pty-echo', 'pty-idle', 'pty-cancel', 'sftp', 'cancel', 'open-cancel', 'wait-error', 'read-wait-error']:
                label = str(count)+'-'+case
                command = [str(binary), case, str(fixture), str(known), pwd.getpwuid(os.getuid()).pw_name, *map(str, ports)]
                try:
                    result = subprocess.run(command, input=private, capture_output=True, timeout=30,
                                            env={**{k:v for k,v in os.environ.items() if k not in ['GT_TEST_ROUTE_OWNER','GT_TEST_PUBLIC_ROUTE_OWNER']}, **({'GT_TEST_ROUTE_OWNER':'1'} if args.route_owner else {}), **({'GT_TEST_PUBLIC_ROUTE_OWNER':'1'} if args.public_route_owner else {}), 'TMPDIR': str(work)})
                except subprocess.TimeoutExpired as error:
                    (logs/(label+'.log')).write_bytes((error.stdout or b'')+(error.stderr or b''))
                    raise
                (logs/(label+'.log')).write_bytes(result.stdout + result.stderr)
                if args.expect_assertion:
                    accepted = result.returncode == 1 and result.stderr.count(b'FAIL jump:') == 1 and not any(marker in result.stderr for marker in [b'AddressSanitizer', b'runtime error:', b'UndefinedBehaviorSanitizer'])
                else: accepted = result.returncode == 0 and b'SSH JUMP IO PASS' in result.stdout
                if not accepted: raise AssertionError(label+' failed: '+str(logs))
                results.append(label); print(label+(': assertion red' if args.expect_assertion else ': PASS'), flush=True)
    for path in work.rglob('*'):
        if path.is_file():
            data = path.read_bytes()
            assert private not in data and json.dumps(private.decode())[1:-1].encode() not in data
    assert hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash
    for name, digest in hashes.items(): assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest() == digest
    (logs/'result.json').write_text(json.dumps({'accepted': True, 'route_owner': args.route_owner, 'public_route_owner': args.public_route_owner, 'expected_assertion': args.expect_assertion, 'cases': results,
        'inputs_sha256': hashes, 'binary_sha256': binary_hash}, indent=2)+'\n')
    for child in work.iterdir():
        if child != logs:
            if child.is_dir(): shutil.rmtree(child)
            else: child.unlink()
    print(('SSH JUMP ASSERTION PASS: ' if args.expect_assertion else 'SSH JUMP IO PASS: ')+str(logs))

if __name__ == '__main__':
    def interrupt(signum, frame): raise KeyboardInterrupt(signum)
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupt)
    main()
