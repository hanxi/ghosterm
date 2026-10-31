#!/usr/bin/env python3
"""Real system-timeout assertions must reject compiled production mutants."""
import hashlib
import argparse
import importlib.util
import os
from pathlib import Path
import re
import signal
import time

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('fg',ROOT/'scripts/test-android-foreground.py')
fg=importlib.util.module_from_spec(spec);spec.loader.exec_module(fg)
MARK='GT_ANDROID_FOREGROUND_SYSTEM_MUTATION'


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial]
    source=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/FileTransferService.kt'
    screen=source.with_name('FileTasksScreen.kt')
    originals={p:p.read_bytes() for p in (source,screen)}
    if any(b'MUTATION' in data for data in originals.values()):raise RuntimeError('Existing mutation marker')
    work=ROOT/'tmp/android-foreground-system-mutations'/str(time.time_ns());work.mkdir(parents=True)
    def system(label,green,mode="timeout"):
        before=set((ROOT/'tmp/android-foreground-system').glob('*'))
        result=fg.helper.run(['python3',ROOT/'scripts/test-android-foreground-system.py','--skip-build','--mode',mode],seconds=150)
        (work/(label+'.log')).write_text(result.stdout+result.stderr)
        folders=set((ROOT/'tmp/android-foreground-system').glob('*'))-before
        if len(folders)!=1:raise RuntimeError('Ambiguous system evidence directory')
        folder=folders.pop();output=(folder/(mode+'.log' if mode in ('notifications-denied','channel-closed') else 'timeout-stage.log')).read_text()
        (work/(label+'-assertion.log')).write_text(output)
        if green:
            if result.returncode!=0 or ('FOREGROUND SYSTEM PASS '+mode) not in result.stdout:raise RuntimeError(result.stdout+result.stderr)
        elif result.returncode==0 or 'AssertionError' not in output or not re.search(r'Tests run:\s*1,\s*Failures:\s*1\b',output):
            raise RuntimeError('Expected exactly one compiled JUnit assertion failure: '+output)
    def build(label):
        result=fg.helper.run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/(label+'-build.log')).write_text(result.stdout+result.stderr);fg.helper.require(result)
    def control(label):
        with fg.notification_access(adb,lambda argv:checked(argv)):
            result=fg.helper.run([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig','com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],seconds=60)
            (work/(label+'-control.log')).write_text(result.stdout+result.stderr);fg.helper.require(result)
            if 'OK (1 test)' not in result.stdout:raise RuntimeError(result.stdout+result.stderr)
    def checked(argv):
        result=fg.helper.run(argv);fg.helper.require(result);return result.stdout
    cases=[
        ('channel-visibility',source,'manager.getNotificationChannel(FileTransferService.CHANNEL)?.importance!=NotificationManager.IMPORTANCE_NONE','true','channel-closed'),
        ('timeout-classification',source,'owner.ended(this,token,true)','owner.ended(this,token,false)','timeout'),
        ('timeout-cancel',source,'work.tasks.stopForBackground(error!!)','work.tasks.holdForBackground(error!!)','timeout'),
        ('timeout-stop',source,'owner.ended(this,token,true)\n        stopForeground(STOP_FOREGROUND_REMOVE);stopSelf()',
         'owner.ended(this,token,true)\n        stopForeground(STOP_FOREGROUND_REMOVE);android.os.Handler(mainLooper).postDelayed({ stopSelf() },4000)','timeout'),
        ('notification-denied-visibility',source,'notificationsVisible=manager.areNotificationsEnabled() &&','notificationsVisible=true || manager.areNotificationsEnabled() &&','notifications-denied'),
        ('notification-denied-eligibility',source,'check(info.foregroundServiceType and ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC!=0)',
         'check(application.checkSelfPermission(android.Manifest.permission.POST_NOTIFICATIONS)==android.content.pm.PackageManager.PERMISSION_GRANTED);check(info.foregroundServiceType and ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC!=0)','notifications-denied'),
        ('notification-denied-warning',screen,'if(!model.background.notificationsVisible)','if(false && !model.background.notificationsVisible)','notifications-denied'),
    ]
    if args.case:
        selected=set(args.case.split(','))
        if not selected.issubset({case[0] for case in cases}):raise RuntimeError('Unknown case')
        cases=[case for case in cases if case[0] in selected]
    def restore():
        for path,data in originals.items():
            path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(data).digest():raise RuntimeError('Restore SHA mismatch')
    build('baseline')
    for mode in sorted({case[-1] for case in cases}):system('baseline-'+mode,True,mode)
    try:
        for name,path,before,after,mode in cases:
            text=originals[path].decode()
            if text.count(before)!=1:raise RuntimeError('Missing mutation anchor '+name)
            path.write_text(text.replace(before,after)+'\n// '+MARK+'\n')
            build(name);system(name,False,mode);control(name)
            restore();build(name+'-restored');system(name+'-restored',True,mode)
            print('PASS '+name+': actual system behavior, compiled assertion red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    print(f'ANDROID FOREGROUND SYSTEM MUTATIONS PASS: {len(cases)} groups; evidence '+str(work),flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
