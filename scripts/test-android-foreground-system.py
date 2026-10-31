#!/usr/bin/env python3
"""Actual Android FGS timeout and Task Manager stop-app; same APK and durable data."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import signal
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('foreground',ROOT/'scripts/test-android-foreground.py')
fg=importlib.util.module_from_spec(spec);spec.loader.exec_module(fg)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--skip-build',action='store_true')
    parser.add_argument('--mode',choices=('timeout','stop-app','notifications-denied','channel-closed','notifications-restored'));args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'),'-s',serial]
    work=ROOT/'tmp/android-foreground-system'/str(time.time_ns());work.mkdir(parents=True)
    def command(argv,seconds=30):
        result=fg.helper.run(argv,seconds=seconds);fg.helper.require(result);return result.stdout
    if int(command([*adb,'shell','getprop','ro.build.version.sdk']).strip())<35:
        raise RuntimeError('Real dataSync timeout requires API 35+; no simulated callback fallback')
    if not args.skip_build:
        result=fg.helper.run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/'build.log').write_text(result.stdout+result.stderr);fg.helper.require(result)
    apks=[ROOT/'android/app/build/outputs'/path for path in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk')]
    hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in apks}
    for apk,package in zip(apks,('com.hanxi.ghosterm','com.hanxi.ghosterm.test')):
        command([*adb,'install','-r','-t',apk])
        paths=command([*adb,'shell','pm','path',package]).splitlines()
        if len(paths)!=1 or not paths[0].startswith('package:'):raise RuntimeError('Unexpected installation layout')
        if command([*adb,'shell','sha256sum',paths[0][8:]]).split()[0]!=hashes[str(apk)]:raise RuntimeError('Installed APK differs')
    runner='com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'
    selection='com.hanxi.ghosterm.FileForegroundServiceTest#actualServiceStopHoldsQueueBeforeCancellingAndNeverWritesWaitingTargets'
    def probe(mode,phase,token):
        return [*adb,'shell','am','instrument','-w','-r','-e','class',selection,
                '-e','foregroundSystemMode',mode,'-e','foregroundSystemPhase',phase,'-e','foregroundSystemToken',token,runner]
    def marker(token,phase,process,seconds=35):
        end=time.monotonic()+seconds
        while time.monotonic()<end:
            result=fg.helper.run([*adb,'shell','run-as','com.hanxi.ghosterm','cat',f'cache/foreground-system-{token}-{phase}.json'],seconds=5)
            if result.returncode==0:return json.loads(result.stdout)
            if process.poll() is not None:raise RuntimeError('Instrumentation exited before '+phase)
            time.sleep(.1)
        raise RuntimeError('Missing actual system checkpoint '+phase)
    permission_modes=([args.mode] if args.mode in ('notifications-denied','notifications-restored') else ['notifications-denied','notifications-restored'] if args.mode is None else [])
    for permission_mode in permission_modes:
        permission='android.permission.POST_NOTIFICATIONS'
        initial='granted=true' in next(line for line in command([*adb,'shell','dumpsys','package','com.hanxi.ghosterm']).splitlines() if 'android.permission.POST_NOTIFICATIONS: granted=' in line)
        try:
            command([*adb,'shell','pm','revoke','com.hanxi.ghosterm',permission])
            result=fg.helper.run(probe(permission_mode,'stage',str(uuid.uuid4())),seconds=60)
            output=result.stdout+result.stderr;(work/(permission_mode+'.log')).write_text(output);fg.helper.require(result)
            if 'OK (1 test)' not in output or ('FOREGROUND_SYSTEM_VERIFIED mode='+permission_mode) not in output:raise RuntimeError(output)
        finally:
            command([*adb,'shell','pm','grant' if initial else 'revoke','com.hanxi.ghosterm',permission])
            restored='granted=true' in next(line for line in command([*adb,'shell','dumpsys','package','com.hanxi.ghosterm']).splitlines() if 'android.permission.POST_NOTIFICATIONS: granted=' in line)
            if restored!=initial:raise RuntimeError('Notification permission was not restored')
        print('FOREGROUND SYSTEM PASS '+permission_mode+': actual permission state, active service and task behavior checked; permission restored',flush=True)
        if args.mode in ('notifications-denied','notifications-restored'):
            print('Evidence: '+str(work),flush=True);return
    if args.mode in (None,'channel-closed'):
        with fg.notification_access(adb,command):
            # A fresh installation may have no channel until its first real service.
            baseline=command([*adb,'shell','am','instrument','-w','-r','-e','class',
                'com.hanxi.ghosterm.FileForegroundServiceTest#promotedServiceCopiesRealBytesAndStopsOnlyAfterFinalResult',runner],seconds=60)
            (work/'channel-setup.log').write_text(baseline)
            if 'OK (1 test)' not in baseline:raise RuntimeError(baseline)
            result=fg.helper.run(probe('channel-closed','stage',str(uuid.uuid4())),seconds=90)
            output=result.stdout+result.stderr;(work/'channel-closed.log').write_text(output);fg.helper.require(result)
            if 'OK (1 test)' not in output or 'FOREGROUND_SYSTEM_VERIFIED mode=channel-closed' not in output:raise RuntimeError(output)
        print('FOREGROUND SYSTEM PASS channel-closed: real Settings channel switch, permission still granted, active service and copy; channel restored',flush=True)
        if args.mode=='channel-closed':
            print('Evidence: '+str(work),flush=True);return
    key='data_sync_fgs_timeout_duration'
    for mode in ([args.mode] if args.mode else ['timeout','stop-app']):
        token=str(uuid.uuid4());process=None
        previous=command([*adb,'shell','device_config','get','activity_manager',key]).strip()
        try:
            if mode=='timeout':
                command([*adb,'shell','device_config','put','activity_manager',key,'2000'])
                if command([*adb,'shell','device_config','get','activity_manager',key]).strip()!='2000':raise RuntimeError('Timeout setting not applied')
            with fg.notification_access(adb,command):
                stage_path=work/f'{mode}-stage.log'
                with stage_path.open('w') as log:
                    wrapper='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require foreground-system; shift; gt_timeout 100 "$@"'
                    process=subprocess.Popen(['bash','-c',wrapper,'foreground-system',str(ROOT),*probe(mode,'stage',token)],cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
                    ready=marker(token,'ready',process)
                    if ready['mode']!=mode:raise RuntimeError('Wrong checkpoint mode')
                    (work/f'{mode}-ready.json').write_text(json.dumps(ready,indent=2))
                    if str(ready['pid']) not in command([*adb,'shell','pidof','com.hanxi.ghosterm']).split():raise RuntimeError('Ready process is not alive')
                    if mode=='timeout':
                        command([*adb,'shell','input','keyevent','KEYCODE_HOME'])
                        stopped=marker(token,'timed-out',process,seconds=30)
                        if stopped!=ready:raise RuntimeError('Timeout checkpoint changed identity')
                        command([*adb,'shell','run-as','com.hanxi.ghosterm','touch',f'cache/foreground-system-{token}-release'])
                        process.wait(timeout=30)
                        if process.returncode!=0 or 'OK (1 test)' not in stage_path.read_text() or 'FOREGROUND_SYSTEM_VERIFIED mode=timeout' not in stage_path.read_text():raise RuntimeError(stage_path.read_text())
                    else:
                        command([*adb,'shell','cmd','activity','stop-app','com.hanxi.ghosterm'])
                        process.wait(timeout=15)
                        current=fg.helper.run([*adb,'shell','pidof','com.hanxi.ghosterm'])
                        if str(ready['pid']) in current.stdout.split():raise RuntimeError('System stop-app did not kill old PID')
                        # Interrupted staging is expected; only the new-process verification counts.
                        verified=fg.helper.run(probe(mode,'verify',token),seconds=45)
                        output=verified.stdout+verified.stderr;(work/f'{mode}-verify.log').write_text(output);fg.helper.require(verified)
                        if 'OK (1 test)' not in output or 'FOREGROUND_SYSTEM_VERIFIED mode=stop-app' not in output:raise RuntimeError(output)
                control=command([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',runner],seconds=60)
                (work/f'{mode}-control.log').write_text(control)
                if 'OK (1 test)' not in control:raise RuntimeError(control)
        finally:
            if process is not None and process.poll() is None:
                fg.helper.run([*adb,'shell','cmd','activity','stop-app','com.hanxi.ghosterm']);process.wait(timeout=15)
            if mode=='timeout':
                command([*adb,'shell','device_config','delete','activity_manager',key] if previous=='null' else [*adb,'shell','device_config','put','activity_manager',key,previous])
                if command([*adb,'shell','device_config','get','activity_manager',key]).strip()!=previous:raise RuntimeError('System timeout configuration was not restored')
        for apk in apks:
            if hashlib.sha256(apk.read_bytes()).hexdigest()!=hashes[str(apk)]:raise RuntimeError('APK changed during system verification')
        print(f'FOREGROUND SYSTEM PASS {mode}: actual system action, side effects and durable record checked; config control green; system settings restored',flush=True)
    (work/'apk-hashes.json').write_text(json.dumps(hashes,indent=2));print('Evidence: '+str(work),flush=True)

if __name__=='__main__':
    signal.signal(signal.SIGTERM,lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
