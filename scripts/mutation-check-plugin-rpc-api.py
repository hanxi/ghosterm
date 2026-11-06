#!/usr/bin/env python3
"""Public JSON RPC facade and native snapshot mutations, isolated actual C and Swift.
Normal assertions only; same archive config green; force target recompilation,
restore bytes/SHA, and run full native/SSH suites before and after all variants.
"""
from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_RPC_API_MUTATION'
NATIVE='registry invalid feature scope policy validation shapes full queued-cancel queued-timeout remote-invalid remote-unknown roundtrip reverse copy lost sent-timeout sent-cancel confirmed-stop'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-rpc-api-mutations-',dir=ROOT/'tmp'));source=work/'source';core=source/'core';build=core/'build';logs=work/'logs';logs.mkdir();commands=[]
    inputs=[p for folder in ['core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt','core/tests/test_ext_session.c','core/tests/test_ext_rpc.c','core/tests/ext_channel_dial.c','scripts/test-plugin-channel.py','scripts/mutation-check-plugin-rpc-api.py','apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift','apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionRPCTests.swift']]
    hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer'}
    def run(label,args,red=False,budget=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-session-api; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-session-api',str(ROOT),str(budget),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in out or any(x in out for x in ['AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:']):raise RuntimeError(label+': expected normal C assertion\n'+out)
        elif p.returncode:raise RuntimeError(label+': '+out)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_ext_rpc', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/session-api.c'),('test_ext_rpc','tests/test_ext_rpc.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_ext_rpc','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in output,label+' failed to rebuild '+name
    def target(label,kind,mode,red=False):run(label,[build/'test_ext_rpc',mode],red=red,budget=30)
    def suite(label):
        for mode in NATIVE:target(label+'-'+mode,'native',mode)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-channel.py','--rpc','--binary',build/'test_ext_rpc','--source-root',source],budget=120)
    api='core/src/extension/session-api.c';channel='core/src/extension/channel.c'
    cases=[
      ('requests-enabled',api,'    c.rpc_enabled = requests;\n','    c.rpc_enabled = false;\n','native','registry'),
      ('feature-explicit',api,'    if (config->features != GT_EXT_FEATURE_REQUESTS ||\n','    if ((false && config->features != GT_EXT_FEATURE_REQUESTS) ||\n','native','registry'),
      ('receiver-no-truncation',api,'        if (methods[i].receiver != GT_EXT_HOST && methods[i].receiver != GT_EXT_EXECUTOR) {\n','        if (false && methods[i].receiver != GT_EXT_HOST && methods[i].receiver != GT_EXT_EXECUTOR) {\n','native','registry'),
      ('method-copy',api,'        s->methods[i] = methods[i];\n','        memset(&s->methods[i], 0, sizeof(s->methods[i]));\n','native','validation'),
      ('capability-copy',api,'        memcpy(m->capability, methods[i].capability, sizeof(m->capability));\n','        strcpy(m->capability, "ui.form");\n','native','policy'),
      ('input-shape',api,"            return p[i] == '{';\n",'            return true;\n','native','validation'),
      ('output-shape',api,'    return ext_shape(((gt_ext_rpc_method_t *)ctx)->output_shape, p, n);\n','    return ctx && p && n;\n','native','validation'),
      ('handle-failure-clear',api,'    if (handle) {\n','    if (false && handle) {\n','native','invalid'),
      ('snapshot-failure-clear',api,'    if (out) {\n        memset(out, 0, sizeof(*out));\n    }\n    if (!out) {\n','    if (!out) {\n','native','invalid'),
      ('scope-before-empty-take',api,'    return gt__bridge_session_scope(s->inner, &l) ? GT_EXT_RPC_OK : GT_EXT_RPC_DENIED;\n','    return (gt__bridge_session_scope(s->inner, &l) || true) ? GT_EXT_RPC_OK : GT_EXT_RPC_DENIED;\n','native','scope'),
      ('wait-not-accepted',api,'            return GT_EXT_RPC_WAIT;\n','            return GT_EXT_RPC_OK;\n','native','full'),
      ('unknown-not-cancelled',api,'            out->state = GT_EXT_RPC_UNKNOWN;\n','            out->state = GT_EXT_RPC_CANCELLED;\n','native','lost'),
      ('confirmation-observed',api,'    out->confirmed = r.confirmed;\n','    out->confirmed = true;\n','native','queued-cancel'),
      ('input-snapshot',api,'    memcpy(out->input, r.input, r.input_size);\n','    memset(out->input, 0, r.input_size);\n','native','roundtrip'),
      ('outcome-snapshot',api,'        memcpy(out->outcome, r.outcome, r.outcome_size);\n','        memset(out->outcome, 0, r.outcome_size);\n','native','roundtrip'),
      ('incoming-ticket',api,'    out->ticket = r.ticket;\n','    out->ticket = r.ticket + 1;\n','native','roundtrip'),
      ('check-before-effect',api,'    return ext_rpc_status(gt__bridge_session_check_request(s->inner, &l, ticket));\n','    return s && l.epoch && ticket ? GT_EXT_RPC_OK : GT_EXT_RPC_INVALID;\n','native','roundtrip'),
      ('queued-cancel-effect-none',api,'    return ext_rpc_status(gt__bridge_session_cancel_request(s->inner, &l, handle));\n','    return s && l.epoch && handle ? GT_EXT_RPC_OK : GT_EXT_RPC_INVALID;\n','native','queued-cancel'),
    ]
    swift_cases=[
      ('swift-result-state','state: RPCState(rawValue: value.state.rawValue) ?? .unknown','state: .cancelled','testLostPendingRequestRemainsUnknown'),
      ('swift-confirmation','confirmed: value.confirmed, outcome: data','confirmed: true, outcome: data','testQueuedCancellationAndSlotBackpressure'),
      ('swift-input-snapshot','let data = withUnsafeBytes(of: value.input) { Data($0.prefix(Int(value.input_size))) }','let data = Data(count: Int(value.input_size))','testBidirectionalSnapshotsSurviveForgetAndClose'),
    ]
    try:
        for name in [api,channel,'apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift']:
            if MARK.encode() in (ROOT/name).read_bytes():
                print('residual mutation '+name,file=sys.stderr);raise SystemExit(2)
        for label,name,old,_,_,_ in cases:
            if old not in (ROOT/name).read_text():raise RuntimeError('missing anchor '+label)
        shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build','build-*'))
        shutil.copytree(ROOT/'apple/GhostermCore',source/'apple/GhostermCore',symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
        run('configure',['cmake','-S',core,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-config',[build/'test_config',core/'tests/fixtures'])
        for label,name,old,new,kind,mode in cases:
            path=source/name;original=path.read_bytes();path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n')
            try:rebuild(label);target(label+'-target',kind,mode,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            rebuild(label+'-restored');target(label+'-restored-target',kind,mode);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': compiled normal assertion red / same archive config green / rebuilt restored / SHA256',flush=True)
        suite('final');run('swift-core-configure',['cmake','-S',core,'-B',build,'-DCMAKE_C_FLAGS=']);rebuild('swift-core')
        def swift(label,method=None,red=False):
            args=['python3',ROOT/'scripts/test-plugin-channel.py','--rpc','--binary',build/'test_ext_rpc','--source-root',source,'--case','allow2','--apple']
            if method:args += ['--apple-filter','ExtensionRPCTests.'+method]
            if red:args += ['--expect-platform-assertion','apple']
            run(label,args,budget=240)
        swift('swift-baseline')
        for label,old,new,method in swift_cases:
            path=source/'apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift';original=path.read_bytes();assert old in original.decode();path.write_text(original.decode().replace(old,new)+'\n// '+MARK+'\n')
            try:swift(label,method,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            swift(label+'-restored',method);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': XCTest assertion red / config green / rebuilt restored / SHA256',flush=True)
        swift('swift-final')
        for name,h in hashes.items():assert sha(ROOT/name)==h,name
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,core_groups=[c[0] for c in cases],swift_groups=[c[0] for c in swift_cases],inputs_sha256=hashes,commands=commands,c_sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        shutil.rmtree(source);print('PUBLIC RPC MUTATIONS PASS: '+str(len(cases))+' C + '+str(len(swift_cases))+' Swift; logs: '+str(logs),flush=True)
    except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
