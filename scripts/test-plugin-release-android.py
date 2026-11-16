#!/usr/bin/env python3
"""Release authentication conformance; explicit device or owned headless AVD.

Uses existing locally installed SDK images. No download, window, user directory
or system /tmp execution. Public fixtures are already in the test APK.
"""
import argparse
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
TESTS = ','.join('com.hanxi.ghosterm.' + name for name in [
    'PluginReleaseSignatureTest', 'PluginReleaseControllerTest',
    'ExtensionControllerTest', 'ConfigRepositoryTest'])


def run(command, environment, seconds=180):
    # This bound applies to our own child command only; it never restarts an AVD.
    result = subprocess.run(list(map(str, command)), cwd=ROOT, env=environment,
        capture_output=True, text=True, timeout=seconds)
    if result.returncode: raise RuntimeError(str(command) + '\n' + result.stdout + result.stderr)
    return result.stdout + result.stderr


@contextmanager
def headless(api, work, environment):
    sdk = Path(environment.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk'))
    image = sdk / f'system-images/android-{api}/google_apis/arm64-v8a'
    if not image.is_dir(): raise RuntimeError('Required locally installed image missing: ' + str(image))
    for name in ['avds', 'user', 'emulator', 'discovery', 'crash']:
        (work / name).mkdir()
    avd = work / 'avds/release.avd'; avd.mkdir()
    (work / 'avds/release.ini').write_text(f'avd.ini.encoding=UTF-8\npath={avd}\ntarget=android-{api}\n')
    config = {'AvdId': 'release', 'avd.ini.encoding': 'UTF-8', 'image.sysdir.1': str(image),
        'abi.type': 'arm64-v8a', 'hw.cpu.arch': 'arm64', 'hw.cpu.ncore': '2', 'hw.ramSize': '2048',
        'hw.lcd.width': '1080', 'hw.lcd.height': '1920', 'hw.lcd.density': '420',
        'hw.keyboard': 'yes', 'hw.gpu.enabled': 'yes', 'hw.gpu.mode': 'swiftshader',
        'hw.audioInput': 'no', 'hw.audioOutput': 'no', 'disk.dataPartition.size': '2G',
        'showDeviceFrame': 'no', 'tag.id': 'google_apis', 'PlayStore.enabled': 'false'}
    (avd / 'config.ini').write_text(''.join(f'{k}={v}\n' for k, v in config.items()))
    environment = dict(environment, ANDROID_SDK_ROOT=str(sdk), ANDROID_HOME=str(sdk),
        ANDROID_USER_HOME=str(work / 'user'), ANDROID_AVD_HOME=str(work / 'avds'),
        ANDROID_EMULATOR_HOME=str(work / 'emulator'), ANDROID_TMP=str(work / 'emulator'),
        ANDROID_EMULATOR_DISCOVERY_DIR=str(work / 'discovery'),
        ANDROID_EMU_ENABLE_CRASH_REPORTING='0', ANDROID_EMU_CRASH_REPORTING_DATABASE=str(work / 'crash'))
    adb = sdk / 'platform-tools/adb'
    devices = run([adb, 'devices'], environment)
    port = None
    for candidate in range(5554, 5586, 2):
        if f'emulator-{candidate}' in devices: continue
        try:
            with socket.socket() as console, socket.socket() as transport:
                console.bind(('127.0.0.1', candidate)); transport.bind(('127.0.0.1', candidate + 1))
                port = candidate; break
        except OSError: continue
    if port is None: raise RuntimeError('No unused AVD console/ADB port pair')
    with (work / 'emulator.log').open('w+') as log:
        child = subprocess.Popen([sdk / 'emulator/emulator', '-avd', 'release', '-port', str(port),
            '-no-window', '-no-audio', '-no-boot-anim', '-no-snapshot', '-no-metrics',
            '-crash-report-mode', 'disabled', '-gpu', 'swiftshader', '-cores', '2', '-memory', '2048'],
            cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        selected = [adb, '-s', f'emulator-{port}']
        try:
            deadline = time.monotonic() + 120
            while True:
                if child.poll() is not None: raise RuntimeError('Owned AVD exited: ' + (work / 'emulator.log').read_text())
                status = subprocess.run([*map(str, selected), 'shell', 'getprop', 'sys.boot_completed'],
                    env=environment, capture_output=True, text=True, timeout=10)
                if status.returncode == 0 and status.stdout.strip() == '1': break
                if time.monotonic() > deadline: raise RuntimeError('Owned AVD boot timeout; same process retained until cleanup')
                time.sleep(.5)
            # boot_completed precedes first-boot package compilation. Wait for
            # observed guest CPU idle before measuring the unchanged validator
            # budget; never retry a failed assertion or raise the Core limit.
            def cpu_sample():
                fields = run([*selected, 'shell', 'cat', '/proc/stat'], environment).splitlines()[0].split()
                if fields[0] != 'cpu': raise RuntimeError('No guest CPU aggregate')
                ticks = list(map(int, fields[1:9]))
                return sum(ticks), ticks[3] + ticks[4]
            previous = cpu_sample(); stable = 0
            idle_deadline = time.monotonic() + 120
            while stable < 3:
                if child.poll() is not None: raise RuntimeError('Owned AVD exited during readiness')
                if time.monotonic() > idle_deadline: raise RuntimeError('Guest CPU did not settle before conformance')
                time.sleep(1)
                current = cpu_sample()
                total, idle = current[0] - previous[0], current[1] - previous[1]
                print('GUEST CPU IDLE ' + str(round(idle * 100 / total) if total > 0 else -1) + '%', flush=True)
                stable = stable + 1 if total > 0 and idle * 100 >= total * 60 else 0
                previous = current
            log.flush(); startup = (work / 'emulator.log').read_text()
            if 'Crashreporting disabled, not reporting crashes.' not in startup or 'Storing crashdata in: /tmp/' in startup:
                raise RuntimeError('AVD did not confirm project-local/disabled crash reporting:\n' + startup)
            # New emulators may leave discovery disabled with no gRPC server.
            # Absence is valid. If a record exists for this process group, its
            # location must belong to this owned workspace.
            default_discovery = Path.home() / 'Library/Caches/TemporaryItems/avd/running'
            for record in default_discovery.glob('pid_*.ini'):
                try:
                    pid = int(record.stem.split('_')[1])
                    if os.getpgid(pid) == child.pid:
                        raise RuntimeError('Owned AVD advertised outside project tmp: ' + str(record))
                except (ValueError, ProcessLookupError):
                    pass
            print(f'HEADLESS READY API {api}: no-window / crash disabled / ANDROID_TMP and discovery in project tmp', flush=True)
            yield selected, environment
        finally:
            if child.poll() is None:
                try: run([*selected, 'emu', 'kill'], environment, 10)
                except (subprocess.TimeoutExpired, RuntimeError): pass
                try: child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(child.pid, signal.SIGTERM)
                    try: child.wait(timeout=3)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL); child.wait()
            print('OWNED AVD EXITED ' + str(child.returncode), flush=True)


def execute(selected, environment, mutate, build_environment, signature_ui=False, archive=False):
    for path in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
        apk = ROOT / 'android/app/build/outputs/apk' / path
        print(path + ' SHA256 ' + hashlib.sha256(apk.read_bytes()).hexdigest(), flush=True)
        run([*selected, 'install', '-r', '-t', apk], environment)
    info = [run([*selected, 'shell', 'getprop', name], environment).strip() for name in ['ro.build.version.sdk', 'ro.product.cpu.abi']]
    page = run([*selected, 'shell', 'getconf', 'PAGE_SIZE'], environment).strip()
    print('DEVICE API/ABI/PAGE ' + '/'.join(info + [page]), flush=True)
    classes = TESTS + (',com.hanxi.ghosterm.PluginSignatureDocumentsTest,com.hanxi.ghosterm.PluginSignatureDraftTest,com.hanxi.ghosterm.PluginSignatureReviewUITest,com.hanxi.ghosterm.FileDescriptorFlagsTest' if signature_ui else '')
    count = 62 if signature_ui else 42
    if archive:
        classes += ',com.hanxi.ghosterm.PluginReleaseArchiveTest,com.hanxi.ghosterm.PluginReleaseArchiveControllerTest'
        count += 23
    output = run([*selected, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', classes,
        'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], environment)
    if ('OK (' + str(count) + ' tests)') not in output or 'FAILURES' in output or 'INSTRUMENTATION_FAILED' in output:
        raise RuntimeError('Release conformance did not run the complete enabled combination:\n' + output)
    print('RELEASE AUTHENTICATION ' + str(count) + ' TESTS PASS', flush=True)
    if mutate:
        mutation = ('scripts/mutation-check-android-release-archive.py' if archive else
            'scripts/mutation-check-android-signature-ui.py' if signature_ui else 'scripts/mutation-check-android-release-signature.py')
        driver = subprocess.Popen([sys.executable, '-B', ROOT / mutation],
            cwd=ROOT, env=dict(build_environment, ANDROID_SERIAL=str(selected[-1])), start_new_session=True)
        try:
            status = driver.wait(timeout=1800)
        except BaseException:
            if driver.poll() is None:
                # The driver's signal handler restores its marked source files.
                # Wait for that restoration before shutting down the owned AVD.
                os.killpg(driver.pid, signal.SIGINT)
                driver.wait()
            raise
        if status: raise RuntimeError('Device mutation driver failed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    choice = parser.add_mutually_exclusive_group(required=True)
    choice.add_argument('--headless-api', choices=[29, 36], type=int)
    choice.add_argument('--android', help='Explicitly authorized device serial')
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--signature-ui', action='store_true', help='Include documents, draft and actual SAF Settings tests')
    parser.add_argument('--archive', action='store_true', help='Include strict ZIP and signed Core preview conformance')
    args = parser.parse_args()
    (ROOT / 'tmp').mkdir(exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    # Explicit enabled build also includes the real Controller/Core snapshot test.
    print(run(['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true',
        ':app:assembleDebug', ':app:assembleDebugAndroidTest'], environment, 900)[-400:], flush=True)
    if args.android:
        execute(['adb', '-s', args.android], environment, args.mutate, environment, args.signature_ui, args.archive)
    else:
        with tempfile.TemporaryDirectory(prefix='release-android-headless-', dir=ROOT / 'tmp') as directory:
            with headless(args.headless_api, Path(directory), environment) as (selected, isolated):
                execute(selected, isolated, args.mutate, environment, args.signature_ui, args.archive)
        print('OWNED AVD FILES REMOVED', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
