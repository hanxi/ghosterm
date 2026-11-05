#!/usr/bin/env python3
"""Public scoped source lifetime/metadata mutations, with actual C/Swift links.
Normal assertions only, same archive configuration control, restored source SHA.
"""
from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_SOURCE_API_MUTATION'
MODES='invalid feature scope ungranted unsupported parent absolute missing utf8 control mime limit symlink fifo capacity session-pin cancel-before change-ready cancel-sent lost roundtrip large empty literal copy confirmed-stop'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-source-api-mutations-',dir=ROOT/'tmp'));source=work/'source';core=source/'core';build=core/'build';logs=work/'logs';logs.mkdir();commands=[]
    api='core/src/extension/source-api.c';session='core/src/extension/session-api.c';swift='apple/GhostermCore/Sources/GhostermCore/ExtensionSource.swift'
    for name in [api,session,swift]:
        if MARK.encode() in (ROOT/name).read_bytes():print('residual mutation '+name,file=sys.stderr);raise SystemExit(2)
    inputs=[p for folder in ['core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt','core/tests/test_ext_session.c','core/tests/test_ext_rpc.c','core/tests/test_ext_resource.c','core/tests/test_ext_resource_boundary.c','core/tests/test_ext_source.c','core/tests/test_ext_source_boundary.c','core/tests/ext_channel_dial.c','scripts/test-plugin-channel.py','scripts/mutation-check-plugin-source-api.py',swift,'apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift','apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionSourceTests.swift']]
    hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer'}
    def run(label,args,red=False,budget=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-resource-api; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-resource-api',str(ROOT),str(budget),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in out or any(x in out for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer']):raise RuntimeError(label+': expected normal assertion\n'+out)
        elif p.returncode:raise RuntimeError(label+': '+out)
    def rebuild(label):
        binaries=['test_ext_source','test_ext_source_boundary','test_config']
        objects=[('ghosterm_core','src/extension/'+n+'.c') for n in ['source-api','session-api','source']]+[(n,'tests/'+n+'.c') for n in binaries[:2]]
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
        for mode in MODES:target(label+'-'+mode,'test_ext_source',mode)
        for mode in ['close-error','failure-close','destroy-error']:target(label+'-'+mode,'test_ext_source_boundary',mode)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-channel.py','--source-api','--binary',build/'test_ext_source','--source-root',source],budget=120)
    cases=[
      ('requires-resources',api,'if(!s->store)return GT_EXT_SOURCE_UNSUPPORTED;','if(false && !s->store)return GT_EXT_SOURCE_UNSUPPORTED;','test_ext_source','feature'),
      ('source-session-pin',api,'source->session=s;s->sources++;','source->session=s;s->sources+=0;','test_ext_source','session-pin'),
      ('source-capacity',api,'if(s->sources>=GT_EXT_SOURCE_LIMIT)','if(false && s->sources>=GT_EXT_SOURCE_LIMIT)','test_ext_source','capacity'),
      ('source-session-unpin',api,'s->session->sources--;','s->session->sources-=0;','test_ext_source','roundtrip'),
      ('session-close-pinned',session,'return s->sources?GT_EXT_DRAINING:GT_EXT_RELEASED;','return s->sources?GT_EXT_RELEASED:GT_EXT_RELEASED;','test_ext_source','session-pin'),
      ('session-destroy-pinned',session,' || (*owned)->sources',' || false','test_ext_source','session-pin'),
      ('constructor-failure-clear',api,'if(out)*out=NULL;','if(out)*out=*out;','test_ext_source','invalid'),
      ('info-failure-clear',api,'if(out)memset(out,0,sizeof(*out));','(void)out;','test_ext_source','invalid'),
      ('hash-copy',api,'memcpy(out->descriptor.sha256,i.descriptor.sha256,65);','memset(out->descriptor.sha256,0,65);','test_ext_source','roundtrip'),
      ('read-count',api,'out->read_bytes=i.read_bytes;','out->read_bytes=0;','test_ext_source','roundtrip'),
      ('accepted-count',api,'out->accepted=i.accepted;','out->accepted=0;','test_ext_source','roundtrip'),
      ('has-transfer',api,'out->has_transfer=i.transfer_handle!=0;','out->has_transfer=false;','test_ext_source','roundtrip'),
      ('confirmation-fact',api,'out->confirmed=i.transfer.confirmed;','out->confirmed=true;','test_ext_source','roundtrip'),
      ('unknown-phase',api,'case GT_BRIDGE_SOURCE_UNCERTAIN:return GT_EXT_SOURCE_UNCERTAIN;','case GT_BRIDGE_SOURCE_UNCERTAIN:return GT_EXT_SOURCE_STOPPED;','test_ext_source','lost'),
      ('unknown-error',api,'case GT_BRIDGE_SOURCE_UNKNOWN:return GT_EXT_SOURCE_UNKNOWN;','case GT_BRIDGE_SOURCE_UNKNOWN:return GT_EXT_SOURCE_CANCELLED;','test_ext_source','lost'),
      ('confirmed-state',api,'case GT_BRIDGE_TRANSFER_SUCCEEDED:return GT_EXT_TRANSFER_SUCCEEDED;','case GT_BRIDGE_TRANSFER_SUCCEEDED:return GT_EXT_TRANSFER_FAILED;','test_ext_source','roundtrip'),
      ('cleanup-io-visible',api,'if(status==GT_EXT_SOURCE_IO)s->cleanup_error=status;','if(status==GT_EXT_SOURCE_IO)s->cleanup_error=GT_EXT_SOURCE_OK;','test_ext_source_boundary','close-error'),
      ('primary-error-preserved',api,'out->error=ext_source_status(i.error);','out->error=s->cleanup_error;','test_ext_source_boundary','failure-close'),
      ('destroy-error-retained',api,'if(status!=GT_EXT_SOURCE_OK){if(status==GT_EXT_SOURCE_IO)s->cleanup_error=status;return status;}','if(false && status!=GT_EXT_SOURCE_OK){if(status==GT_EXT_SOURCE_IO)s->cleanup_error=status;return status;}','test_ext_source_boundary','destroy-error'),
      ('wait-not-accepted',api,'case GT_BRIDGE_SOURCE_WAIT:return GT_EXT_SOURCE_WAIT;','case GT_BRIDGE_SOURCE_WAIT:return GT_EXT_SOURCE_OK;','test_ext_source','roundtrip'),
      ('cancel-effect',api,'if(s)gt__bridge_source_cancel(s->inner);','if(s)(void)s->inner;','test_ext_source','cancel-before'),
      ('step-advances',api,'return s?ext_source_phase(gt__bridge_source_step(s->inner)):GT_EXT_SOURCE_FAILED;','return s && s->inner?GT_EXT_SOURCE_HASHING:GT_EXT_SOURCE_FAILED;','test_ext_source','roundtrip'),
      ('denied-not-success',api,'case GT_BRIDGE_SOURCE_DENIED:return GT_EXT_SOURCE_DENIED;','case GT_BRIDGE_SOURCE_DENIED:return GT_EXT_SOURCE_OK;','test_ext_source','scope'),
      ('close-io-not-success',api,'case GT_BRIDGE_SOURCE_IO:return GT_EXT_SOURCE_IO;','case GT_BRIDGE_SOURCE_IO:return GT_EXT_SOURCE_OK;','test_ext_source_boundary','close-error'),
    ]
    swift_cases=[
      ('swift-source-hashed',swift,'hashed: v.hashed, readBytes: v.read_bytes','hashed: 0, readBytes: v.read_bytes','testFileReceiptAndBorrowPinSurviveParentClose'),
      ('swift-source-error',swift,'error: Status(v.error), cleanupError:','error: .cancelled, cleanupError:','testDroppedSessionKeepsSourceSafeUntilRelease'),
      ('swift-source-phase',swift,'Info(phase: Phase(rawValue: v.phase.rawValue) ?? .failed','Info(phase: .stopped','testDroppedSessionKeepsSourceSafeUntilRelease'),
      ('swift-parent-reaper',swift,'let q = owner; q.async { ExtensionSession.retryPendingClose(on: q); ExtensionResourceStore.retryPendingClose(on: q) }','let q = owner; q.async { }','testDroppedSessionKeepsSourceSafeUntilRelease'),
      ('swift-live-source-reaper','apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift','ExtensionFileSource.reapDroppedSources(on: owner); return phase','return phase','testDroppedSourceIsReapedWhileSessionKeepsRunning'),
    ]
    try:
        for label,name,old,_,_,_ in cases:
            if old not in (ROOT/name).read_text():raise RuntimeError('missing anchor '+label)
        shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build','build-*'));(source/'tmp').mkdir()
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
            args=['python3',ROOT/'scripts/test-plugin-channel.py','--source-api','--binary',build/'test_ext_source','--source-root',source,'--case','allow2','--apple']
            if method:args += ['--apple-filter','ExtensionSourceTests.'+method]
            if red:args += ['--expect-platform-assertion','apple']
            run(label,args,budget=240)
        swift_run('swift-baseline')
        for label,name,old,new,method in swift_cases:
            path=source/name;original=path.read_bytes();assert old in original.decode();path.write_text(original.decode().replace(old,new)+'\n// '+MARK+'\n')
            try:swift_run(label,method,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            swift_run(label+'-restored',method);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': normal XCTest assertion red / config green / restored / SHA256',flush=True)
        swift_run('swift-final')
        for name,h in hashes.items():assert sha(ROOT/name)==h,name
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,core_groups=[x[0] for x in cases],swift_groups=[x[0] for x in swift_cases],inputs_sha256=hashes,commands=commands,c_sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        shutil.rmtree(source);print('PUBLIC SOURCE MUTATIONS PASS; logs: '+str(logs),flush=True)
    except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
