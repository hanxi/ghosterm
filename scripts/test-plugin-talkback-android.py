#!/usr/bin/env python3
"""Verify plugin form, table and task speech on owned Google APIs AVDs."""
import argparse
import hashlib
import os
from pathlib import Path
import runpy
import signal
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]
TALKBACK = 'com.google.android.marvin.talkback'
SERVICE = TALKBACK + '/.TalkBackService'
PREFERENCES = TALKBACK + '/com.android.talkback.TalkBackPreferencesActivity'
PREF_FILE = f'/data/user_de/0/{TALKBACK}/shared_prefs/{TALKBACK}_preferences.xml'
APP = 'com.hanxi.ghosterm'


def interrupted(signum, _frame):
    raise SystemExit(128 + signum)


def prepare(selected, environment, adb, api):
    # These operations require root and affect only the disposable,
    # serial-selected emulator created by the shared headless harness.
    adb('root')
    adb('wait-for-device')
    if adb('shell', 'id', '-u').strip() != '0':
        raise RuntimeError('Owned Google APIs AVD must support adbd root')
    adb('shell', 'input', 'keyevent', 'KEYCODE_WAKEUP')
    adb('shell', 'svc', 'power', 'stayon', 'true')
    adb('shell', 'wm', 'dismiss-keyguard')
    if api >= 33:
        adb('shell', 'pm', 'grant', TALKBACK, 'android.permission.POST_NOTIFICATIONS')

    def enable(on):
        adb('shell', 'settings', 'put', 'secure', 'enabled_accessibility_services', SERVICE if on else "''")
        adb('shell', 'settings', 'put', 'secure', 'accessibility_enabled', str(int(on)))

    enable(True)
    wait_bound(adb)
    adb('shell', 'am', 'start', '-W', '-n', PREFERENCES)
    for _ in range(20):
        data = adb('shell', f'test ! -s {PREF_FILE} || cat {PREF_FILE}').strip()
        if data:
            break
        time.sleep(.5)
    else:
        raise RuntimeError('TalkBack preferences were not written')
    enable(False)
    adb('shell', 'am', 'force-stop', TALKBACK)
    tree = ET.fromstring(data)
    for key, kind, value in [('pref_log_level', 'string', '2'), ('pref_tts_overlay', 'boolean', 'true')]:
        element = tree.find(f"./*[@name='{key}']")
        if element is None:
            element = ET.SubElement(tree, kind, {'name': key})
        if kind == 'boolean':
            element.set('value', value)
        else:
            element.text = value
    subprocess.run([*map(str, selected), 'shell', 'cat > ' + PREF_FILE],
                   input=ET.tostring(tree), env=environment, check=True, timeout=15)
    adb('shell', 'am', 'start', '-W', '-n', PREFERENCES)
    enable(True)
    wait_bound(adb)
    adb('shell', 'input', 'keyevent', 'KEYCODE_HOME')


def wait_bound(adb):
    for _ in range(20):
        status = adb('shell', 'dumpsys', 'accessibility')
        if any('Bound services' in line and 'label=TalkBack' in line and 'FEEDBACK_SPOKEN' in line
               for line in status.splitlines()):
            return
        time.sleep(.5)
    raise RuntimeError('TalkBack is not bound with spoken feedback')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--api', type=int, choices=(29, 36), default=36,
                        help='API 29 uses the TalkBack APK from the local API 36 image')
    parser.add_argument('--system-image', type=Path, help='installed system image for the owned target AVD')
    parser.add_argument('--expect-page-size', type=int, choices=(4096, 16384),
                        help='require the guest to use this page size before installing any APK')
    args = parser.parse_args()
    artifacts = ROOT / 'android/app/build/outputs/apk'
    apks = [artifacts / 'debug/app-debug.apk', artifacts / 'androidTest/debug/app-debug-androidTest.apk']
    missing = [str(apk) for apk in apks if not apk.is_file()]
    if missing:
        raise RuntimeError('Build Debug and DebugAndroidTest APKs first: ' + ', '.join(missing))
    runner = runpy.run_path(str(ROOT / 'scripts/test-plugin-release-android.py'))
    tmp = ROOT / 'tmp'
    tmp.mkdir(exist_ok=True)
    parent = Path(os.environ.get('TMPDIR', tmp)).resolve()
    owned_parent = parent.parent == tmp and parent.name.startswith('agent-') and parent.is_dir()
    with tempfile.TemporaryDirectory(prefix='talkback-' if owned_parent else 'agent-',
                                     dir=parent if owned_parent else tmp) as directory:
        env = dict(os.environ, TMPDIR=directory, TMP=directory, TEMP=directory,
                   GHOSTERM_AUTOMATION_OFFSCREEN='1')
        work = Path(directory)
        talkback_apk = None
        if args.api == 29:
            source = work / 'api36'
            source.mkdir()
            with runner['headless'](36, source, env) as (selected, device):
                result = runner['run']([*selected, 'shell', 'pm', 'path', TALKBACK], device)
                paths = [line.removeprefix('package:') for line in result.splitlines() if line.startswith('package:')]
                if len(paths) != 1:
                    raise RuntimeError('Expected a standalone TalkBack APK in the local API 36 image')
                talkback_apk = work / 'talkback.apk'
                runner['run']([*selected, 'pull', paths[0], talkback_apk], device)

        target = work / f'api{args.api}'
        target.mkdir()
        image = args.system_image.resolve(strict=True) if args.system_image else None
        with runner['headless'](args.api, target, env, image) as (selected, device):
            def adb(*args, seconds=30):
                return runner['run']([*selected, *args], device, seconds)

            api = adb('shell', 'getprop', 'ro.build.version.sdk').strip()
            page_size = adb('shell', 'getconf', 'PAGE_SIZE').strip()
            if api != str(args.api):
                raise RuntimeError(f'Expected API {args.api}, observed {api}')
            if args.expect_page_size is not None and page_size != str(args.expect_page_size):
                raise RuntimeError(f'Expected page size {args.expect_page_size}, observed {page_size}')
            print(f'Owned Android API {api}, page size {page_size}', flush=True)
            if talkback_apk is not None:
                adb('install', talkback_apk)
            prepare(selected, device, adb, args.api)
            for apk in apks:
                with apk.open('rb') as stream:
                    digest = hashlib.file_digest(stream, 'sha256').hexdigest()
                print(f'{apk.name} SHA256 {digest}', flush=True)
                adb('install', '-r', '-t', apk)
            adb('logcat', '-b', 'main', '-c')
            result = adb('shell', 'am', 'instrument', '-w', '-e', 'pluginTalkBack', 'true',
                         '-e', 'class', APP + '.PluginFormTalkBackTest',
                         APP + '.test/androidx.test.runner.AndroidJUnitRunner', seconds=120)
            print(result, flush=True)
            if 'OK (3 tests)' not in result:
                raise RuntimeError('Expected all three TalkBack instrumentation cases to pass')
            wait_bound(adb)
    if Path(directory).exists():
        raise RuntimeError('Owned AVD directory remains: ' + directory)
    print('Owned AVD directory cleaned', flush=True)


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGHUP, interrupted)
    main()
