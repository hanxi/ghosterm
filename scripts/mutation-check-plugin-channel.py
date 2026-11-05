#!/usr/bin/env python3
"""Isolated compiled public-channel and Swift binding mutations. Every target
must fail by its normal assertion, same archive config stays green, restored
target is rebuilt, and complete native/SSH suites pass at the boundaries.
"""
from pathlib import Path
import sys
import hashlib,json,os,shutil,signal,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_CHANNEL_MUTATION'
NATIVE='invalid copy partial quantum eof close-queued stop failure-stop rejected-fd capacity'.split()
BOUNDARY='allocation constructor-failure wait-stop draining close-error post-write-stop post-read-stop counter'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-channel-mutations-',dir=ROOT/'tmp'));source=work/'source';core=source/'core';build=core/'build';logs=work/'logs';logs.mkdir();fixtures=work/'fixtures';fixtures.mkdir();commands=[]
    inputs=[p for folder in ['core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt','core/tests/test_ext_channel.c','core/tests/test_ext_channel_boundary.c','core/tests/ext_channel_dial.c','scripts/test-plugin-channel.py','scripts/mutation-check-plugin-channel.py','apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift','apple/GhostermCore/Tests/GhostermCoreTests/ExtensionChannelTests.swift']]
    hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','DEVELOPER_DIR':'/Applications/Xcode.app/Contents/Developer'}
    def run(label,args,red=False,budget=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-channel-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-channel-mutations',str(ROOT),str(budget),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion_target=red))
        if red:
            if p.returncode!=1 or 'FAIL ' not in out or any(x in out for x in ['AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:']):raise RuntimeError(label+': expected normal C assertion\n'+out)
        elif p.returncode:raise RuntimeError(label+': '+out)
    def rebuild(label):
        # GNU make can rebuild .o/.a within one second without relinking tests.
        binaries=['test_ext_channel', 'test_ext_channel_boundary', 'test_config']
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        objects=[('ghosterm_core','src/extension/channel.c'),('test_ext_channel_boundary','tests/test_ext_channel_boundary.c'),('test_ext_channel','tests/test_ext_channel.c')]
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_ext_channel','test_ext_channel_boundary','test_config','-j','2'])
        link_log=(logs/(label+'-build.log')).read_text()
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in link_log:raise RuntimeError('Missing actual link: '+label+' '+marker)
        out=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in out,label+' did not recompile '+name
    def target(label,kind,mode,red):
        if kind=='ssh':run(label,['python3',ROOT/'scripts/test-plugin-channel.py','--binary',build/'test_ext_channel','--source-root',source,'--case',mode,*(['--expect-assertion'] if red else [])],budget=60)
        else:run(label,[build/('test_ext_channel_boundary' if kind=='boundary' else 'test_ext_channel'),mode,*([fixtures] if kind=='native' else [])],red=red,budget=20)
    def suite(label):
        for mode in NATIVE:target(label+'-'+mode,'native',mode,False)
        for mode in BOUNDARY:target(label+'-'+mode,'boundary',mode,False)
        run(label+'-ssh',['python3',ROOT/'scripts/test-plugin-channel.py','--binary',build/'test_ext_channel','--source-root',source],budget=280)
    cases=[
      ('copy-native-storage',[('memcpy(c->write_buffer,p,n);','memset(c->write_buffer,0,n);')],'native','copy'),
      ('queue-backpressure',[('c->write_at<c->write_size)return (gt_ext_io_t){GT_EXT_WAIT,0,GT_OK};','c->write_at<c->write_size)return (gt_ext_io_t){GT_EXT_DATA,0,GT_OK};')],'native','copy'),
      ('quantum',[('n=sizeof(c->write_buffer);','n=sizeof(c->write_buffer)-1;')],'native','quantum'),
      ('wait-borrow-lifetime',[('c->info.challenge=0;c->waiting=false;}','c->info.challenge=0;c->waiting=false;channel_discard(c);}')],'boundary','wait-stop'),
      ('draining-borrow-lifetime',[('gt_bridge_close_t r=gt__bridge_stream_close(c->stream,ms);','channel_discard(c);gt_bridge_close_t r=gt__bridge_stream_close(c->stream,ms);')],'boundary','draining'),
      ('partial-write-offset',[('c->write_at+=r.count;','c->write_at=c->write_size;')],'native','copy'),
      ('observed-write',[('c->info.sent+=r.count;','c->info.sent+=0;')],'boundary','post-write-stop'),
      ('observed-read',[('c->info.received+=r.count;','c->info.received+=0;')],'boundary','post-read-stop'),
      ('capacity',[('if(count>=GT_EXT_CHANNEL_LIMIT)','if(false && count>=GT_EXT_CHANNEL_LIMIT)')],'boundary','allocation'),
      ('allocation-unwind',[('if(!c)atomic_fetch_sub_explicit','if(false && !c)atomic_fetch_sub_explicit')],'boundary','allocation'),
      ('constructor-unwind',[('if(status!=GT_OK){channel_free(c);return status;}','if(status!=GT_OK){return status;}')],'boundary','constructor-failure'),
      ('owner-release',[('free(c);atomic_fetch_sub_explicit','free(c);return;atomic_fetch_sub_explicit')],'boundary','allocation'),
      ('owner-cleared',[('channel_free(c);*owned=NULL;return GT_OK;','channel_free(c);return GT_OK;')],'boundary','allocation'),
      ('accepted-overflow',[('if(UINT64_MAX-c->info.accepted<n)','if(false && UINT64_MAX-c->info.accepted<n)')],'boundary','counter'),
      ('failure-preserved',[('if(c->info.phase==GT_EXT_FAILED || c->info.phase==GT_EXT_STOPPED)return c->info.phase;','if(false && (c->info.phase==GT_EXT_FAILED || c->info.phase==GT_EXT_STOPPED))return c->info.phase;')],'native','failure-stop'),
      ('cleanup-error-visible',[('c->info.cleanup_error=GT_ERR_IO;','c->info.cleanup_error=GT_OK;')],'boundary','close-error'),
      ('unknown-not-accept',[('c->info.decisions[i]=GT_HOSTKEY_UNKNOWN;','c->info.decisions[i]=GT_HOSTKEY_ACCEPT;')],'ssh','credentials2'),
      ('reply-hop',[(' || hop!=c->info.hop ||',' || (false && hop!=c->info.hop) ||')],'ssh','unknown-accept'),
      ('reply-token',[('challenge!=c->info.challenge ||','false ||')],'ssh','credentials2'),
      ('reply-rotation',[('c->info.challenge=++c->challenge_counter;','c->info.challenge=1;')],'ssh','credentials-retry'),
      ('setup-budget',[('options?&opts:NULL,NULL,&c->stream','(options && false)?&opts:NULL,NULL,&c->stream')],'ssh','expired-before-supply'),
    ]
    swift_cases=[
      ('swift-count','count: value.count,','count: 0,','testCopiedTemporaryDataAndBidirectionalIO'),
      ('swift-buffered','buffered: i.buffered,','buffered: 0,','testCopiedTemporaryDataAndBidirectionalIO'),
      ('swift-exit-unknown','exitStatus: i.exit_status_known ? i.exit_status : nil','exitStatus: i.exit_status','testCopiedTemporaryDataAndBidirectionalIO'),
    ]
    try:
        for name in ['core/src/extension/channel.c','apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift']:
            if MARK.encode() in (ROOT/name).read_bytes():print('residual mutation '+name,file=sys.stderr);raise SystemExit(2)
        text=(ROOT/'core/src/extension/channel.c').read_text()
        for label,changes,_,_ in cases:
            for old,_ in changes:
                if old not in text:raise RuntimeError('missing anchor '+label)
        shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build','build-*'))
        shutil.copytree(ROOT/'apple/GhostermCore',source/'apple/GhostermCore',symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
        run('configure',['cmake','-S',core,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');run('baseline-config',[build/'test_config',core/'tests/fixtures'])
        for label,changes,kind,mode in cases:
            path=core/'src/extension/channel.c';original=path.read_bytes();mutated=original.decode()
            for old,new in changes:mutated=mutated.replace(old,new)
            path.write_text(mutated+'\n/* '+MARK+' */\n')
            try:rebuild(label);target(label+'-target',kind,mode,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            rebuild(label+'-restored');target(label+'-restored-target',kind,mode,False);assert sha(path)==hashlib.sha256(original).hexdigest()
            print(label+': compiled assertion red / same archive config green / rebuilt target restored / SHA256',flush=True)
        suite('final')
        # Swift uses the same restored Core source, rebuilt without C sanitizers;
        # no claim that the platform library or crypto dependency has LSan proof.
        run('swift-core-configure',['cmake','-S',core,'-B',build,'-DCMAKE_C_FLAGS='])
        rebuild('swift-core')
        def swift(label,method=None,red=False):
            args=['python3',ROOT/'scripts/test-plugin-channel.py','--binary',build/'test_ext_channel','--source-root',source,'--case','echo2','--apple']
            if method:args += ['--apple-filter','ExtensionChannelTests.'+method]
            if red:args += ['--expect-platform-assertion','apple']
            run(label,args,budget=240)
        swift('swift-baseline')
        for label,old,new,method in swift_cases:
            path=source/'apple/GhostermCore/Sources/GhostermCore/ExtensionChannel.swift';original=path.read_bytes();assert old in original.decode()
            path.write_text(original.decode().replace(old,new)+'\n// '+MARK+'\n')
            try:swift(label,method,True);run(label+'-config',[build/'test_config',core/'tests/fixtures'])
            finally:path.write_bytes(original)
            swift(label+'-restored',method);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': actual Swift assertion red / config green / rebuilt target restored / SHA256',flush=True)
        swift('swift-final')
        for name,h in hashes.items():assert sha(ROOT/name)==h,name
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,core_groups=[c[0] for c in cases],swift_groups=[c[0] for c in swift_cases],inputs_sha256=hashes,commands=commands,c_sanitizers=['address','undefined'],leak_sanitizer=False,restoration='each target rebuilt; native/SSH suites baseline/final; Swift native/SSH baseline/final'),indent=2)+'\n')
        shutil.rmtree(source);shutil.rmtree(fixtures);print('PUBLIC CHANNEL MUTATIONS PASS: '+str(len(cases))+' C + '+str(len(swift_cases))+' Swift; logs: '+str(logs),flush=True)
    except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
