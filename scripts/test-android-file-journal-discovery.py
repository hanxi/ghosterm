#!/usr/bin/env python3
"""Verify default JUnit discovery excludes host-only probes; log-only is not execution."""
import argparse
from collections import Counter
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import time

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_ANDROID_JOURNAL_DISCOVERY_MUTATION'
RUNNER='com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'
PROBE='com.hanxi.ghosterm.FileJournalCrashProbe'
spec=importlib.util.spec_from_file_location('android_helpers',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
run,require=helper.run,helper.require


class DiscoveryFailure(AssertionError):
    pass


def discovered(output):
    records=[];status={}
    for line in output.splitlines():
        if line.startswith('INSTRUMENTATION_STATUS: '):
            key,separator,value=line.removeprefix('INSTRUMENTATION_STATUS: ').partition('=')
            if separator: status[key]=value
        elif line.startswith('INSTRUMENTATION_STATUS_CODE: '):
            records.append((int(line.split(': ',1)[1]),status));status={}
    for _,entry in records:
        if entry.get('class')==PROBE or 'Host-only file journal crash probes' in entry.values():
            raise DiscoveryFailure('host-only probe was discovered without journalCrash')
    starts=[entry for code,entry in records if code==1]
    ends=[entry for code,entry in records if code==0 and 'class' in entry]
    if not starts or any(code not in (0,1) for code,_ in records):
        raise DiscoveryFailure('discovery has no tests or reports failure/skip')
    def identity(entry):
        if not entry.get('class') or not entry.get('test'):
            raise DiscoveryFailure('discovery contains an unnamed test')
        return entry['class']+'#'+entry['test']
    names=[identity(entry) for entry in starts]
    if len(set(names))!=len(names) or Counter(names)!=Counter(identity(entry) for entry in ends):
        raise DiscoveryFailure('discovery start/end identities do not match uniquely')
    if any(entry.get('numtests')!=str(len(names)) for entry in starts):
        raise DiscoveryFailure('discovery declared count differs from actual named tests')
    summary=re.search(r'OK \((\d+) tests?\)',output)
    if summary is None or int(summary[1])!=len(names) or 'INSTRUMENTATION_CODE: -1' not in output:
        raise DiscoveryFailure('discovery final count is missing or inconsistent')
    anchors={'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',
        'com.hanxi.ghosterm.FileJournalTest#failedPlanTransactionKeepsOriginalIntentAndNoPartialItems'}
    if not anchors.issubset(names):
        raise DiscoveryFailure('ordinary config/journal tests are missing from default discovery')
    return sorted(names)


def main():
    parser=argparse.ArgumentParser();parser.add_argument('--mutate',action='store_true');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to the intended device')
    adb=[str(ROOT/'build/android-toolchain/sdk/platform-tools/adb'),'-s',serial];require(run([*adb,'get-state']))
    source=ROOT/'android/app/src/androidTest/java/com/hanxi/ghosterm/FileJournalCrashRunner.kt'
    original=source.read_bytes();digest=hashlib.sha256(original).digest()
    if MARK.encode() in original: raise RuntimeError('Residual discovery mutation')
    work=ROOT/'tmp/android-file-journal-discovery'/str(time.time_ns());work.mkdir(parents=True)
    def build(label):
        result=run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/f'{label}-build.log').write_text(result.stdout+result.stderr);require(result)
        for suffix,package in [('apk/debug/app-debug.apk','com.hanxi.ghosterm'),
                ('apk/androidTest/debug/app-debug-androidTest.apk','com.hanxi.ghosterm.test')]:
            apk=ROOT/'android/app/build/outputs'/suffix
            require(run([*adb,'install','-r','-t',apk]))
            location=run([*adb,'shell','pm','path',package]);require(location)
            paths=[line.removeprefix('package:').strip() for line in location.stdout.splitlines() if line.startswith('package:')]
            if len(paths)!=1: raise RuntimeError('Unexpected installed APK shape')
            actual=run([*adb,'shell','sha256sum',paths[0]]);require(actual)
            if actual.stdout.split()[0]!=hashlib.sha256(apk.read_bytes()).hexdigest():
                raise RuntimeError('Installed APK differs from build')
    def discover(label,expect_phantom=False):
        # No class/package/exclusion filter: an explicit class list can hide this bug.
        result=run([*adb,'shell','am','instrument','-w','-r','-e','log','true',RUNNER],seconds=120)
        output=result.stdout+result.stderr;(work/f'{label}.log').write_text(output);require(result)
        try: return discovered(output)
        except Exception as error:
            if not (expect_phantom and isinstance(error,DiscoveryFailure) and
                    str(error)=='host-only probe was discovered without journalCrash'):
                print(output,flush=True)
            raise
    def control(label):
        result=run([*adb,'shell','am','instrument','-w','-r','-e','class',
            'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',RUNNER],seconds=60)
        output=result.stdout+result.stderr;(work/f'{label}.log').write_text(output);require(result)
        if 'OK (1 test)' not in output or 'FAILURES!!!' in output: raise RuntimeError(output)
    def restore():
        if MARK.encode() in source.read_bytes(): source.write_bytes(original)
        if hashlib.sha256(source.read_bytes()).digest()!=digest: raise RuntimeError('Discovery source restore mismatch')
    build('baseline');baseline=discover('baseline');control('baseline-control')
    (work/'discovered.json').write_text(json.dumps(baseline,ensure_ascii=False,indent=2)+'\n')
    if args.mutate:
        before='if(!enabled || !filter.shouldRun(description))'
        after='if(enabled && !filter.shouldRun(description))'
        text=original.decode()
        if text.count(before)!=1: raise RuntimeError('Missing/ambiguous discovery mutation anchor')
        try:
            source.write_text(text.replace(before,after)+'\n// '+MARK+'\n')
            build('mutated')
            try: discover('mutated-red',expect_phantom=True)
            except DiscoveryFailure as error:
                if str(error)!='host-only probe was discovered without journalCrash': raise
                print('TARGET RED: '+str(error),flush=True)
            else:
                print((work/'mutated-red.log').read_text(),flush=True)
                raise RuntimeError('Discovery exclusion mutation survived')
            control('mutated-control')
        finally: restore()
        build('restored')
        if discover('restored')!=baseline:
            print((work/'restored.log').read_text(),flush=True)
            raise RuntimeError('Restored discovery changed ordinary test identities')
        control('restored-control')
        print('ANDROID JOURNAL DISCOVERY MUTATION PASS: compiled target red / same APK config green / restored green / SHA256',flush=True)
    print(f'ANDROID JOURNAL DISCOVERY PASS: {len(baseline)} named tests discovered, not executed; evidence {work}',flush=True)


if __name__=='__main__':
    def interrupt(signum,frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT,interrupt);signal.signal(signal.SIGTERM,interrupt)
    main()
