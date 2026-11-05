#!/usr/bin/env python3
"""Public Session ownership/consent mutations, isolated actual C and Swift.
Normal assertions only; same archive config green; force target recompilation,
restore bytes/SHA, and run full native/SSH suites before and after all variants.
"""
from pathlib import Path
import hashlib,json,os,shutil,signal,subprocess,sys,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_SESSION_API_MUTATION'
NATIVE='invalid config dirty-write dirty-read stopped closed capacity allow deny expired ticket stop lost revoke'.split()
BOUNDARY='allocation inner-open close-error draining destroy-error'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-session-api-mutations-',dir=ROOT/'tmp'));source=work/'source';core=source/'core';build=core/'build';logs=work/'logs';logs.mkdir();commands=[]
    inputs=[p for folder in ['core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt','core/tests/test_ext_session.c','core/tests/test_ext_session_boundary.c','core/tests/ext_channel_dial.c','scripts/test-plugin-channel.py','scripts/mutation-check-plugin-session-api.py','apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift','apple/GhostermCore/Sources/GhostermCore/ExtensionSession.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionSessionTests.swift']]
    hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer'}
    def run(label,args,red=False,budget=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-session-api; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-session-api',str(ROOT),str(budget),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in out or any(x in out for x in ['AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:']):raise RuntimeError(label+': expected normal C assertion\n'+out)
        elif p.returncode:raise RuntimeError(label+': '+out)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_ext_session', 'test_ext_session_boundary', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/channel.c'),('ghosterm_core','src/extension/session-api.c'),('test_ext_session_boundary','tests/test_ext_session_boundary.c'),('test_ext_session','tests/test_ext_session.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_ext_session','test_ext_session_boundary','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in output,label+' failed to rebuild '+name
    def target(label,kind,mode,red=False):run(label,[build/('test_ext_session_boundary' if kind=='boundary' else 'test_ext_session'),mode],red=red,budget=30)
    def suite(label):
        for mode in NATIVE:target(label+'-'+mode,'native',mode)
        for mode in BOUNDARY:target(label+'-'+mode,'boundary',mode)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-channel.py','--session','--binary',build/'test_ext_session','--source-root',source],budget=120)
    api='core/src/extension/session-api.c';channel='core/src/extension/channel.c'
    cases=[
      ('ownership-cleared',api,'*owned=NULL;*out=s;','*out=s;','native','invalid'),
      ('capacity-release',api,'gt__ext_channel_retire(s->channel);','(void)s->channel;','native','capacity'),
      ('pristine-write',channel,' || c->info.accepted ||',' || false ||','native','dirty-write'),
      ('pristine-read',channel,' || c->info.received ||',' || false ||','native','dirty-read'),
      ('provider-kind',channel,'c->ssh!=ssh ||','(false && c->ssh!=ssh) ||','native','config'),
      ('features-explicit',api,'if(config->features ||','if((false && config->features) ||','native','config'),
      ('peer-evidence',api,'memcpy(out->peer.identity,i.peer.identity,sizeof(out->peer.identity));','memset(out->peer.identity,0,sizeof(out->peer.identity));','native','allow'),
      ('unknown-keeps-waiting',api,'decision==GT_EXT_DECISION_ALLOW?GT_BRIDGE_DECISION_ALLOW:','decision==GT_EXT_DECISION_UNKNOWN?GT_BRIDGE_DECISION_ALLOW:decision==GT_EXT_DECISION_ALLOW?GT_BRIDGE_DECISION_ALLOW:','native','allow'),
      ('deny-preserved',api,'decision==GT_EXT_DECISION_DENY?GT_BRIDGE_DECISION_DENY:','decision==GT_EXT_DECISION_DENY?GT_BRIDGE_DECISION_ALLOW:','native','deny'),
      ('lease-epoch',api,'out->epoch=l.epoch;','out->epoch=l.epoch+1;','native','allow'),
      ('close-error-visible',api,'s->cleanup_error=GT_ERR_IO;','s->cleanup_error=GT_OK;','boundary','close-error'),
      ('destroy-error-retained',api,'if(status!=GT_OK){s->cleanup_error=status;return status;}','if(false && status!=GT_OK){s->cleanup_error=status;return status;}','boundary','destroy-error'),
      ('destroy-history',api,'if(s->released)return GT_EXT_RELEASED;','if(false && s->released)return GT_EXT_RELEASED;','boundary','destroy-error'),
    ]
    swift_cases=[
      ('swift-decision','decision: Decision(rawValue: i.decision.rawValue) ?? .unknown','decision: .allow','testNativeConsentAndScopedLease'),
      ('swift-lease','? Lease(raw: l) : nil','? nil : nil','testNativeConsentAndScopedLease'),
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
            args=['python3',ROOT/'scripts/test-plugin-channel.py','--session','--binary',build/'test_ext_session','--source-root',source,'--case','allow2','--apple']
            if method:args += ['--apple-filter','ExtensionSessionTests.'+method]
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
        shutil.rmtree(source);print('PUBLIC SESSION MUTATIONS PASS: '+str(len(cases))+' C + '+str(len(swift_cases))+' Swift; logs: '+str(logs),flush=True)
    except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
