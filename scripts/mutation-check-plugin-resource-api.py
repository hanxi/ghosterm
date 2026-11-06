#!/usr/bin/env python3
"""Public resource ownership/copy/receipt mutations, with actual C/Swift links.
Normal assertions only, same archive configuration control, restored source SHA.
"""
from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_RESOURCE_API_MUTATION'
MODES='invalid constructor features store-pending denied unsupported scope roundtrip reverse large empty unknown-size copy retain after-close quota backpressure queued-cancel lost confirmed-stop digest length'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-resource-api-mutations-',dir=ROOT/'tmp'));source=work/'source';core=source/'core';build=core/'build';logs=work/'logs';logs.mkdir();commands=[]
    api='core/src/extension/resource-api.c';session='core/src/extension/session-api.c';swift='apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift'
    for name in [api,session,swift]:
        if MARK.encode() in (ROOT/name).read_bytes():print('residual mutation '+name,file=sys.stderr);raise SystemExit(2)
    inputs=[p for folder in ['core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt','core/tests/test_ext_session.c','core/tests/test_ext_rpc.c','core/tests/test_ext_resource.c','core/tests/test_ext_resource_boundary.c','core/tests/ext_channel_dial.c','scripts/test-plugin-channel.py','scripts/mutation-check-plugin-resource-api.py',swift,'apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionResourceTests.swift']]
    hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer'}
    def run(label,args,red=False,budget=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-resource-api; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-resource-api',str(ROOT),str(budget),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in out or any(x in out for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer']):raise RuntimeError(label+': expected normal assertion\n'+out)
        elif p.returncode:raise RuntimeError(label+': '+out)
    def rebuild(label):
        binaries=['test_ext_resource','test_ext_resource_boundary','test_config']
        objects=[('ghosterm_core','src/extension/'+n+'.c') for n in ['resource-api','session-api','resource']]+[(n,'tests/'+n+'.c') for n in binaries[:2]]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target',*binaries,'-j','2'])
        out=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in out:raise RuntimeError('missing actual compilation '+label+' '+name)
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+n for n in binaries]]:
            if marker not in out:raise RuntimeError('missing actual link '+label+' '+marker)
    def target(label,binary,mode,red=False):run(label,[build/binary,mode,work],red=red,budget=45)
    def suite(label):
        for mode in MODES:target(label+'-'+mode,'test_ext_resource',mode)
        for mode in ['retired-close','directory-close','metadata-boundary']:target(label+'-'+mode,'test_ext_resource_boundary',mode)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-channel.py','--resources','--binary',build/'test_ext_resource','--source-root',source],budget=120)
    cases=[
      ('resource-id-nul',api,'!memchr(d->resource_id,0,sizeof(d->resource_id)) ||','false ||','test_ext_resource_boundary','metadata-boundary'),
      ('digest-nul',api,'!memchr(d->sha256,0,sizeof(d->sha256)) ||','false ||','test_ext_resource_boundary','metadata-boundary'),
      ('mime-nul',api,'!memchr(d->mime,0,sizeof(d->mime)) ||','false ||','test_ext_resource_boundary','metadata-boundary'),
      ('name-nul',api,'!memchr(d->display_name,0,sizeof(d->display_name))','false','test_ext_resource_boundary','metadata-boundary'),
      ('store-session-pin',session,'if(store)store->sessions++;','if(store)store->sessions+=0;','test_ext_resource','store-pending'),
      ('store-session-release',session,'if(s->store)s->store->sessions--;','if(s->store)s->store->sessions-=0;','test_ext_resource','constructor'),
      ('store-scope-budget',session,'scope>store->total_bytes ||','false ||','test_ext_resource','constructor'),
      ('explicit-features',session,'(config->features!=GT_EXT_FEATURE_RESOURCES && config->features!=(GT_EXT_FEATURE_RESOURCES|GT_EXT_FEATURE_REQUESTS))','(false && config->features!=GT_EXT_FEATURE_RESOURCES)','test_ext_resource','features'),
      ('usage-counts',api,'out->reserved_bytes=u.reserved_bytes;','out->reserved_bytes=0;','test_ext_resource','roundtrip'),
      ('deny-not-success',api,'case GT_BRIDGE_RESOURCE_DENIED:return GT_EXT_RESOURCE_DENIED;','case GT_BRIDGE_RESOURCE_DENIED:return GT_EXT_RESOURCE_OK;','test_ext_resource','denied'),
      ('handle-failure-clear',api,'if(handle)*handle=0;','if(handle)*handle=*handle;','test_ext_resource','scope'),
      ('take-failure-clear',api,'if(out)memset(out,0,sizeof(*out));if(!out)return GT_EXT_RESOURCE_INVALID;','if(!out)return GT_EXT_RESOURCE_INVALID;','test_ext_resource','scope'),
      ('metadata-copy',api,'memcpy(d.sha256,p->sha256,65);','memset(d.sha256,0,65);','test_ext_resource','roundtrip'),
      ('snapshot-token',api,'memcpy(out->token,p.ref.token,33);','memset(out->token,0,33);','test_ext_resource','roundtrip'),
      ('snapshot-owner',api,'out->owner=p.owner;','out->owner=p.owner+1;','test_ext_resource','roundtrip'),
      ('snapshot-descriptor',api,'out->descriptor=resource_metadata(&p.descriptor);','memset(&out->descriptor,0,sizeof(out->descriptor));','test_ext_resource','roundtrip'),
      ('read-offset',api,'&r,offset,p,cap,read_bytes)','&r,offset+1,p,cap,read_bytes)','test_ext_resource','roundtrip'),
      ('retain-pin',api,'return resource_status(gt__bridge_resource_retain(s->inner,ref->owner,&l,&r));','return s && l.epoch && r.handle?GT_EXT_RESOURCE_OK:GT_EXT_RESOURCE_INVALID;','test_ext_resource','retain'),
      ('release-ref-clear',api,'gt__secure_erase(ref,sizeof(*ref));','(void)ref;','test_ext_resource','roundtrip'),
      ('retired-scope-collect',api,'gt__bridge_resource_collect_retired(s->inner);','(void)s->inner;','test_ext_resource_boundary','retired-close'),
      ('destroy-consumed-pointer',api,'gt__plugin_memory_free(NULL,s,sizeof(*s));*owned=NULL;','gt__plugin_memory_free(NULL,s,sizeof(*s));','test_ext_resource_boundary','directory-close'),
      ('finish-source-eof',api,'&l,handle,complete));','&l,handle,(complete || true)));','test_ext_resource','roundtrip'),
      ('accepted-bytes',api,'return resource_status(gt__bridge_session_write_resource(s->inner,&l,handle,p,n,accepted));','gt_ext_resource_status_t result=resource_status(gt__bridge_session_write_resource(s->inner,&l,handle,p,n,accepted));*accepted=0;return result;','test_ext_resource','roundtrip'),
      ('confirmation-fact',api,'out->confirmed=r.confirmed;','out->confirmed=true;','test_ext_resource','queued-cancel'),
      ('unknown-not-cancelled',api,'case GT_BRIDGE_TRANSFER_UNKNOWN:out->state=GT_EXT_TRANSFER_UNKNOWN;','case GT_BRIDGE_TRANSFER_UNKNOWN:out->state=GT_EXT_TRANSFER_CANCELLED;','test_ext_resource','lost'),
      ('confirmed-success-state',api,'case GT_BRIDGE_TRANSFER_SUCCEEDED:out->state=GT_EXT_TRANSFER_SUCCEEDED;','case GT_BRIDGE_TRANSFER_SUCCEEDED:out->state=GT_EXT_TRANSFER_FAILED;','test_ext_resource','roundtrip'),
    ]
    swift_cases=[
      ('swift-resource-confirmation','confirmed: v.confirmed, cancelRequested: v.cancel_requested','confirmed: true, cancelRequested: v.cancel_requested','testQueuedCancellationAndLostAreDifferent'),
      ('swift-resource-unknown','state: TransferState(rawValue: v.state.rawValue) ?? .unknown','state: .cancelled','testQueuedCancellationAndLostAreDifferent'),
      ('swift-resource-read','data.count = n; return (status, data)','data = Data(count: n); return (status, data)','testRetainedSnapshotOutlivesSessionAndRemainsCharged'),
      ('swift-store-session-charge','sessions: u.sessions)','sessions: 0)','testRetainedSnapshotOutlivesSessionAndRemainsCharged'),
    ]
    try:
        for label,name,old,_,_,_ in cases:
            if old not in (ROOT/name).read_text():raise RuntimeError('missing anchor '+label)
        shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build','build-*'))
        shutil.copytree(ROOT/'apple/GhostermCore',source/'apple/GhostermCore',symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
        run('configure',['cmake','-S',core,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-config',[build/'test_config',core/'tests/fixtures'])
        for label,name,old,new,binary,mode in cases:
            path=source/name;original=path.read_bytes();path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n')
            try:rebuild(label);target(label+'-target',binary,mode,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            rebuild(label+'-restored');target(label+'-restored-target',binary,mode);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': real compile/link assertion red / same archive config green / rebuilt restored / SHA256',flush=True)
        suite('final');run('swift-configure',['cmake','-S',core,'-B',build,'-DCMAKE_C_FLAGS=']);rebuild('swift-core')
        def swift_run(label,method=None,red=False):
            args=['python3',ROOT/'scripts/test-plugin-channel.py','--resources','--binary',build/'test_ext_resource','--source-root',source,'--case','allow2','--apple']
            if method:args += ['--apple-filter','ExtensionResourceTests.'+method]
            if red:args += ['--expect-platform-assertion','apple']
            run(label,args,budget=240)
        swift_run('swift-baseline')
        for label,old,new,method in swift_cases:
            path=source/swift;original=path.read_bytes();assert old in original.decode();path.write_text(original.decode().replace(old,new)+'\n// '+MARK+'\n')
            try:swift_run(label,method,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            swift_run(label+'-restored',method);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': normal XCTest assertion red / config green / restored / SHA256',flush=True)
        swift_run('swift-final')
        for name,h in hashes.items():assert sha(ROOT/name)==h,name
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,core_groups=[x[0] for x in cases],swift_groups=[x[0] for x in swift_cases],inputs_sha256=hashes,commands=commands,c_sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        shutil.rmtree(source);print('PUBLIC RESOURCE MUTATIONS PASS; logs: '+str(logs),flush=True)
    except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
