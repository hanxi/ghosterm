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
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
import uuid
import shlex

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


def execute(selected, environment, mutate, build_environment, signature_ui=False, archive=False, archive_ui=False, install=False, death=False, catalog=False, selection=False, selection_channel=False, removal=False, removal_hardlink=False, apk_directory=None, maintenance=False, unclassified=False, policy_review=False):
    for path in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
        apk = (ROOT / 'android/app/build/outputs/apk' if apk_directory is None else Path(apk_directory)) / path
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
    if archive_ui:
        classes += ',com.hanxi.ghosterm.PluginArchiveCaptureTest,com.hanxi.ghosterm.PluginArchiveDraftTest,com.hanxi.ghosterm.PluginReleaseDeclarationsTest,com.hanxi.ghosterm.PluginArchivePreviewTest,com.hanxi.ghosterm.PluginArchiveReviewUITest'
        count += 24
    if install:
        classes += ',com.hanxi.ghosterm.PluginReleaseStageTest,com.hanxi.ghosterm.PluginInstallFilesTest,com.hanxi.ghosterm.PluginInstallDraftTest,com.hanxi.ghosterm.PluginInstallControllerTest,com.hanxi.ghosterm.PluginInstallUITest'
        count += 37
    if catalog:
        classes += ',com.hanxi.ghosterm.PluginReleaseCatalogTest,com.hanxi.ghosterm.PluginVersionLeaseTest,com.hanxi.ghosterm.PluginInstalledVersionsModelTest,com.hanxi.ghosterm.PluginCatalogControllerTest,com.hanxi.ghosterm.PluginCatalogUITest'
        count += 31
    if selection:
        classes += ',com.hanxi.ghosterm.PluginVersionSelectionControllerTest,com.hanxi.ghosterm.PluginVersionSelectionModelTest,com.hanxi.ghosterm.PluginVersionSelectionUITest'
        count += 24
    if removal:
        classes += ',com.hanxi.ghosterm.PluginRemovalFilesTest,com.hanxi.ghosterm.PluginReleaseRemovalTest,com.hanxi.ghosterm.PluginVersionRemovalControllerTest,com.hanxi.ghosterm.PluginVersionRemovalModelTest,com.hanxi.ghosterm.PluginVersionRemovalUITest'
        count += 33
    if maintenance:
        classes += ',com.hanxi.ghosterm.PluginInstallJournalTest,com.hanxi.ghosterm.PluginStageRemovalTest,com.hanxi.ghosterm.PluginInstallMaintenanceModelTest,com.hanxi.ghosterm.PluginInstallMaintenanceControllerTest,com.hanxi.ghosterm.PluginInstallMaintenanceUITest'
        count += 26
    if unclassified:
        classes += ',com.hanxi.ghosterm.PluginUnclassifiedDirectoriesTest,com.hanxi.ghosterm.PluginUnclassifiedControllerTest,com.hanxi.ghosterm.PluginUnclassifiedModelTest,com.hanxi.ghosterm.PluginUnclassifiedUITest'
        count += 24
    if policy_review:
        classes += ',com.hanxi.ghosterm.PluginPolicyReviewTest,com.hanxi.ghosterm.PluginPolicyReviewControllerTest,com.hanxi.ghosterm.PluginPolicyReviewModelTest,com.hanxi.ghosterm.PluginPolicyReviewUITest'
        count += 26
    fixture_arguments = []
    if removal_hardlink:
        # Only --headless-api may request this fixture. su creates two links in
        # our fresh app-owned test tree; no SELinux setting or other data changes.
        home = run([*selected, 'shell', 'run-as', 'com.hanxi.ghosterm', 'pwd'], environment).strip()
        if home not in ['/data/user/0/com.hanxi.ghosterm', '/data/data/com.hanxi.ghosterm']:
            raise RuntimeError('Unexpected owned fixture namespace')
        directory = home + '/cache/removal-hardlink-' + str(uuid.uuid4())
        version = directory + '/' + 'a' * 64
        run([*selected, 'shell', 'run-as', 'com.hanxi.ghosterm', 'mkdir', '-p', version], environment)
        script = 'umask 077; printf content > ' + shlex.quote(version + '/one')
        run([*selected, 'shell', 'run-as', 'com.hanxi.ghosterm', 'sh', '-c', shlex.quote(script)], environment)
        run([*selected, 'shell', 'su', '0', 'ln', version + '/one', version + '/two'], environment)
        run([*selected, 'shell', 'run-as', 'com.hanxi.ghosterm', 'chmod', '400', version + '/one'], environment)
        run([*selected, 'shell', 'run-as', 'com.hanxi.ghosterm', 'chmod', '500', version], environment)
        run([*selected, 'shell', 'run-as', 'com.hanxi.ghosterm', 'chmod', '700', directory], environment)
        classes += ',com.hanxi.ghosterm.PluginRemovalHardlinkTest'; count += 1
        fixture_arguments = ['-e', 'removalHardlinkRoot', directory]
    output = run([*selected, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', classes, *fixture_arguments,
        'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], environment)
    receipt = ROOT / 'tmp/android-release-conformance' / (str(time.time_ns()) + '.log')
    receipt.parent.mkdir(parents=True, exist_ok=True); receipt.write_text(output)
    print('INSTRUMENTATION RECEIPT ' + str(receipt) + ' SHA256 ' + hashlib.sha256(receipt.read_bytes()).hexdigest(), flush=True)
    completed = len(re.findall(r'^INSTRUMENTATION_STATUS_CODE: 0\s*$', output, re.MULTILINE))
    if (('OK (' + str(count) + ' tests)') not in output or completed != count or
            'FAILURES' in output or 'INSTRUMENTATION_FAILED' in output):
        raise RuntimeError('Release conformance did not run the complete enabled combination:\n' + output)
    print('RELEASE AUTHENTICATION ' + str(count) + ' TESTS PASS', flush=True)
    if selection_channel:
        methods = ['anotherControllerSwitchPreservesSessionSnapshotAndRuntimeLease', 'sameControllerRegistryReplacementPreservesSessionSnapshotAndRuntimeLease']
        if removal: methods += ['developmentSessionSelectingSameManagedDigestPinsVersionUntilActualRuntimeReleaseAndRemoval', 'developmentRuntimeSelectingSameManagedDigestPinsVersionUntilActualRuntimeReleaseAndRemoval']
        if policy_review: methods += ['sameControllerPolicyReviewDisablePreservesRuntimeAndLease', 'anotherControllerPolicyReviewDisablePreservesRuntimeAndLease']
        for method in methods:
            output = run([sys.executable, '-B', ROOT / 'scripts/test-plugin-channel.py', '--rpc',
                '--android-client-runtime', '--android', selected[-1], '--case', 'allow2', '--android-test',
                'com.hanxi.ghosterm.PluginVersionSelectionChannelTest#' + method], environment, 300)
            proof = receipt.with_name(str(time.time_ns()) + '-' + method + '.log')
            proof.write_text(output)
            print('VERSION SESSION/RUNTIME SSH ' + method + ' PASS; RECEIPT ' + str(proof) + ' SHA256 ' + hashlib.sha256(proof.read_bytes()).hexdigest(), flush=True)
    if death:
        driver = subprocess.Popen([sys.executable, '-B', ROOT / 'scripts/test-android-install-death.py'],
            cwd=ROOT, env=dict(environment, ANDROID_SERIAL=str(selected[-1])), start_new_session=True)
        try: status = driver.wait(timeout=600)
        except BaseException:
            if driver.poll() is None:
                os.killpg(driver.pid, signal.SIGINT); driver.wait()
            raise
        if status: raise RuntimeError('Owned install death driver failed')
        if unclassified:
            driver = subprocess.Popen([sys.executable, '-B', ROOT / 'scripts/test-android-unclassified-death.py'],
                cwd=ROOT, env=dict(environment, ANDROID_SERIAL=str(selected[-1])), start_new_session=True)
            try: status = driver.wait(timeout=600)
            except BaseException:
                if driver.poll() is None: os.killpg(driver.pid, signal.SIGINT); driver.wait()
                raise
            if status: raise RuntimeError('Owned unknown-directory death driver failed')
    if mutate:
        mutation = ('scripts/mutation-check-android-policy.py' if policy_review else
            'scripts/mutation-check-android-unclassified.py' if unclassified else
            'scripts/mutation-check-android-maintenance.py' if maintenance else
            'scripts/mutation-check-android-removal.py' if removal else
            'scripts/mutation-check-android-selection.py' if selection else
            'scripts/mutation-check-android-catalog.py' if catalog else
            'scripts/mutation-check-android-install.py' if install else
            'scripts/mutation-check-android-archive-ui.py' if archive_ui else
            'scripts/mutation-check-android-release-archive.py' if archive else
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
    parser.add_argument('--archive-ui', action='store_true', help='Include ZIP capture/draft/declarations/actual SAF UI; implies --archive --signature-ui')
    parser.add_argument('--install', action='store_true', help='Include atomic installation/stage/native/confirmation tests; implies --archive-ui')
    parser.add_argument('--catalog', action='store_true', help='Include read-only saved versions, selection storage and shared version leases; implies --install')
    parser.add_argument('--selection', action='store_true', help='Include actual version selection/disable/rollback, fresh policy/Core, admission and confirmation races; implies --catalog')
    parser.add_argument('--selection-channel', action='store_true', help='Also prove old signed Session/Runtime and directory leases over two-hop isolated SSH using built core/build/test_ext_rpc; implies --selection')
    parser.add_argument('--removal', action='store_true', help='Include explicit local version removal, intent observation/resume/cancel and actual Settings confirmation; implies --selection')
    parser.add_argument('--removal-hardlink', action='store_true', help='Also create actual app-owned hard-link fixture through su on our headless AVD only; implies --removal')
    parser.add_argument('--policy-review', action='store_true', help='Include current trusted-policy review and explicit atomic subset disable; implies --unclassified')
    parser.add_argument('--unclassified', action='store_true', help='Include unknown-directory full review, extra consent and format-three removal; implies --maintenance')
    parser.add_argument('--maintenance', action='store_true', help='Include persistent install ownership, explicit stage removal/metadata retirement and Settings UI; implies --removal')
    parser.add_argument('--death', action='store_true', help='Owned headless install checkpoints only; implies --install')
    args = parser.parse_args()
    if args.policy_review: args.unclassified = True
    if args.unclassified: args.maintenance = True
    if args.maintenance: args.removal = True
    if args.removal_hardlink:
        if args.android: parser.error('Privileged fixture creation requires an owned headless AVD')
        args.removal = True
    if args.removal: args.selection = True
    if args.selection_channel: args.selection = True
    if args.selection: args.catalog = True
    if args.catalog: args.install = True
    if args.death:
        if args.android: parser.error('Process-death scans require an owned headless AVD')
        args.install = True
    if args.install: args.archive_ui = True
    if args.archive_ui: args.archive = True; args.signature_ui = True
    (ROOT / 'tmp').mkdir(exist_ok=True)
    environment = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    # Explicit enabled build also includes the real Controller/Core snapshot test.
    print(run(['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true',
        ':app:assembleDebug', ':app:assembleDebugAndroidTest'], environment, 900)[-400:], flush=True)
    if args.android:
        execute(['adb', '-s', args.android], environment, args.mutate, environment, args.signature_ui, args.archive, args.archive_ui, args.install, args.death, args.catalog, args.selection, args.selection_channel, args.removal, args.removal_hardlink, maintenance=args.maintenance, unclassified=args.unclassified, policy_review=args.policy_review)
    else:
        with tempfile.TemporaryDirectory(prefix='release-android-headless-', dir=ROOT / 'tmp') as directory:
            with headless(args.headless_api, Path(directory), environment) as (selected, isolated):
                execute(selected, isolated, args.mutate, environment, args.signature_ui, args.archive, args.archive_ui, args.install, args.death, args.catalog, args.selection, args.selection_channel, args.removal, args.removal_hardlink, maintenance=args.maintenance, unclassified=args.unclassified, policy_review=args.policy_review)
        print('OWNED AVD FILES REMOVED', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
