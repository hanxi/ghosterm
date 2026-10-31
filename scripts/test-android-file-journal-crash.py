#!/usr/bin/env python3
"""Kill the real instrumented app at a durable checkpoint; restart the same APK/data."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import time
import uuid

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('android_helpers',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require


def main(argv=None,fixture=None):
    parser=argparse.ArgumentParser()
    parser.add_argument('--point',choices=('intent','candidate','published','saf-create-intent','saf-created','remote-create','remote-publish','remote-published','remote-directory'))
    parser.add_argument('--skip-build',action='store_true',help='Use the already built APK pair, still install and verify hashes')
    parser.add_argument('--remote',action='store_true',help='Run real SSH points inside test-android-ssh.sh fixture')
    args=parser.parse_args(argv)
    points=([args.point] if args.point else ['remote-create','remote-publish','remote-published','remote-directory'] if args.remote else ['intent','candidate','published','saf-create-intent','saf-created'])
    if any(point.startswith('remote-') for point in points) and fixture is None:
        raise RuntimeError('Use ANDROID_FILE_JOURNAL_REMOTE_CRASH=1 with test-android-ssh.sh and RemoteFileJournalTest')
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL to the intended test device')
    adb=[str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'),'-s',serial]
    require(run([*adb,'get-state']))
    work=ROOT/'tmp/android-file-journal-crash'/str(time.time_ns());work.mkdir(parents=True)
    if not args.skip_build:
        built=run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/'build.log').write_text(built.stdout+built.stderr);require(built)
    apks=(ROOT/'android/app/build/outputs/apk/debug/app-debug.apk',ROOT/'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk')
    hashes={p:hashlib.sha256(p.read_bytes()).hexdigest() for p in apks}
    for apk,package in zip(apks,('com.hanxi.ghosterm','com.hanxi.ghosterm.test')):
        require(run([*adb,'install','-r','-t',apk]))
        installed=run([*adb,'shell','pm','path',package]);require(installed)
        paths=[line.removeprefix('package:').strip() for line in installed.stdout.splitlines() if line.startswith('package:')]
        if len(paths)!=1:raise RuntimeError('Unexpected APK installation shape')
        actual=run([*adb,'shell','sha256sum',paths[0]]);require(actual)
        if actual.stdout.split()[0]!=hashes[apk]:raise RuntimeError('Installed APK differs from build')
    runner='com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'
    def probe(action,point,token):
        selection='com.hanxi.ghosterm.SafPublicationTest#journalUriIsPersistedBeforeContentWrite' if action=='stage' and point.startswith('saf-') else 'com.hanxi.ghosterm.FileJournalCrashProbe'
        fixture_args=['-e','fixturePort',str(fixture[0]),'-e','fixtureToken',fixture[1]] if point.startswith('remote-') else []
        return [*adb,'shell','am','instrument','-w','-r','-e','class',selection,'-e','journalCrash','true','-e','action',action,'-e','point',point,'-e','token',token,*fixture_args,runner]
    for point in points:
        token=str(uuid.uuid4());ready=f'files/journal-crash-{token}/ready'
        stage_path=work/f'{point}-stage.log'
        stage_log=stage_path.open('w')
        # The shared timeout wrapper bounds a missing checkpoint/crashed adb as well.
        wrapper='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require journal-crash; shift; gt_timeout 60 "$@"'
        process=subprocess.Popen(['bash','-c',wrapper,'journal-crash',str(ROOT),*probe('stage',point,token)],cwd=ROOT,stdout=stage_log,stderr=subprocess.STDOUT)
        pid=None
        try:
            deadline=time.monotonic()+30
            while time.monotonic()<deadline:
                observed=run([*adb,'shell','run-as','com.hanxi.ghosterm','cat',ready],seconds=5)
                if observed.returncode==0:
                    lines=observed.stdout.splitlines()
                    if len(lines)!=3 or lines[2]!=point or not lines[0].isdigit():raise RuntimeError('Invalid checkpoint acknowledgement')
                    pid=lines[0];uuid.UUID(lines[1]);break
                if process.poll() is not None:raise RuntimeError('App exited before required checkpoint: '+stage_path.read_text())
                time.sleep(.1)
            if pid is None:raise RuntimeError('Checkpoint not reached before host deadline: '+stage_path.read_text())
            current=run([*adb,'shell','pidof','com.hanxi.ghosterm']);require(current)
            if pid not in current.stdout.split():raise RuntimeError('Acknowledged process is not alive')
            require(run([*adb,'shell','run-as','com.hanxi.ghosterm','kill','-9',pid]))
            process.wait(timeout=15)
            current=run([*adb,'shell','pidof','com.hanxi.ghosterm'])
            if pid in current.stdout.split():raise RuntimeError('Target process did not exit')
            verified=run(probe('verify',point,token),seconds=30)
            output=verified.stdout+verified.stderr;(work/f'{point}-verify.log').write_text(output)
            require(verified)
            if 'OK (1 test)' not in output or f'JOURNAL_CRASH_VERIFIED point={point} oldPid={pid} ' not in output:
                raise RuntimeError('JOURNAL_CRASH_TARGET_FAIL: '+output)
            control=run([*adb,'shell','am','instrument','-w','-r','-e','class',
                'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],seconds=60)
            (work/f'{point}-control.log').write_text(control.stdout+control.stderr);require(control)
            if 'OK (1 test)' not in control.stdout:raise RuntimeError(control.stdout+control.stderr)
            cleaned=run(probe('cleanup',point,token),seconds=30);require(cleaned)
            if 'OK (1 test)' not in cleaned.stdout:raise RuntimeError(cleaned.stdout)
            print(f'PASS {point}: live PID {pid} killed, new process read original record, actual files checked; same APK config control green',flush=True)
        finally:
            if process.poll() is None:
                if pid:run([*adb,'shell','run-as','com.hanxi.ghosterm','kill','-9',pid])
                process.terminate();process.wait(timeout=15)
            stage_log.close()
    if any(hashlib.sha256(p.read_bytes()).hexdigest()!=digest for p,digest in hashes.items()):raise RuntimeError('APKs changed during crash probes')
    print(f'ANDROID FILE JOURNAL CRASH PASS; evidence {work}',flush=True)


if __name__=='__main__':
    def interrupt(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT,interrupt);signal.signal(signal.SIGTERM,interrupt)
    main()
