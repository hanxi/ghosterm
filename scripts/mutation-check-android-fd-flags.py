#!/usr/bin/env python3
"""Actual API-29-compatible fd flags, pipe behavior, errno and descriptor ownership."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_FD_FLAGS_MUTATION'
spec=importlib.util.spec_from_file_location('native_helper',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)


def sources():
    names=set(subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard'],cwd=ROOT,text=True).splitlines())
    return {name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in sorted(names)
        if not name.endswith('.md') and not name.startswith('vendor/') and (ROOT/name).is_file()}


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case',type=int,action='append',dest='selected');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL to the intended test device')
    adb=['adb','-s',serial];helper.require(helper.run([*adb,'get-state']))
    path=ROOT/'android/bridge/src/main/cpp/staging_jni.c';original=path.read_bytes()
    if re.search(rb'GT_[A-Z0-9_]*MUTATION',original):raise RuntimeError('Residual native mutation')
    prefix='FileDescriptorFlagsTest#'
    append=prefix+'statusChangesPreserveAppendAndCloseTheirTemporaryDuplicates'
    pipe=prefix+'nonblockingChangesTheOriginalPipeAndRestoreMakesItWaitAgain'
    descriptor=prefix+'descriptorFlagsObserveTheOriginalSlotAndKeepOtherDuplicatesIndependent'
    error=prefix+'invalidDescriptorPreservesTheOperatingSystemError'
    cases=[
        ('int result=fcntl(fd,F_GETFL);','int result=fcntl(fd,F_GETFL); if(result>=0) result &= ~O_APPEND;',append),
        ('if(fcntl(fd,F_SETFL,flags)<0)','if(0 && fcntl(fd,F_SETFL,flags)<0)',pipe),
        ('fcntl(fd,F_SETFL,flags)','fcntl(fd,F_SETFL,flags | O_NONBLOCK)',pipe),
        ('fcntl(fd,F_SETFL,flags)','fcntl(fd,F_SETFL,flags & ~O_APPEND)',append),
        ('int result=fcntl(fd,F_GETFD);','int result=fcntl(fd,F_GETFD); if(result>=0) result |= FD_CLOEXEC;',descriptor),
        ('if(failure)(*env)->Throw(env,(jthrowable)failure);','(void)failure;',error),
        ('int result=fcntl(fd,F_GETFL);','int result=fcntl(dup(fd),F_GETFL);',append),
        ('int result=fcntl(fd,F_GETFL);','int result=fcntl(fd,F_GETFL); if(result>=0) result &= ~O_ACCMODE;',append),
        ('(jint)error);','(jint)(error==EBADF?ENOENT:error));',error),
    ]
    selected=args.selected or list(range(1,len(cases)+1))
    if any(n<1 or n>len(cases) for n in selected):raise RuntimeError('case out of range')
    work=ROOT/'tmp/android-fd-flags-mutations'/str(time.time_ns());work.mkdir(parents=True)
    before=sources();(work/'sources-before.json').write_text(json.dumps(before,indent=2)+'\n')
    def build(label):
        result=helper.run(['bash',ROOT/'scripts/build-android.sh'],900)
        (work/(label+'-build.log')).write_text(result.stdout+result.stderr);helper.require(result)
        for name in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
            helper.require(helper.run([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/name]))
    def test(name,green,label):
        result=helper.run([*adb,'shell','am','instrument','-w','-r','-e','class',
            'com.hanxi.ghosterm.'+name,'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'],120)
        output=result.stdout+result.stderr;(work/(label+'.log')).write_text(output);helper.require(result)
        good=('OK (1 test)' in output and 'FAILURES' not in output) if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1',output)) and any(
                kind in output for kind in ('java.lang.AssertionError','org.junit.ComparisonFailure')))
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:raise RuntimeError(output)
    try:
        build('baseline')
        for n,target in enumerate((append,pipe,descriptor,error)):test(target,True,f'baseline-{n}')
        for number in selected:
            old,new,target=cases[number-1];source=original.decode()
            if source.count(old)!=1:raise RuntimeError(f'D{number}: missing or ambiguous anchor')
            path.write_text(source.replace(old,new+' /* '+MARK+' */'))
            label=f'D{number}';build(label);test(target,False,label+'-target')
            test('ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',True,label+'-control')
            path.write_bytes(original);build(label+'-restored');test(target,True,label+'-restored')
            if hashlib.sha256(path.read_bytes()).digest()!=hashlib.sha256(original).digest():raise RuntimeError('restore SHA256 mismatch')
            print(label+': compiled single target assertion red / same APK config green / restored green / SHA256',flush=True)
    finally:
        if MARK.encode() in path.read_bytes():path.write_bytes(original)
        after=sources();(work/'sources-after.json').write_text(json.dumps(after,indent=2)+'\n')
        if before!=after:raise RuntimeError('Source set or SHA256 changed during fd mutations')
        helper.require(helper.run([*adb,'shell','am','force-stop','com.hanxi.ghosterm']))
    print(f'ANDROID FD FLAGS MUTATIONS PASS: {len(selected)} groups; receipts {work}',flush=True)


if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    main()
