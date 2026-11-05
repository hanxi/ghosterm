#!/usr/bin/env python3
"""Public native channel ABI and Apple/JNI workers over managed SSH routes.
Only disposable in-memory keys; no external gateway or automatic tool install.
"""
from pathlib import Path
from contextlib import ExitStack
import argparse,hashlib,importlib.util,json,os,pwd,re,secrets,shlex,signal,socketserver,struct,subprocess,sys,tempfile,threading
ROOT=Path(__file__).resolve().parents[1];sys.dont_write_bytecode=True
def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--session',action='store_true');parser.add_argument('--rpc',action='store_true');parser.add_argument('--binary',type=Path);parser.add_argument('--apple',action='store_true');parser.add_argument('--android');parser.add_argument('--case',action='append');parser.add_argument('--expect-assertion',action='store_true');parser.add_argument('--apple-filter');parser.add_argument('--android-test');parser.add_argument('--expect-platform-assertion',choices=['apple','android']);parser.add_argument('--source-root',type=Path,default=ROOT);args=parser.parse_args();args.session=args.session or args.rpc;binary=(args.binary or ROOT/('core/build/test_ext_rpc' if args.rpc else 'core/build/test_ext_session' if args.session else 'core/build/test_ext_channel')).resolve();assert binary.is_file();args.apple_filter=args.apple_filter or ('ExtensionRPCTests' if args.rpc else 'ExtensionSessionTests' if args.session else 'ExtensionChannelTests');args.android_test=args.android_test or ('com.hanxi.ghosterm.ExtensionRPCTest' if args.rpc else 'com.hanxi.ghosterm.ExtensionSessionTest' if args.session else 'com.hanxi.ghosterm.ExtensionChannelTest')
    if args.expect_assertion:assert args.case and len(args.case)==1 and not args.apple and not args.android
    if args.expect_platform_assertion:assert (args.apple and not args.android) if args.expect_platform_assertion=='apple' else (args.android and not args.apple)
    source_root=args.source_root.resolve();inputs=[p for folder in ['core/src','core/include'] for p in (source_root/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [source_root/p for p in ['core/CMakeLists.txt','core/tests/test_ext_channel.c','core/tests/test_ext_channel_boundary.c','core/tests/ext_channel_dial.c']]
    if args.apple:inputs += [source_root/p for p in ['apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionChannelTests.swift']]
    if args.android:inputs += [source_root/p for p in ['android/bridge/src/main/cpp/extension_jni.c','android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ExtensionChannel.kt','android/app/src/androidTest/java/com/hanxi/ghosterm/ExtensionChannelTest.kt']]
    if args.session:
        inputs += [source_root/'core/tests/test_ext_session.c']
        if args.apple:inputs += [source_root/p for p in ['apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionSessionTests.swift']]
        if args.android:inputs += [source_root/p for p in ['android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ExtensionSession.kt','android/app/src/androidTest/java/com/hanxi/ghosterm/ExtensionSessionTest.kt']]
    if args.rpc:
        inputs += [source_root/'core/tests/test_ext_rpc.c']
        if args.apple:inputs += [source_root/'apple/GhostermCore/Tests/GhostermCoreTests/ExtensionRPCTests.swift']
        if args.android:inputs += [source_root/'android/app/src/androidTest/java/com/hanxi/ghosterm/ExtensionRPCTest.kt']
    hashes={str(p.relative_to(source_root)):hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}
    work=Path(tempfile.mkdtemp(prefix='plugin-public-channel-',dir=ROOT/'tmp'));results=[];env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    proofs=work/'proofs';proofs.mkdir()
    helpers=load('channel_keys',ROOT/'scripts/test-plugin-ssh.py');streams=load('channel_memory',ROOT/'scripts/test-plugin-ssh-stream.py');route=load('channel_routes',ROOT/'scripts/test-plugin-route.py');private,public=streams.memory_identity();wrong_private,_=streams.memory_identity()
    def run(label,cmd,*,input=None,budget=120,environment=env,red=None):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-public-channel; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-public-channel',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=environment,input=input,capture_output=True,timeout=budget+10);output=(p.stdout+p.stderr).decode(errors='replace');(work/(label+'.log')).write_text(output)
        if red=='c':
            assert p.returncode==1 and 'FAIL ' in output and not any(x in output for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer']),output
        elif red=='apple':
            assert p.returncode==1 and re.search(r'Executed 1 test, with [1-9][0-9]* failures? \(0 unexpected\)',output) and not any(x in output for x in ['AddressSanitizer','runtime error:','crashed']),output
        elif red=='android':
            assert p.returncode==0 and re.search(r'Tests run:\s*1,\s*Failures:\s*1',output) and 'java.lang.AssertionError' in output and 'INSTRUMENTATION_FAILED' not in output,output
        elif p.returncode:raise RuntimeError(label+': '+output)
        results.append(label);return output
    with helpers.owned_agent(work,'host-agent') as agent:
        keys=[helpers.add_memory_key(agent,'public-channel-hop-'+str(i)) for i in range(3)];assert len({k.split()[1] for k in keys})==3
        cases=args.case or (['allow2','allow3','deny2','deny3'] if args.session else ['echo2','echo3','credentials2','credentials3','unknown-accept','unknown-reject','wrong-pin','forward-deny','credentials-retry','expired-before-supply'])
        for case in cases:
            assert case in (['allow2','allow3','deny2','deny3'] if args.session else ['echo2','echo3','credentials2','credentials3','unknown-accept','unknown-reject','wrong-pin','forward-deny','credentials-retry','expired-before-supply']);count=3 if case.endswith('3') else 2;mode=('ssh-deny' if case.startswith('deny') else 'ssh-allow') if args.session else case[:-1] if case[-1:] in ('2','3') else case;folder=work/case;folder.mkdir()
            with route.chain(folder,count,public,keys,agent,'echo' if args.session else mode) as (ports,known,_):
                command=shlex.join([str(binary),'--executor',*([str(proofs)] if args.rpc else [])]);output=run(case,[binary,mode,str(count),pwd.getpwuid(os.getuid()).pw_name,known,command,*ports],input=struct.pack('>I',len(private))+private+(struct.pack('>I',len(wrong_private))+wrong_private if case=='credentials-retry' else b''),budget=30,red='c' if args.expect_assertion else None)
                if not args.expect_assertion:assert f"PASS {'ext_rpc' if args.rpc else 'ext_session' if args.session else 'ext_channel'}_{mode}_{count}" in output,output
            print(('ASSERTION RED public channel ' if args.expect_assertion else 'PASS public channel ')+case,flush=True)
        if args.apple or args.android:
            with ExitStack() as stack:
                fixture={};first_ports=[]
                for label,count,mode in [('two',2,'echo'),('three',3,'echo'),('denied',2,'forward-deny')]:
                    folder=work/('native-'+label);folder.mkdir();ports,known,_=stack.enter_context(route.chain(folder,count,public,keys,agent,'echo' if args.session else mode));first_ports.append(ports[0]);fixture[label]=dict(ports=ports,known=known.read_text())
                fixture.update(user=pwd.getpwuid(os.getuid()).pw_name,pem=private.decode(),wrongPem=wrong_private.decode(),bootstrap=shlex.join([str(binary),'--executor',*([str(proofs)] if args.rpc else [])]))
                payload=json.dumps(fixture).encode();token=secrets.token_hex(24)
                class Handler(socketserver.StreamRequestHandler):
                    def handle(self):
                        self.request.settimeout(5)
                        if self.rfile.readline(128).rstrip(b'\n')!=token.encode():return
                        self.wfile.write(struct.pack('>I',len(payload))+payload)
                server=socketserver.ThreadingTCPServer(('127.0.0.1',0),Handler);server.daemon_threads=True;thread=threading.Thread(target=server.serve_forever);thread.start()
                def close_server():server.shutdown();server.server_close();thread.join(5);assert not thread.is_alive()
                stack.callback(close_server);port=server.server_address[1]
                if args.apple:
                    apple={**env,'DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer','GHOSTERM_EXT_FIXTURE_PORT':str(port),'GHOSTERM_EXT_FIXTURE_TOKEN':token}
                    output=run('apple',['swift','test','--package-path',source_root/'apple/GhostermCore','--filter',args.apple_filter],budget=180,environment=apple,red='apple' if args.expect_platform_assertion=='apple' else None)
                    if not args.expect_platform_assertion:assert 'skipped' not in output.lower() and '0 failures' in output,output
                    print('PASS public channel Swift native + two/three SSH',flush=True)
                if args.android:
                    adb=ROOT/'build/android-toolchain/sdk/platform-tools/adb';device=[str(adb),'-s',args.android]
                    for p in [port,*first_ports]:
                        run('reverse-'+str(p),[*device,'reverse','tcp:'+str(p),'tcp:'+str(p)],budget=15)
                        stack.callback(lambda p=p:subprocess.run([*device,'reverse','--remove','tcp:'+str(p)],capture_output=True,timeout=10))
                    output=run('android',[*device,'shell','am','instrument','-w','-r','-e','class',args.android_test,'-e','fixturePort',str(port),'-e','fixtureToken',token,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],budget=240,red='android' if args.expect_platform_assertion=='android' else None)
                    if not args.expect_platform_assertion:assert 'OK (' in output and 'FAILURES' not in output and 'INSTRUMENTATION_FAILED' not in output,output
                    print('PASS public channel JNI native + two/three SSH (only first ports reversed)',flush=True)
    if args.rpc and not args.expect_assertion and not args.expect_platform_assertion:
        reports=list(proofs.glob('rpc-proof-*'));assert len(reports)>=sum(case.startswith('allow') for case in cases),'missing actual executor completion proof'
        for proof in reports:assert json.loads(proof.read_text())==dict(completed=80,handled=80,done=True),proof
    for p in work.rglob('*'):
        if p.is_file() and (b'PRIVATE KEY-----' in p.read_bytes() or b'openssh-key-v1\0' in p.read_bytes()):raise RuntimeError('private fixture serialized')
    for name,value in hashes.items():assert hashlib.sha256((source_root/name).read_bytes()).hexdigest()==value,name
    scope='public bidirectional JSON RPC; actual remote completion proofs; no method JSON Schema/runtime/UI/resources/WAN/gateway' if args.rpc else 'public native Session handshake/consent/lease/revoke; no request/resource facade, App consent UI/runtime/WAN/gateway' if args.session else 'public channel providers only; managed loopback routes and native worker bindings, no plugin consent/runtime/UI/WAN/gateway'
    (work/'result.json').write_text(json.dumps(dict(accepted=True,cases=results,session_lifecycle=args.session,expected_assertion=args.expect_assertion,expected_platform_assertion=args.expect_platform_assertion,inputs_sha256=hashes,source_root=str(source_root),binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),scope=scope),indent=2)+'\n');print('PUBLIC CHANNEL PASS; logs: '+str(work),flush=True)
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
