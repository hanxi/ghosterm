#!/usr/bin/env python3
"""Dedicated device, owned loopback SSH chains, memory-only private key fixture."""
import argparse, hashlib, re, importlib.util, json, os, pwd, secrets, shutil, socketserver, subprocess, tempfile, threading
from contextlib import ExitStack
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
def load(name, file):
    spec = importlib.util.spec_from_file_location(name, ROOT/'scripts'/file)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--method');parser.add_argument('--hops',type=int,choices=[2,3]);parser.add_argument('--expect-assertion',action='store_true')
    args=parser.parse_args()
    if args.expect_assertion and not (args.method and args.hops): parser.error('one method and hop count required for mutation')
    sources=[p for scope in ['android/app/src','android/bridge/src','core/src','core/include'] for p in (ROOT/scope).rglob('*') if p.is_file()]
    hashes={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    serial = os.environ['ANDROID_SERIAL']
    adb = [str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'), '-s', serial]
    work = Path(tempfile.mkdtemp(prefix='aj-', dir=ROOT/'tmp')); logs=work/'logs'; logs.mkdir()
    routes=load('aj_routes','test-plugin-route.py'); helpers=load('aj_keys','test-plugin-ssh.py'); streams=load('aj_streams','test-plugin-ssh-stream.py')
    private, public=streams.memory_identity()
    token=secrets.token_hex(24); payload={}
    def run(args, label):
        result=subprocess.run(args,cwd=ROOT,capture_output=True,text=True,timeout=180)
        (logs/(label+'.log')).write_text(result.stdout+result.stderr)
        if result.returncode: raise RuntimeError((label, result.stdout+result.stderr))
        return result.stdout+result.stderr
    class Handler(socketserver.StreamRequestHandler):
        def handle(self):
            if self.rfile.readline(256).strip().decode()==token:
                self.wfile.write(json.dumps(payload).encode()+b'\n')
    with ExitStack() as stack:
        server=stack.enter_context(socketserver.TCPServer(('127.0.0.1',0),Handler))
        thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
        stack.callback(server.shutdown)
        fixture_port=server.server_address[1]
        run([*adb,'reverse',f'tcp:{fixture_port}',f'tcp:{fixture_port}'],'fixture-reverse')
        stack.callback(lambda: subprocess.run([*adb,'reverse','--remove',f'tcp:{fixture_port}'],capture_output=True))
        for name,path in [('app','app/build/outputs/apk/debug/app-debug.apk'),('test','app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk')]:
            run([*adb,'install','-r','-t',str(ROOT/'android'/path)],'install-'+name)
        agent=stack.enter_context(helpers.owned_agent(work,'agent'))
        keys=[helpers.add_memory_key(agent,'android-jump-'+str(i)) for i in range(3)]
        tmux=shutil.which('tmux'); assert tmux
        for count in ([args.hops] if args.hops else [2,3]):
            folder=work/str(count);folder.mkdir()
            ports,_,_=stack.enter_context(routes.chain(folder,count,public,keys,agent,'echo',sftp=True))
            (folder/'payload').write_bytes(b'owned Android route fixture')
            socket=folder/'tmux-route'
            stack.callback(lambda socket=socket: subprocess.run([tmux,'-S',str(socket),'kill-server'],capture_output=True))
            run([*adb,'reverse',f'tcp:{ports[0]}',f'tcp:{ports[0]}'],f'{count}-reverse')
            stack.callback(lambda port=ports[0]: subprocess.run([*adb,'reverse','--remove',f'tcp:{port}'],capture_output=True))
            payload.update(ports=ports,user=pwd.getpwuid(os.getuid()).pw_name,pem=private.decode(),socket=str(socket),tmux=tmux)
            output=run([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.ProxyJumpTest'+('#'+args.method if args.method else ''),'-e','fixturePort',str(fixture_port),'-e','fixtureToken',token,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],f'{count}-tests')
            if args.expect_assertion:
                valid=bool(re.search(r'Tests run: 1,\s+Failures: 1',output)) and any(x in output for x in ['java.lang.AssertionError','org.junit.ComparisonFailure'])
                valid=valid and not any(x in output for x in ['Process crashed','INSTRUMENTATION_FAILED','Fatal signal','AddressSanitizer','runtime error:'])
            else: valid=('OK (1 test)' if args.method else 'OK (5 tests)') in output and 'FAILURES' not in output
            if not valid: raise RuntimeError((logs,output))
            control=run([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig','com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],f'{count}-control')
            if 'OK (1 test)' not in control: raise RuntimeError(control)
    for name,digest in hashes.items(): assert hashlib.sha256((ROOT/name).read_bytes()).hexdigest()==digest
    apks={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [ROOT/'android/app/build/outputs/apk/debug/app-debug.apk',ROOT/'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk']}
    (logs/'receipt.json').write_text(json.dumps(dict(expected_assertion=args.expect_assertion,sources_sha256=hashes,apk_sha256=apks),indent=2)+'\n')
    print('ANDROID PROXYJUMP PASS: '+str(logs))
if __name__=='__main__': main()
