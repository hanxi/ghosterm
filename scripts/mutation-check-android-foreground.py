#!/usr/bin/env python3
"""Compiled foreground service mutations over real Android service/notification behavior."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal
import re

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_FOREGROUND_MUTATION'
spec=importlib.util.spec_from_file_location('foreground_runner',ROOT/'scripts/test-android-foreground.py')
runner=importlib.util.module_from_spec(spec);spec.loader.exec_module(runner)
run,require=runner.helper.run,runner.helper.require

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial]
    def command(argv):
        result=run(argv);require(result);return result.stdout
    if int(command([*adb,'shell','getprop','ro.build.version.sdk']).strip())<34:
        raise RuntimeError('All foreground permission mutations require API 34 or newer')
    app=ROOT/'android/app/src/main';service=app/'java/com/hanxi/ghosterm/FileTransferService.kt'
    store=app/'java/com/hanxi/ghosterm/FileTasksViewModel.kt';manifest=app/'AndroidManifest.xml'
    activity=app/'java/com/hanxi/ghosterm/MainActivity.kt'
    prefix='FileForegroundServiceTest#'
    copy='promotedServiceCopiesRealBytesAndStopsOnlyAfterFinalResult'
    rejected='rejectedPlatformStartHoldsSameRequestUntilVisibleUserRetry'
    cancel='privateCancelReceiverUsesGenerationAndRequestIdentity'
    stale='staleAndEmptyServiceIntentsCannotStopCurrentRegistrationOrReplayWork'
    notification='postedNotificationOpensTasksAndCancelsOnlyItsOriginalRequest'
    stopped='actualServiceStopHoldsQueueBeforeCancellingAndNeverWritesWaitingTargets'
    cases=[
        ('hold-state',store,'backgroundHold=reason','backgroundHold=null',rejected),
        ('hold-advance',store,' || backgroundHold!=null','',rejected),
        ('actual-promotion',service,'binding=service;state=FileBackgroundState.ACTIVE','binding=service;state=FileBackgroundState.STARTING',copy),
        ('promotion-release',service,'refreshNotifications();work.tasks.allowBackgroundScheduling();render()','refreshNotifications();render()',copy),
        ('request-not-promoted',service,'generation=token;binding=null;state=FileBackgroundState.STARTING;error=null','generation=token;binding=null;state=FileBackgroundState.ACTIVE;error=null',copy),
        ('cancel-generation',service,'if(accepts(token) && id!=null)','if(id!=null)',cancel),
        ('cancel-one-request',service,'work.tasks.cancelRequest(id)','work.tasks.tasks.filter { it.active }.forEach(work.tasks::cancel)',cancel),
        ('stale-start',service,'if(token==null) stopSelfResult(startId)','stopSelfResult(startId)',stale),
        ('stop-start-id',service,'lastStart=startId','if(lastStart==0) lastStart=startId',stale),
        ('private-service',manifest,'android:name=".FileTransferService" android:exported="false"','android:name=".FileTransferService" android:exported="true"',copy),
        ('private-receiver',manifest,'android:name=".FileTransferActions" android:exported="false"','android:name=".FileTransferActions" android:exported="true"',copy),
        ('service-type',manifest,'android:foregroundServiceType="dataSync"','android:foregroundServiceType="specialUse"',copy),
        ('foreground-permission',manifest,'    <uses-permission android:name="android.permission.FOREGROUND_SERVICE" />\n','',copy),
        ('data-sync-permission',manifest,'    <uses-permission android:name="android.permission.FOREGROUND_SERVICE_DATA_SYNC" />\n','',copy),
        ('notification-privacy',service,'.setContentText(text)','.setContentText(application.filesDir.path)',notification),
        ('notification-open',service,'Intent(this,MainActivity::class.java).setAction(OPEN_TASKS)','Intent(this,MainActivity::class.java).setAction(Intent.ACTION_MAIN)',notification),
        ('notification-cancel',service,'.setAction(CANCEL)','.setAction("com.hanxi.ghosterm.NOOP")',notification),
        ('notification-identity',service,'.setData(android.net.Uri.parse("ghosterm://file-task/$generation/$id"))','.setData(null)',notification),
        ('stop-hold',store,'holdForBackground(reason)\n        tasks.filter','tasks.filter',stopped),
        ('stop-cancel',service,'work.tasks.stopForBackground(error!!)','work.tasks.holdForBackground(error!!)',stopped),
        ('notification-page',activity,'if(fileTasks.visible) { FileTasksScreen(fileTasks);return@MaterialTheme }','if(false && fileTasks.visible) { FileTasksScreen(fileTasks);return@MaterialTheme }',notification),
        ('notification-replay',activity,'if(savedInstanceState==null && intent.action==FileTransferService.OPEN_TASKS) fileTaskPage.show()','if(intent.action==FileTransferService.OPEN_TASKS) fileTaskPage.show()',notification),
        ('paste-service-entry',activity,'fileTasks.startUserTask(snapshot,files.location,','fileTasks.start(snapshot,files.location,','actualFilePagePasteStartsForegroundAndFreezesItsDestination'),
    ]
    if args.case:
        selected=set(args.case.split(','))
        if not selected.issubset({case[0] for case in cases}):raise RuntimeError('Unknown case')
        cases=[case for case in cases if case[0] in selected]
    originals={p:p.read_bytes() for p in (service,store,manifest,activity)}
    if any(b'GT_ANDROID_' in data and b'MUTATION' in data for data in originals.values()):
        raise RuntimeError('Existing Android mutation marker')
    work=ROOT/'tmp/android-foreground-mutations';work.mkdir(exist_ok=True)
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    def restore():
        for path,data in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashes[p] for p in originals):
            raise RuntimeError('Foreground mutation source drift')
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/(label+'-build.log')).write_text(result.stdout+result.stderr);require(result)
        for apk in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            command([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/apk])
    def test(label,selection,green,count=1):
        with runner.notification_access(adb,command):
            result=run([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.'+selection,
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],seconds=240)
            output=result.stdout+result.stderr;(work/(label+'.log')).write_text(output)
            passed=result.returncode==0 and f'OK ({count} '+('test)' if count==1 else 'tests)') in output
            if passed!=green or (not green and ('FAILURES!!!' not in output or 'AssertionError' not in output or not re.search(r'Tests run:\s*1,\s*Failures:\s*1\b',output))):
                print(output,flush=True);raise RuntimeError(label+': expected compiled assertion '+('green' if green else 'red'))
    control='ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    build('baseline');test('baseline','FileForegroundServiceTest',True,8);test('baseline-control',control,True)
    try:
        for name,path,before,after,method in cases:
            source=originals[path].decode()
            if source.count(before)!=1:raise RuntimeError(name+': missing/ambiguous anchor')
            marker='\n<!-- '+MARK+' -->\n' if path.suffix=='.xml' else '\n// '+MARK+'\n'
            path.write_text(source.replace(before,after)+marker)
            build(name);test(name+'-red',prefix+method,False);test(name+'-control',control,True)
            path.write_bytes(originals[path]);restore()
            build(name+'-restored');test(name+'-restored',prefix+method,True)
            print('PASS '+name+': compiled target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    test('final','FileForegroundServiceTest',True,8);test('final-control',control,True)
    print(f'ANDROID FOREGROUND MUTATIONS PASS: {len(cases)} groups',flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGINT,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
