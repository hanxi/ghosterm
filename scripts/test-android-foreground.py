#!/usr/bin/env python3
"""Real foreground-service tests with host-owned notification permission restoration."""
import argparse
from contextlib import contextmanager
import importlib.util
import os
from pathlib import Path
import re

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('android_helpers',ROOT/'scripts/mutation-check-android-native.py')
helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)

@contextmanager
def notification_access(adb, command):
    sdk=int(command([*adb,'shell','getprop','ro.build.version.sdk']).strip())
    def granted():
        output=command([*adb,'shell','dumpsys','package','com.hanxi.ghosterm'])
        values=re.findall(r'android\.permission\.POST_NOTIFICATIONS:\s+granted=(true|false)',output)
        if len(values)!=1: raise RuntimeError('Notification permission state is unknown')
        return values[0]=='true'
    change=sdk>=33 and not granted()
    try:
        if change:
            command([*adb,'shell','pm','grant','com.hanxi.ghosterm','android.permission.POST_NOTIFICATIONS'])
            if not granted(): raise RuntimeError('Notification permission was not granted')
        yield
    finally:
        # Runtime permission revocation can kill the app. It belongs after instrumentation.
        if change:
            command([*adb,'shell','pm','revoke','com.hanxi.ghosterm','android.permission.POST_NOTIFICATIONS'])
            if granted(): raise RuntimeError('Notification permission was not restored')

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--skip-build',action='store_true');args=parser.parse_args()
    serial=os.environ.get('ANDROID_SERIAL')
    if not serial:raise RuntimeError('Set ANDROID_SERIAL')
    adb=[ROOT/'build/android-toolchain/sdk/platform-tools/adb','-s',serial]
    work=ROOT/'tmp/android-foreground-tests';work.mkdir(exist_ok=True)
    def command(argv):
        result=helper.run(argv);helper.require(result);return result.stdout
    if not args.skip_build:
        result=helper.run(['bash',ROOT/'scripts/build-android.sh'],seconds=900)
        (work/'build.log').write_text(result.stdout+result.stderr);helper.require(result)
    for apk in ('apk/debug/app-debug.apk','apk/androidTest/debug/app-debug-androidTest.apk'):
        command([*adb,'install','-r','-t',ROOT/'android/app/build/outputs'/apk])
    with notification_access(adb,command):
        output=command([*adb,'shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.FileForegroundServiceTest',
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        (work/'device.log').write_text(output)
        if 'OK (8 tests)' not in output:
            print(output,flush=True);raise RuntimeError('Foreground assertions failed')
    print('ANDROID FOREGROUND PASS: 8 real service tests; notification permission restored',flush=True)

if __name__=='__main__':main()
