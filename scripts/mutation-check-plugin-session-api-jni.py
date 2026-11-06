#!/usr/bin/env python3
"""Real recompiled public Session JNI variants on an explicit disposable device.
Only this new binding file is temporarily edited; exact bytes are restored on
INT/TERM/failure. Normal single JUnit assertion red, SAME APK Core control green,
then rebuilt/restored target. No private keys enter argv/files or test snapshots.
"""
from pathlib import Path
import argparse,hashlib,json,os,signal,subprocess,sys,tempfile,zipfile
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_SESSION_API_JNI_MUTATION'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
    parser=argparse.ArgumentParser();parser.add_argument('--device',required=True);args=parser.parse_args()
    work=Path(tempfile.mkdtemp(prefix='plugin-session-api-jni-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();commands=[]
    path=ROOT/'android/bridge/src/main/cpp/extension_jni.c';original=path.read_bytes();owned_mutation=None
    (work/'extension_jni.c.original').write_bytes(original)
    if MARK.encode() in original:
        print('residual JNI mutation',file=sys.stderr);raise SystemExit(2)
    inputs=[p for folder in ['core/src','core/include'] for p in (ROOT/folder).rglob('*') if p.is_file() and p.suffix in ('.c','.h','.inc')]
    inputs += [ROOT/p for p in ['core/CMakeLists.txt','core/include/ghosterm.h','android/bridge/src/main/cpp/CMakeLists.txt','android/bridge/src/main/cpp/extension_jni.c','android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ExtensionChannel.kt','android/app/src/androidTest/java/com/hanxi/ghosterm/ExtensionChannelTest.kt','scripts/test-plugin-channel.py','scripts/mutation-check-plugin-session-api-jni.py','android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ExtensionSession.kt','android/app/src/androidTest/java/com/hanxi/ghosterm/ExtensionSessionTest.kt']]
    hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs}
    adb=ROOT/'build/android-toolchain/sdk/platform-tools/adb';device=[str(adb),'-s',args.device];ndk=ROOT/'android/bridge/.cxx/Debug/5v2x5f5z/arm64-v8a';obj=ndk/'CMakeFiles/ghosterm_android.dir/extension_jni.c.o';apk=ROOT/'android/app/build/outputs/apk/debug/app-debug.apk';test_apk=ROOT/'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk'
    env={**os.environ,'TMPDIR':str(work),'ZIG':str(ROOT/'build/android-toolchain/zig-aarch64-macos-0.16.0/zig')}
    def run(label,cmd,budget=240):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-channel-jni; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-channel-jni',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode))
        if p.returncode:raise RuntimeError(label+': '+out)
        return out
    def rebuild(label):
        obj.unlink(missing_ok=True)
        shared=ROOT/'android/bridge/build/intermediates/cxx/Debug/5v2x5f5z/obj/arm64-v8a/libghosterm_android.so'
        shared.unlink(missing_ok=True)
        out=run(label+'-native',['cmake','--build',ndk,'--target','ghosterm_android','--verbose'])
        if not obj.is_file() or 'extension_jni.c' not in out or ' -c ' not in out:raise RuntimeError('JNI not actually compiled: '+label)
        if not shared.is_file() or ' -shared ' not in out or '-o ' not in out:raise RuntimeError('JNI not actually linked: '+label)
        run(label+'-apk',['bash',ROOT/'scripts/build-android.sh',':app:assembleDebug'],budget=600)
        run(label+'-install',[*device,'install','-r',apk],budget=60)
        # Verify bytes actually installed, not just host package existence.
        target=run(label+'-path',[*device,'shell','pm','path','com.hanxi.ghosterm'],budget=15).strip().split('package:')[-1]
        installed=run(label+'-device-sha',[*device,'shell','sha256sum',target],budget=20).split()[0]
        assert installed==sha(apk),label+' installed APK drift'
        with zipfile.ZipFile(apk) as z:library=hashlib.sha256(z.read('lib/arm64-v8a/libghosterm_android.so')).hexdigest()
        (logs/(label+'-artifact.json')).write_text(json.dumps(dict(cpp_sha256=sha(path),object_sha256=sha(obj),apk_sha256=sha(apk),test_apk_sha256=sha(test_apk),installed_apk_sha256=installed,packaged_jni_sha256=library),indent=2)+'\n')
    def suite(label,method=None,red=False):
        cmd=['python3',ROOT/'scripts/test-plugin-channel.py','--session','--case','allow2','--android',args.device]
        if method:cmd += ['--android-test','com.hanxi.ghosterm.ExtensionSessionTest#'+method]
        if red:cmd += ['--expect-platform-assertion','android']
        run(label,cmd,budget=300)
    cases=[
      ('jni-session-worker','    if (s && owner && !pthread_equal(s->owner, pthread_self())) {\n','    if (s && false && owner && !pthread_equal(s->owner, pthread_self())) {\n','workerOwnershipAndCrossThreadStop'),
      ('jni-adoption-generation','        s->id = successor;\n','        (void)successor;\n','staleChannelAndSessionGenerationsCannotStopNewOwner'),
      ('jni-session-unknown','                   i.decision,\n','                   1,\n','consentLeaseAndRevocationRemainScoped'),
      ('jni-session-lease-epoch','        encoded[99 + n] = (uint8_t)(l.epoch >> (56 - n * 8));\n','        encoded[99 + n] = (uint8_t)((l.epoch + 1) >> (56 - n * 8));\n','consentLeaseAndRevocationRemainScoped'),
    ]
    try:
        for label,old,_,_ in cases:
            if old not in original.decode():raise RuntimeError('missing JNI anchor '+label)
        run('install-tests',device+['install','-r',str(test_apk)],budget=60)
        rebuild('baseline');suite('baseline')
        for label,old,new,method in cases:
            owned_mutation=(original.decode().replace(old,new)+'\n/* '+MARK+' */\n').encode();path.write_bytes(owned_mutation)
            try:
                rebuild(label);suite(label+'-target',method,True);variant_sha=sha(apk)
                output=run(label+'-core-control',device+['shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.NativeSmokeTest','com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],budget=120)
                if 'OK (3 tests)' not in output or 'FAILURES' in output:raise RuntimeError(label+' independent Core control: '+output)
                assert sha(apk)==variant_sha
            finally:
                if path.read_bytes() not in (original,owned_mutation):raise RuntimeError('Concurrent JNI edit; original backup retained')
                path.write_bytes(original)
            rebuild(label+'-restored');suite(label+'-restored',method);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': actual NDK/JUnit assertion red / same APK Core green / rebuilt restored target / SHA256',flush=True)
        suite('final')
        for name,h in hashes.items():assert sha(ROOT/name)==h,name
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],device=args.device,inputs_sha256=hashes,commands=commands,scope='native debug API 36 arm64; actual JNI compilation/APK install; normal single JUnit assertion, Core same-APK control, target restored; full Session native/SSH baseline/final; no client UI/request/resource facade/WAN/gateway'),indent=2)+'\n')
        print('PUBLIC SESSION JNI MUTATIONS PASS: '+str(len(cases))+' groups; logs: '+str(logs),flush=True)
    except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
    finally:
        # Unexpected source changes are not silently overwritten.
        current=path.read_bytes()
        if current!=original:
            if current!=owned_mutation:raise RuntimeError('Concurrent JNI edit; retained backup and logs')
            path.write_bytes(original)
        assert sha(path)==hashlib.sha256(original).hexdigest()
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
