#!/usr/bin/env python3
"""Copied Android Native diff, owned no-window AVDs and loopback SSH Executors.

No user AVD, account, endpoint, filesystem tree or foreground window is used.
Mutations require a compiled ordinary assertion, same-APK Core control and restore.
"""
import argparse
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
MODEL = APP + 'PluginDiff.kt'
VM = APP + 'ExtensionPackagesViewModel.kt'
BOOT = APP + 'ExtensionRelayBootstrap.kt'
UI = APP + 'PluginDiffContent.kt'
CONTROL = 'com.hanxi.ghosterm.ConfigIdentityTest#identitySourcesUseCoreOrderingAndOwnership'
RUNNER = 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'
TESTS = 'com.hanxi.ghosterm.PluginDiffTest,com.hanxi.ghosterm.PluginDiffUiTest'
ACTUAL = 'com.hanxi.ghosterm.PluginDiffSSHAppTest#officialDiffThroughViewModelConsentAndNativePaging'


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
    args = parser.parse_args()
    build, package, receipt = args.build.resolve(strict=True), args.package.resolve(strict=True), args.receipt.resolve()
    for path in (build, package, receipt):
        if not path.is_relative_to(ROOT / 'tmp'): parser.error('build, package and receipt must use project tmp/')
    cache = (build / 'CMakeCache.txt').read_text()
    if any(option + ':BOOL=ON' not in cache for option in ('GHOSTERM_WITH_PLUGIN_JS', 'GHOSTERM_WITH_LIBSSH2')):
        parser.error('build explicit JS/SSH-on gterm first')
    inputs = {}
    for folder in ('android', 'terminal/android', 'core/src', 'core/include', 'packages/plugin-sdk', 'plugins/official/diff'):
        for path in (ROOT / folder).rglob('*'):
            if path.is_file() and not any(part in ('build', '.gradle', '.kotlin', '.cxx') for part in path.relative_to(ROOT / folder).parts):
                if path.name != 'local.properties': inputs[str(path.relative_to(ROOT))] = sha(path)
    for name in ('scripts/test-plugin-diff-android-native.py', 'scripts/test-plugin-codex-patch-android.py',
                 'scripts/test-plugin-diff.py', 'scripts/test-plugin-route.py', 'scripts/test-plugin-ssh.py', 'scripts/test-plugin-ssh-stream.py',
                 'scripts/test-plugin-channel.py', 'scripts/lib/gt-timeout.sh'):
        inputs[name] = sha(ROOT / name)
    if any(b'GT_ANDROID_DIFF_MUTATION' in (ROOT / n).read_bytes() for n in inputs if n.endswith('.kt')):
        raise RuntimeError('leftover Native diff mutation')
    package_hashes = {p.relative_to(package).as_posix(): sha(p) for p in package.rglob('*') if p.is_file()}
    binaries = {'gterm': sha(build / 'gterm'), 'core': sha(build / 'core/libghosterm_core.a')}
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.with_suffix('.inputs.json').write_text(json.dumps(dict(accepted=False, inputs=inputs,
        package=package_hashes, binaries=binaries), indent=2) + '\n')
    helper = load('diff_avd', 'scripts/test-plugin-codex-patch-android.py')
    junit = load('diff_junit', 'scripts/test-plugin-channel.py')
    keys = load('diff_keys', 'scripts/test-plugin-ssh.py')
    streams = load('diff_streams', 'scripts/test-plugin-ssh-stream.py')
    routes = load('diff_routes', 'scripts/test-plugin-route.py')
    logs = receipt.parent / (receipt.stem + '-logs'); logs.mkdir(parents=True, exist_ok=False)
    commands, groups, devices, records = [], [], [], []
    with tempfile.TemporaryDirectory(prefix='ad-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'
        for folder in ('android', 'terminal/android'):
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
        for folder in ('core', 'vendor', 'build', 'assets', 'packages', 'scripts', 'patches', 'runtime-payload'):
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), JAVA_TOOL_OPTIONS='-Djava.io.tmpdir=' + str(ROOT / 'tmp'),
            JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'),
            GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))

        def run(label, command, seconds=180):
            bounded = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require android-diff; shift; exec "$gt_timeout_bin" --foreground "$@"',
                       'android-diff', ROOT, seconds, *command]
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

        scenarios = load('diff_scenarios', 'scripts/test-plugin-diff.py').SCENARIOS
        private, public = streams.memory_identity()
        with keys.owned_agent(work, 'agent') as agent:
            host_keys = [keys.add_memory_key(agent, 'android-diff-' + str(i)) for i in range(2)]
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
                        print('PASS API' + api + ': 27 Native model/codec/UI tests', flush=True)

                        def actual(label, route, scenario, red=False):
                            before, after, total = scenarios[scenario]
                            token = secrets.token_hex(24); mappings = []
                            payload = dict(route=route, total=total, before=before, after=after, package=str(package), gterm=str(build / 'gterm'), ports=list(ports),
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
                                        raise RuntimeError('Actual reverse mapping differs')
                                    return device
                                raise RuntimeError('No free owned device listener')
                            class Handler(socketserver.StreamRequestHandler):
                                def handle(self):
                                    self.connection.settimeout(5)
                                    if self.rfile.readline(128).rstrip(b'\n') != token.encode(): return
                                    data = json.dumps(payload, separators=(',', ':')).encode()
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
                            if not red: records.append(dict(api=api, route=route, scenario=scenario, total=total))

                        for route in ('direct', 'jump'):
                            for scenario in ('change', 'empty', 'unicode', 'equal'):
                                actual('api' + api + '-' + route + '-' + scenario, route, scenario)
                                print('PASS API' + api + ': actual Native ' + route + ' ' + scenario, flush=True)
                        scroll_header = '''Box(Modifier.fillMaxWidth().heightIn(max = 320.dp).horizontalScroll(rememberScrollState()).testTag("plugin-diff-rows")) {
                    Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {'''
                        combined_scroll = scroll_header.replace('.horizontalScroll(rememberScrollState())',
                            '.horizontalScroll(rememberScrollState()).verticalScroll(rememberScrollState())').replace(
                            'Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement', 'Column(verticalArrangement')
                        cases = [
                            ('view', MODEL, 'value["viewId"].id() == description.viewID', 'value["viewId"].id().isNotEmpty()', 'selectedPageRejectsDifferentViewRevisionOffsetAndTotal'),
                            ('revision', MODEL, 'value["revision"].revision() == description.revision', 'value["revision"].revision().isNotEmpty()', 'selectedPageRejectsDifferentViewRevisionOffsetAndTotal'),
                            ('page-complete', MODEL, 'rows.size == minOf(description.pageSize, description.total - offset)', 'true', 'selectedPageMustContainEveryExpectedRow'),
                            ('prefix-before', MODEL, 'value["beforeOffset"].integer(0..128) == prefix.before', 'value["beforeOffset"].integer(0..128) >= 0', 'selectedPageRequiresExactBeforeAndAfterPrefix'),
                            ('prefix-after', MODEL, 'value["afterOffset"].integer(0..128) == prefix.after', 'value["afterOffset"].integer(0..128) >= 0', 'selectedPageRequiresExactBeforeAndAfterPrefix'),
                            ('coordinates', MODEL, 'old == (if (kind == PluginDiffRow.Kind.INSERT) 0 else before)', 'true', 'rowCoordinatesKindsEndingsAndTextAreExact'),
                            ('final-newline', MODEL, 'ending != PluginDiffRow.Ending.NONE ||', 'true ||', 'noFinalNewlineIsOnlyValidOnFinalSourceLine'),
                            ('final-consumed', MODEL, 'offset + parsed.size != description.total ||', 'true ||', 'finalPageMustConsumeBothSources'),
                            ('late-reply', MODEL, 'generation === expected && available && currentCoroutineContext().isActive', 'currentCoroutineContext().isActive', 'latePageCannotReopenAnInvalidatedView'),
                            ('native-service', BOOT, 'request.clientCapabilities.forEach { add("--client-capability"); add(it) }', 'request.clientCapabilities.forEach { _ -> Unit }', 'nativeBootstrapDeclaresOnlyUniqueImplementedClientServices'),
                            ('native-next', UI, 'model.showPage(state.selectedOffset + (description?.pageSize ?: 3))', 'model.showPage(state.selectedOffset + 2 * (description?.pageSize ?: 3))', 'UI#realPagingButtonsShowNextAndPreviousExactTextAndEndings'),
                            ('native-scroll-axis', UI, scroll_header, combined_scroll, 'UI#scrollActionsKeepBothAxesIndependentlyAddressable'),
                            ('native-declaration', VM, 'if (state.info.phase == ExtensionRuntime.Phase.ACTIVE && diff == null &&', 'if (false && state.info.phase == ExtensionRuntime.Phase.ACTIVE && diff == null &&', None),
                            ('native-methods', VM, '(tableRequest.methods + PluginDiffModel.methods).distinct()', 'tableRequest.methods', None),
                            ('native-render', APP + 'SettingsContent.kt', 'extensions.diff?.let { PluginDiffContent(it) }', 'extensions.diff?.let { _ -> Unit }', None),
                        ]
                        if args.case and set(args.case) - {c[0] for c in cases}: parser.error('unknown mutation group')
                        for label, filename, old, new, target in cases if args.mutate and api_index == 0 else []:
                            if args.case and label not in args.case: continue
                            path = source / filename; original = path.read_bytes()
                            if original.count(old.encode()) != 1: raise RuntimeError('Ambiguous mutation anchor: ' + label)
                            before = [sha(p) for p in apks]
                            selected = ('com.hanxi.ghosterm.PluginDiffUiTest#' + target[3:] if target.startswith('UI#') else
                                        'com.hanxi.ghosterm.PluginDiffTest#' + target) if target else None
                            try:
                                path.write_bytes(original.replace(old.encode(), new.encode()) + b'\n// GT_ANDROID_DIFF_MUTATION\n')
                                mutated = compile_apks(label + '-build-red')
                                if before[0] == mutated[0]: raise RuntimeError('Actual production APK unchanged: ' + label)
                                install(label + '-red')
                                if selected: test(label + '-red', selected, 1, True)
                                else: actual(label + '-red', 'direct', 'change', True)
                                test(label + '-control', CONTROL, 1)
                            finally: path.write_bytes(original)
                            compile_apks(label + '-build-restored'); install(label + '-restored')
                            if selected: test(label + '-restored', selected, 1)
                            else: actual(label + '-restored', 'direct', 'change')
                            if sha(path) != inputs[filename]: raise RuntimeError('Source restoration mismatch')
                            groups.append(dict(api=api, name=label, mutatedAPK=mutated[0]))
                            print('PASS ' + label + ': ordinary assertion / same-APK Core / restored / APK / SHA256', flush=True)
                        test('api' + api + '-final')
                        devices.append(dict(api=api, abi=abi, pageSize=int(page), noWindow=True, apks=[sha(p) for p in apks]))
        if any(sha(ROOT / n) != h for n, h in inputs.items()): raise RuntimeError('Root input changed')
        if package_hashes != {p.relative_to(package).as_posix(): sha(p) for p in package.rglob('*') if p.is_file()}: raise RuntimeError('Package changed')
        if binaries != {'gterm': sha(build / 'gterm'), 'core': sha(build / 'core/libghosterm_core.a')}: raise RuntimeError('Shared binary changed')
        receipt.write_text(json.dumps(dict(accepted=True, inputs=inputs, package=package_hashes, binaries=binaries,
            baselineAPKs=baseline, devices=devices, groups=groups, actual=records, commands=commands,
            scope='Native Android/arm64/owned no-window AVDs; macOS loopback SSH Executor; not physical devices, full client or release'), indent=2) + '\n')
    print('ANDROID NATIVE DIFF PASS', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
