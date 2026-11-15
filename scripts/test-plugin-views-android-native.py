#!/usr/bin/env python3
"""Copied Android Native view registry, owned no-window AVDs and loopback SSH Executors.

No user AVD, account, endpoint, filesystem tree or foreground window is used.
Mutations require a compiled ordinary assertion, same-APK Core control and restore.
"""
import argparse
from contextlib import ExitStack
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import re
import secrets
import shutil
import signal
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
APP = 'android/app/src/main/java/com/hanxi/ghosterm/'
MODEL = APP + 'PluginViews.kt'
VM = APP + 'ExtensionPackagesViewModel.kt'
BOOT = APP + 'ExtensionRelayBootstrap.kt'
UI = APP + 'PluginViewsContent.kt'
CONTROL = 'com.hanxi.ghosterm.ConfigIdentityTest#identitySourcesUseCoreOrderingAndOwnership'
RUNNER = 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'
TESTS = 'com.hanxi.ghosterm.PluginViewsTest,com.hanxi.ghosterm.PluginViewsUiTest'
ACTUAL = 'com.hanxi.ghosterm.PluginViewsSSHAppTest#registeredViewsThroughViewModelAndNativeControls'


CASES = [
    ('instance', MODEL, 'snapshot.instance == instance && snapshot.epoch == epoch', 'snapshot.instance.isNotEmpty() && snapshot.epoch == epoch', 'wrongInstanceClearsViews'),
    ('epoch', MODEL, 'snapshot.epoch == epoch', 'snapshot.epoch.isNotEmpty()', 'wrongEpochClearsViews'),
    ('same-revision', MODEL, 'require(snapshot == it)', 'require(true)', 'sameRevisionCannotChangeBindings'),
    ('resurrection', MODEL, 'require(r.counter > highWater)', 'require(true)', 'removedHandleCannotReturnAtNewRevision'),
    ('binding', MODEL, 'methods.containsAll(r.methods)', 'true', 'unknownMethodBindingRejected'),
    ('remove', MODEL, 'filter { it.handle !in handles }', 'filter { false && it.handle !in handles }', 'reopenSameViewIDUsesFreshModel'),
    ('task-history', MODEL, 'history = history', 'history = null', 'taskReopenKeepsUnknownCancellationIdentity'),
    ('descriptor-id', APP + 'PluginTable.kt', 'expectedViewID == null || expectedViewID == description.viewID', 'true', 'descriptorsMustMatchRegisteredViewID'),
    ('native-enable', APP + 'ExtensionController.kt', 'try { active.enableViews(viewKinds) }', 'try { Unit }', None),
    ('native-owner', APP + 'ExtensionController.kt', 'runtimeInstanceID == instanceID && runtimeViewScope != null', 'runtimeViewScope != null', None),
    ('native-render', APP + 'SettingsContent.kt', 'extensions.registeredViews?.let { PluginViewsContent(it) }', 'extensions.registeredViews?.let { _ -> Unit }', None),
    ('native-close', UI, 'onClick = { model.closeView(item.handle) }', 'onClick = { Unit }', 'UI#actualCloseKeepsOtherTableAndReopenGetsFreshHandle'),
    ('native-restart', APP + 'PluginTaskContent.kt', 'LaunchedEffect(model, model.available)', 'LaunchedEffect(model)', 'UI#unconfirmedCloseRetainsWidgetAndRestartsPolling'),
]


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True, help='JS/SSH-enabled host gterm build')
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--receipt', type=Path, required=True)
    parser.add_argument('--api', action='append', choices=('29', '36'))
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--case', action='append', help='select mutation groups (baseline and SSH still run)')
    parser.add_argument('--attach', action='store_true', help='connect an owned one-shot Portable foreground IPC instance')
    args = parser.parse_args()
    build, package, receipt = args.build.resolve(strict=True), args.package.resolve(strict=True), args.receipt.resolve()
    for path in (build, package, receipt):
        if not path.is_relative_to(ROOT / 'tmp'): parser.error('build, package and receipt must use project tmp/')
    cache = (build / 'CMakeCache.txt').read_text()
    if any(option + ':BOOL=ON' not in cache for option in ('GHOSTERM_WITH_PLUGIN_JS', 'GHOSTERM_WITH_LIBSSH2')):
        parser.error('build explicit JS/SSH-on gterm first')
    inputs = {}
    for folder in ('android', 'terminal/android', 'core/src', 'core/include', 'packages/plugin-sdk', 'plugins/official/views'):
        for path in (ROOT / folder).rglob('*'):
            if path.is_file() and not any(part in ('build', '.gradle', '.kotlin', '.cxx') for part in path.relative_to(ROOT / folder).parts):
                if path.name != 'local.properties': inputs[str(path.relative_to(ROOT))] = sha(path)
    for name in ('scripts/test-plugin-views-android-native.py', 'scripts/test-plugin-codex-patch-android.py',
                 'scripts/test-plugin-task.py', 'scripts/test-plugin-route.py', 'scripts/test-plugin-ssh.py', 'scripts/test-plugin-ssh-stream.py',
                 'scripts/test-plugin-channel.py', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_ANDROID_VIEWS_MUTATION' in (ROOT / n).read_bytes() for n in inputs if n.endswith('.kt')):
        raise RuntimeError('leftover Native views mutation')
    for label, filename, old, _new, _test in CASES:
        if (ROOT / filename).read_text().count(old) != 1: raise RuntimeError('Ambiguous root mutation: ' + label)
    inputs['scripts/fixtures/plugin-native-ipc.py'] = sha(ROOT / 'scripts/fixtures/plugin-native-ipc.py')
    ipc = load('native_ipc', 'scripts/fixtures/plugin-native-ipc.py')
    package_hashes = {p.relative_to(package).as_posix(): sha(p) for p in package.rglob('*') if p.is_file()}
    binaries = {'gterm': sha(build / 'gterm'), 'core': sha(build / 'core/libghosterm_core.a')}
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.with_suffix('.inputs.json').write_text(json.dumps(dict(accepted=False, inputs=inputs,
        package=package_hashes, binaries=binaries), indent=2) + '\n')
    helper = load('task_avd', 'scripts/test-plugin-codex-patch-android.py')
    junit = load('task_junit', 'scripts/test-plugin-channel.py')
    keys = load('task_keys', 'scripts/test-plugin-ssh.py')
    streams = load('task_streams', 'scripts/test-plugin-ssh-stream.py')
    routes = load('task_routes', 'scripts/test-plugin-route.py')
    logs = receipt.parent / (receipt.stem + '-logs'); logs.mkdir(parents=True, exist_ok=False)
    commands, groups, devices, records = [], [], [], []
    with tempfile.TemporaryDirectory(prefix='ad-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'
        for folder in ('android', 'terminal/android'):
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
        for folder in ('core', 'vendor', 'build', 'assets', 'packages', 'scripts', 'patches', 'runtime-payload'):
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1', JAVA_TOOL_OPTIONS='-Djava.io.tmpdir=' + str(ROOT / 'tmp') + ' -Djava.awt.headless=true',
            JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'),
            GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))

        def run(label, command, seconds=180):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require android-views; shift; exec "$gt_timeout_bin" --foreground "$@"',
                       'android-views', ROOT, seconds, *command]
            with (logs / (label + '.log')).open('w') as output:
                child = subprocess.Popen(list(map(str, bounded)), cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                try: child.wait(timeout=seconds + 5)
                except BaseException:
                    if child.poll() is None:
                        try: os.killpg(child.pid, signal.SIGTERM)
                        except ProcessLookupError: pass
                    try: child.wait(timeout=8)
                    except subprocess.TimeoutExpired:
                        if child.poll() is None:
                            try: os.killpg(child.pid, signal.SIGKILL)
                            except ProcessLookupError: pass
                        child.wait(timeout=3)
                    raise
            text = (logs / (label + '.log')).read_text()
            commands.append(dict(label=label, returncode=child.returncode))
            if child.returncode: raise RuntimeError(label + '\n' + text[-10000:])
            return text

        app = source / 'android/app/build/outputs/apk'
        apks = [app / 'debug/app-debug.apk', app / 'androidTest/debug/app-debug-androidTest.apk']
        def compile_apks(label):
            run(label, [source / 'android/gradlew', '-p', source / 'android', '--offline', '--no-daemon', '--max-workers=2',
                '-Dorg.gradle.parallel=false', '-PghostermPluginJs=true', ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 600)
            return [sha(p) for p in apks]
        baseline = compile_apks('build-baseline')

        scenarios = {'views': 1000000}
        private, public = streams.memory_identity()
        with keys.owned_agent(work, 'agent') as agent:
            host_keys = [keys.add_memory_key(agent, 'android-views-' + str(i)) for i in range(2)]
            route_root = work / 'r'; route_root.mkdir()
            with routes.chain(route_root, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                for api_index, api in enumerate(args.api or ('29', '36')):
                    with helper.headless(api) as serial:
                        adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]
                        def install(label):
                            for index, path in enumerate(apks):
                                run(label + '-install-' + str(index), [*adb, 'install', '-r', '-t', path])
                            for index, package_id in enumerate(('com.hanxi.ghosterm', 'com.hanxi.ghosterm.test')):
                                remote = run(label + '-path-' + str(index), [*adb, 'shell', 'pm', 'path', package_id]).strip()
                                if not remote.startswith('package:/data/app/') or '\n' in remote: raise RuntimeError('Unexpected installed APK path')
                                actual = run(label + '-sha-' + str(index), [*adb, 'shell', 'sha256sum', remote[len('package:'):]]).split()[0]
                                if actual != sha(apks[index]): raise RuntimeError('Installed APK mismatch')
                        def test(label, selected=TESTS, count=27, red=False, extra=()):
                            text = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selected,
                                              *extra, RUNNER])
                            if red:
                                if not junit.normal_junit_assertion(text) or text.count('INSTRUMENTATION_STATUS_CODE: -2\n') != 1 or any(
                                    mark in text for mark in ('Process crashed', 'AppNotIdleException', 'TimeoutException', 'INSTRUMENTATION_FAILED', 'AddressSanitizer', 'runtime error:')):
                                    raise RuntimeError(label + ': expected one ordinary assertion\n' + text)
                            elif f'OK ({count} test' not in text or f'INSTRUMENTATION_STATUS: numtests={count}\n' not in text or any(
                                mark in text for mark in ('INSTRUMENTATION_FAILED', 'assumption failed:', 'INSTRUMENTATION_STATUS_CODE: -4')):
                                raise RuntimeError(label + ': discovery/no-skip mismatch\n' + text)
                            return text
                        install('api' + api)
                        actual_api = run('api' + api + '-profile', [*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip()
                        abi = run('api' + api + '-abi', [*adb, 'shell', 'getprop', 'ro.product.cpu.abi']).strip()
                        page = run('api' + api + '-pagesize', [*adb, 'shell', 'getconf', 'PAGE_SIZE']).strip()
                        if actual_api != api or abi != 'arm64-v8a': raise RuntimeError('Wrong owned device profile')
                        test('api' + api + '-baseline')
                        test('api' + api + '-legacy-table', 'com.hanxi.ghosterm.PluginTableTest,com.hanxi.ghosterm.PluginTableUiTest', 23)
                        test('api' + api + '-legacy-diff', 'com.hanxi.ghosterm.PluginDiffTest,com.hanxi.ghosterm.PluginDiffUiTest', 27)
                        test('api' + api + '-legacy-task', 'com.hanxi.ghosterm.PluginTaskTest,com.hanxi.ghosterm.PluginTaskUiTest', 33)
                        print('PASS API' + api + ': 27 Native registry/model/UI tests', flush=True)

                        def actual(label, route, scenario, red=False):
                            limit = scenarios[scenario]
                            token = secrets.token_hex(24); mappings = []
                            payload = dict(route=route, limit=limit, scenario=scenario, package=str(package), gterm=str(build / 'gterm'), ports=list(ports),
                                hostKeys=[p.decode() for p in host_keys], user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode(),
                                packageDigest=sha(package / 'integrity.json'), packageFiles={p.relative_to(package).as_posix():
                                    base64.b64encode(p.read_bytes()).decode('ascii') for p in package.rglob('*') if p.is_file()})
                            ephemeral = run(label + '-ephemeral', [*adb, 'shell', 'cat', '/proc/sys/net/ipv4/ip_local_port_range']).split()
                            if len(ephemeral) != 2 or not all(v.isdigit() for v in ephemeral): raise RuntimeError('Device ephemeral range missing')
                            low, high = map(int, ephemeral)
                            if not 1024 <= low <= high <= 65535: raise RuntimeError('Invalid ephemeral range')
                            start, end = (10000, min(low, 32768)) if low > 10000 else (high + 1, 65536)
                            if start >= end: raise RuntimeError('No listener range outside ephemeral range')
                            def reverse(host):
                                for attempt in range(32):
                                    device = start + secrets.randbelow(end - start)
                                    p = subprocess.run([*map(str, adb), 'reverse', '--no-rebind', f'tcp:{device}', f'tcp:{host}'],
                                        env=env, capture_output=True, text=True, timeout=15)
                                    if p.returncode:
                                        if 'Address already in use' in p.stderr or 'cannot rebind existing socket' in p.stderr: continue
                                        raise RuntimeError('ADB reverse failed: ' + p.stdout + p.stderr)
                                    mappings.append(device)
                                    listing = run(label + '-reverse-' + str(len(mappings)), [*adb, 'reverse', '--list'])
                                    if not any(line.split()[-2:] == [f'tcp:{device}', f'tcp:{host}'] for line in listing.splitlines()):
                                        raise RuntimeError('Actual reverse mapping views')
                                    return device
                                raise RuntimeError('No free owned device listener')
                            with ExitStack() as attached:
                                frontend = attached.enter_context(ipc.foreground(build / 'gterm', package, 'views', [], env)) if args.attach else None
                                if frontend: payload.update(ipcDirectory=str(frontend.endpoint), ipcInstance=frontend.instance)
                                class Handler(socketserver.StreamRequestHandler):
                                    def handle(self):
                                        self.connection.settimeout(5)
                                        request = self.rfile.readline(128).rstrip(b'\n').split(b' ')
                                        if request[0] != token.encode() or len(request) > 2: return
                                        if len(request) == 2:
                                            if frontend is None: return
                                            frontend.control(request[1].decode('ascii'))
                                        data = json.dumps(dict(payload, ipcObservation=frontend.observation()) if frontend else payload, separators=(',', ':')).encode()
                                        self.wfile.write(struct.pack('>I', len(data)) + data)
                                try:
                                    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                                        first = 0 if route == 'jump' else 1
                                        payload['ports'][first] = reverse(ports[first])
                                        fixture_port = reverse(server.server_address[1])
                                        thread = threading.Thread(target=server.serve_forever); thread.start()
                                        try: test(label, ACTUAL, 1, red, ['-e', 'fixturePort', fixture_port, '-e', 'fixtureToken', token])
                                        finally: server.shutdown(); thread.join(5)
                                        if thread.is_alive(): raise RuntimeError('Owned fixture service did not stop')
                                finally:
                                    for device in mappings: run(label + '-remove-' + str(device), [*adb, 'reverse', '--remove', f'tcp:{device}'])
                                if not red: records.append(dict(api=api, route=route, scenario=scenario, limit=limit))
                                if frontend and not red: records[-1]['ipc'] = frontend.finish()

                        for route in ('direct', 'jump'):
                            for scenario in scenarios:
                                actual('api' + api + '-' + route + '-' + scenario, route, scenario)
                                print('PASS API' + api + ': actual Native ' + route + ' ' + scenario, flush=True)
                        cases = CASES
                        if args.case and set(args.case) - {c[0] for c in cases}: parser.error('unknown mutation group')
                        for label, filename, old, new, target in cases if args.mutate and api_index == 0 else []:
                            if args.case and label not in args.case: continue
                            path = source / filename; original = path.read_bytes()
                            if original.count(old.encode()) != 1: raise RuntimeError('Ambiguous mutation anchor: ' + label)
                            before = [sha(p) for p in apks]
                            selected = ('com.hanxi.ghosterm.PluginViewsUiTest#' + target[3:] if target.startswith('UI#') else
                                        'com.hanxi.ghosterm.PluginViewsTest#' + target) if target else None
                            try:
                                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_ANDROID_VIEWS_MUTATION\n')
                                mutated = compile_apks(label + '-build-red')
                                if before[0] == mutated[0]: raise RuntimeError('Actual production APK unchanged: ' + label)
                                install(label + '-red')
                                if selected: test(label + '-red', selected, 1, True)
                                else: actual(label + '-red', 'direct', 'views', True)
                                test(label + '-control', CONTROL, 1)
                            finally: path.write_bytes(original)
                            compile_apks(label + '-build-restored'); install(label + '-restored')
                            if selected: test(label + '-restored', selected, 1)
                            else: actual(label + '-restored', 'direct', 'views')
                            if sha(path) != inputs[filename]: raise RuntimeError('Source restoration mismatch')
                            groups.append(dict(api=api, name=label, mutatedAPK=mutated[0]))
                            print('PASS ' + label + ': ordinary assertion / same-APK Core / restored / APK / SHA256', flush=True)
                        test('api' + api + '-final')
                        devices.append(dict(api=api, abi=abi, pageSize=int(page), noWindow=True, apks=[sha(p) for p in apks]))
        if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('Root input changed')
        if package_hashes != {p.relative_to(package).as_posix(): sha(p) for p in package.rglob('*') if p.is_file()}: raise RuntimeError('Package changed')
        if binaries != {'gterm': sha(build / 'gterm'), 'core': sha(build / 'core/libghosterm_core.a')}: raise RuntimeError('Shared binary changed')
        for log in logs.glob('*.log'):
            if b'PRIVATE KEY-----' in log.read_bytes(): raise RuntimeError('Private fixture leaked to a log')
        receipt.write_text(json.dumps(dict(accepted=True, foregroundAttachment=args.attach, inputs=inputs, package=package_hashes, binaries=binaries,
            baselineAPKs=baseline, devices=devices, groups=groups, actual=records, commands=commands,
            scope='Native Android/arm64/owned no-window AVDs; macOS loopback SSH Executor; not physical devices, full client or release'), indent=2) + '\n')
    print('ANDROID NATIVE VIEWS PASS', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
