#!/usr/bin/env python3
"""Device connection admission: production JNI/page mutation and same-APK config control."""
import hashlib, importlib.util, os, signal
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_CONNECTION_CONFIG_MUTATION'
spec=importlib.util.spec_from_file_location('helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)

def main():
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL')
    adb=['adb','-s',serial]; helper.require(helper.run([*adb,'get-state']))
    native=ROOT/'android/bridge/src/main/cpp/config_jni.c'
    bridge=ROOT/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/ConfigBridge.kt'
    model=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/HostsViewModel.kt'
    screen=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    originals={p:p.read_bytes() for p in (native,bridge,model,screen)}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('residual mutation')
    logs=ROOT/'tmp/android-connection-config-mutations'; logs.mkdir(parents=True,exist_ok=True)
    binding='ConfigRepositoryTest#connectionAdmissionReportsSharedSourcesWithoutEditing'
    page='HostScreenTest#hostClickReportsUnsupportedConfigurationAndLeavesEditorAvailable'
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    cases=[
      (native,'result=(*env)->NewObjectArray(env,(jsize)(count*4),type,NULL);','count=0; result=(*env)->NewObjectArray(env,(jsize)(count*4),type,NULL);',binding),
      (native,'info.keyword,info.source_path,line,info.included?', 'info.keyword,info.source_path,"0",info.included?',binding),
      (bridge,'.chunked(4).map { ConnectionIssue(','.chunked(4).take(0).map { ConnectionIssue(',binding),
      (model,'if (issues.isNotEmpty()) error = "不支持 SSH 配置："','if (issues.isNotEmpty()) error = "配置可直接连接："',page),
      (screen,'onClick = { model.checkConnection(host, sessions::connect) }','onClick = { model.edit(host) }',page),
    ]
    def build(label):
        r=helper.run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (logs/(label+'-build.log')).write_text(r.stdout+r.stderr); helper.require(r)
        for path in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            helper.require(helper.run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/path]))
    def test(label,method,green):
        r=helper.run([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.'+method,
          'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        out=r.stdout+r.stderr; (logs/(label+'.log')).write_text(out)
        passed=r.returncode==0 and 'OK (1 test)' in out
        if passed!=green or (not green and ('FAILURES' not in out or not any(x in out for x in ('AssertionError','ComparisonFailure')))):
            raise RuntimeError(out)
    try:
        build('baseline')
        for method in (binding,page,control): test('baseline-'+method,method,True)
        for i,(path,old,new,method) in enumerate(cases,1):
            text=originals[path].decode()
            if text.count(old)!=1: raise RuntimeError('ambiguous anchor: '+old)
            path.write_text(text.replace(old,new+' /* '+MARK+' */'))
            label='A'+str(i); build(label); test(label,method,False); test(label+'-control',control,True)
            path.write_bytes(originals[path]); assert hashlib.sha256(path.read_bytes()).digest()==hashes[path]
            build(label+'-restored'); test(label+'-restored',method,True)
            print(label+': compiled target red / same APK control green / restored green / SHA256',flush=True)
    finally:
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()): raise RuntimeError('source changed')
    print(f'ANDROID CONNECTION CONFIG MUTATIONS PASS: {len(cases)} groups')

if __name__=='__main__':
    def interrupt(signum,frame): raise KeyboardInterrupt(signum)
    for sig in (signal.SIGINT,signal.SIGTERM): signal.signal(sig,interrupt)
    main()
