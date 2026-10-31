#!/usr/bin/env python3
"""Prove the host kill/restart verifier detects lost interruption state and history."""
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_JOURNAL_CRASH_MUTATION'
spec=importlib.util.spec_from_file_location('android_helpers',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require


def main():
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'),'-s',serial];require(run([*adb,'get-state']))
    domain=ROOT/'android/domain/src/main/kotlin/com/hanxi/ghosterm/domain/FileJournal.kt'
    store=ROOT/'android/app/src/main/java/com/hanxi/ghosterm/FileTasksViewModel.kt'
    cases=[('interruption',domain,'phase != FileJournalPhase.FINISHED','phase == FileJournalPhase.FINISHED','published','restarted record must retain identity and interruption'),
        ('history',store,'history.addAll(journal.load())','journal.load()','saf-created','restored history must be visible')]
    originals={p:p.read_bytes() for p in (domain,store)}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual crash mutation')
    work=ROOT/'tmp/android-file-journal-crash-mutations';work.mkdir(exist_ok=True)
    def restore():
        for path,body in originals.items():
            if MARK.encode() in path.read_bytes():path.write_bytes(body)
        if any(hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(b).digest() for p,b in originals.items()):raise RuntimeError('Crash mutation source drift')
    def build(label):
        output=run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/f'{label}-build.log').write_text(output.stdout+output.stderr);require(output)
    def probe(label,point,green,reason=''):
        output=run(['python3',ROOT/'scripts/test-android-file-journal-crash.py','--skip-build','--point',point],seconds=180)
        text=output.stdout+output.stderr;(work/f'{label}.log').write_text(text)
        if green:
            if output.returncode or 'ANDROID FILE JOURNAL CRASH PASS' not in text:raise RuntimeError(text)
        elif output.returncode==0 or 'JOURNAL_CRASH_TARGET_FAIL' not in text or 'FAILURES!!!' not in text or reason not in text:
            raise RuntimeError('Not the expected post-kill assertion: '+text)
    build('baseline')
    for name,path,before,after,point,reason in cases:probe(name+'-baseline',point,True)
    try:
        for name,path,before,after,point,reason in cases:
            body=originals[path].decode();assert body.count(before)==1
            path.write_text(body.replace(before,after)+'\n// '+MARK+'\n')
            build(name);probe(name+'-red',point,False,reason)
            control=run([*adb,'shell','am','instrument','-w','-r','-e','class',
                'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],seconds=60)
            (work/f'{name}-control.log').write_text(control.stdout+control.stderr);require(control)
            if 'OK (1 test)' not in control.stdout:raise RuntimeError(control.stdout)
            restore();build(name+'-restored');probe(name+'-restored',point,True)
            print(f'PASS {name}: real process death target red / same APK config green / restored green / SHA256',flush=True)
    finally:restore()
    print('ANDROID FILE JOURNAL CRASH MUTATIONS PASS: 2 groups',flush=True)


if __name__=='__main__':
    def interrupt(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT,interrupt);signal.signal(signal.SIGTERM,interrupt)
    main()
