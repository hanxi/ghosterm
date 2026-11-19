"""Owned no-window Android backend for test-plugin-mixed-native.py.

The parent prepares one real signed/development package and selected tools.
This backend owns its copied project, AVD, reverse mappings and Native UI test.
"""
import base64
from contextlib import ExitStack
import hashlib
import json
import os
from pathlib import Path
import pwd
import secrets
import shlex
import shutil
import socketserver
import struct
import threading

ROOT = Path(__file__).resolve().parents[2]
TEST = 'com.hanxi.ghosterm.ExtensionMixedConnectionTest#mixedExecutorsKeepResourcesAndAdmissionFailuresIndependent'
CONTROL = 'com.hanxi.ghosterm.ConfigIdentityTest#identitySourcesUseCoreOrderingAndOwnership'
RUNNER = 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(api, selected_routes, mutate, build, node, sdk, out, package, installed, policy, sdk_digest, node_version, run, env, load, inputs, source, revocation):
    helper = load('mixed_avd', 'test-plugin-codex-patch-android.py')
    keys = load('mixed_android_keys', 'test-plugin-ssh.py')
    streams = load('mixed_android_streams', 'test-plugin-ssh-stream.py')
    routes = load('mixed_android_routes', 'test-plugin-route.py')
    junit = load('mixed_android_junit', 'test-plugin-channel.py')
    devices, records, mutations = [], [], []
    # Boot before allocating a copied NDK/Gradle build: the selected AOSP image
    # checks its required free partition space at startup. Do not shrink it.
    with helper.headless(api) as serial, ExitStack() as stack:
        for folder in ('android', 'terminal/android'):
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'), dirs_exist_ok=True)
        for folder in ('core', 'vendor', 'build', 'assets', 'packages', 'scripts', 'patches', 'runtime-payload'):
            link = source / folder
            if link.is_symlink(): link.unlink()
            link.symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir(exist_ok=True)
        (source / '.mixed-fixture-project.json').write_text(json.dumps({'root': str(ROOT)}))
        for name, digest in inputs.items():
            if name.startswith(('android/', 'terminal/android/')) and sha(source / name) != digest:
                raise RuntimeError('copied Android source mismatch: ' + name)
        env.update(JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'), GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'),
            JAVA_TOOL_OPTIONS='-Djava.io.tmpdir=' + str(out))
        gradle = [source / 'android/gradlew', '-p', source / 'android', '--offline', '--no-daemon', '--max-workers=2',
            '-Dorg.gradle.parallel=false', '-PghostermPluginJs=true']
        if revocation:
            gradle += ['-PghostermPluginChannel=fixture', '-PghostermPluginChannelRoot=' + revocation['root'],
                       '-PghostermPluginChannelUrl=https://localhost.invalid/', '-PghostermChannelProcessFixture=true']
            if mutate:
                # Compare complete Kotlin outputs across rounds. Incremental
                # recompiles can reorder metadata in otherwise unchanged classes.
                gradle += ['-Pkotlin.incremental=false']
        app = source / 'android/app/build/outputs/apk'
        apks = [app / 'debug/app-debug.apk', app / 'androidTest/debug/app-debug-androidTest.apk']
        adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]

        def compile_apks(label):
            run(label, [*gradle, ':app:assembleDebug', ':app:assembleDebugAndroidTest'], seconds=600)
            return [sha(p) for p in apks]

        def install(label):
            for i, path in enumerate(apks):
                run(label + '-install-' + str(i), [*adb, 'install', '-r', '-t', path])
            for i, package_id in enumerate(('com.hanxi.ghosterm', 'com.hanxi.ghosterm.test')):
                remote = run(label + '-path-' + str(i), [*adb, 'shell', 'pm', 'path', package_id])[0].decode().strip()
                if not remote.startswith('package:/data/app/') or '\n' in remote:
                    raise RuntimeError('unexpected installed APK path')
                digest = run(label + '-sha-' + str(i), [*adb, 'shell', 'sha256sum', remote[len('package:'):]])[0].decode().split()[0]
                if digest != sha(apks[i]):
                    raise RuntimeError('installed APK bytes differ')

        def test(label, target=TEST, red=False, expected_assertion=None, extra=()):
            _, text = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', target, *extra, RUNNER], seconds=300)
            if red:
                if not junit.normal_junit_assertion(text) or text.count('INSTRUMENTATION_STATUS_CODE: -2\n') != 1 or any(
                    mark in text for mark in ('Process crashed', 'AppNotIdleException', 'TimeoutException', 'INSTRUMENTATION_FAILED', 'AddressSanitizer', 'runtime error:')):
                    raise RuntimeError(label + ': expected one compiled ordinary assertion\n' + text[-10000:])
                if expected_assertion:
                    if expected_assertion not in text:
                        raise RuntimeError(label + ': expected revocation assertion\n' + text[-10000:])
                    return
                publication_failed = any(mark in text for mark in (
                    'publish:failed:EXPIRED', 'publish:unknown:RESOURCE_RESULT_UNKNOWN'))
                rpc_cancelled = 'CANCELLED' in text and 'state=UNKNOWN' in text
                if not any(f'RPC resource/{slot}:' in text for slot in range(8)) or 'stage=initial' not in text or not (
                    publication_failed or rpc_cancelled
                ):
                    raise RuntimeError(label + ': assertion did not identify the resource-progress regression\n' + text[-10000:])
            elif 'OK (1 test)' not in text or 'INSTRUMENTATION_STATUS: numtests=1\n' not in text or any(
                mark in text for mark in ('INSTRUMENTATION_FAILED', 'assumption failed:', 'INSTRUMENTATION_STATUS_CODE: -4')):
                raise RuntimeError(label + ': exact discovery/no-skip evidence missing\n' + text[-10000:])

        baseline = compile_apks('android-build')
        install('android-baseline')
        profile = dict(api=run('android-api', [*adb, 'shell', 'getprop', 'ro.build.version.sdk'])[0].decode().strip(),
            abi=run('android-abi', [*adb, 'shell', 'getprop', 'ro.product.cpu.abi'])[0].decode().strip(),
            pagesize=run('android-pagesize', [*adb, 'shell', 'getconf', 'PAGE_SIZE'])[0].decode().strip())
        if profile['api'] != api or profile['abi'] != 'arm64-v8a':
            raise RuntimeError('wrong owned device profile')
        devices.append(profile)
        private, public = streams.memory_identity()
        agent = stack.enter_context(keys.owned_agent(out, 'a'))
        host_keys = [keys.add_memory_key(agent, 'android-mixed-hop-' + str(i)) for i in range(2)]
        route_root = out / 'r'; route_root.mkdir()
        ports, _, _ = stack.enter_context(routes.chain(route_root, 2, public, host_keys, agent, 'echo'))
        package_files = {p.relative_to(package).as_posix(): base64.b64encode(p.read_bytes()).decode() for p in package.rglob('*') if p.is_file()}
        ephemeral = run('android-ephemeral', [*adb, 'shell', 'cat', '/proc/sys/net/ipv4/ip_local_port_range'])[0].decode().split()
        low, high = map(int, ephemeral)
        if not 1024 <= low <= high <= 65535:
            raise RuntimeError('invalid device ephemeral range')
        start, end = (10000, min(low, 32768)) if low > 10000 else (high + 1, 65536)
        if start >= end:
            raise RuntimeError('no listener range outside device ephemeral range')

        def actual(route, label, red=False, expected_assertion=None):
            if revocation:
                cleared, _ = run(label + '-reset', [*adb, 'shell', 'pm', 'clear', 'com.hanxi.ghosterm'])
                if cleared.strip() != b'Success':
                    raise RuntimeError('could not reset the owned revocation fixture')
            scratch = out / label; scratch.mkdir(); (scratch / 'stage').mkdir()
            remote_stderr = scratch / 'gterm.stderr'
            launcher = scratch / 'gterm'
            launcher.write_text('#!/bin/sh\nexec ' + shlex.quote(str(build / 'gterm')) + ' "$@" 2>>' +
                                shlex.quote(str(remote_stderr)) + '\n')
            launcher.chmod(0o700)
            for slot in range(8):
                folder = scratch / ('workspace-' + str(slot)); folder.mkdir()
                (folder / 'file.bin').write_bytes(bytes((i + slot * 17) % 251 for i in range(262144)))
            token, mappings = secrets.token_hex(24), []
            def reverse(host):
                for attempt in range(32):
                    device = start + secrets.randbelow(end - start)
                    # Explicit free-range choice and --no-rebind preserve all
                    # mappings belonging to other work on this owned AVD.
                    command = [*adb, 'reverse', '--no-rebind', f'tcp:{device}', f'tcp:{host}']
                    import subprocess
                    p = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=15)
                    if p.returncode:
                        if 'Address already in use' in p.stderr or 'cannot rebind existing socket' in p.stderr:
                            continue
                        raise RuntimeError('ADB reverse failed: ' + p.stdout + p.stderr)
                    mappings.append(device)
                    listing = run(label + '-reverse-' + str(len(mappings)), [*adb, 'reverse', '--list'])[0].decode()
                    if not any(line.split()[-2:] == [f'tcp:{device}', f'tcp:{host}'] for line in listing.splitlines()):
                        raise RuntimeError('actual reverse mapping differs')
                    return device
                raise RuntimeError('no free owned device listener')
            try:
                target_ports = list(ports)
                first = 0 if route == 'jump' else 1
                target_ports[first] = reverse(ports[first])
                payload = dict(route=route, scratch=str(scratch), temporary=str(scratch / 'stage'), package=str(package),
                    installed=str(installed / 'payload'), release=str(installed), policy=str(policy), policySHA256=sha(policy),
                    ports=target_ports, hostKeys=[key.decode() for key in host_keys], user=pwd.getpwuid(os.getuid()).pw_name,
                    pem=private.decode(), gterm=str(launcher), node=str(node), nodeVersion=node_version,
                    nodeSHA256=sha(node), sdk=str(sdk), sdkSHA256=sdk_digest,
                    packageFiles={} if revocation else package_files, revocation=revocation,
                    packageDigest=sha(package / 'integrity.json'))
                class Handler(socketserver.StreamRequestHandler):
                    def handle(self):
                        self.connection.settimeout(5)
                        if self.rfile.readline(128).rstrip(b'\n') != token.encode():
                            return
                        data = json.dumps(payload, separators=(',', ':')).encode()
                        self.wfile.write(struct.pack('>I', len(data)) + data)
                with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                    thread = threading.Thread(target=server.serve_forever); thread.start()
                    try:
                        fixture_port = reverse(server.server_address[1])
                        test(label, red=red, expected_assertion=expected_assertion,
                            extra=('-e', 'fixturePort', str(fixture_port), '-e', 'fixtureToken', token))
                    except RuntimeError as failure:
                        if remote_stderr.exists():
                            with remote_stderr.open('rb') as diagnostics:
                                diagnostics.seek(max(0, remote_stderr.stat().st_size - 8192))
                                tail = diagnostics.read(8192).decode(errors='replace')
                            raise RuntimeError(f'{failure}\nOwned gterm stderr (tail):\n{tail}') from failure
                        raise
                    finally:
                        server.shutdown(); thread.join(5)
                if thread.is_alive():
                    raise RuntimeError('fixture service did not stop')
                if not red:
                    if list((scratch / 'stage').iterdir()): raise RuntimeError('remote staging was not released')
                    records.append(dict(api=api, route=route, label=label, apks=[sha(p) for p in apks]))
            finally:
                for port in mappings:
                    run(label + '-unmap-' + str(port), [*adb, 'reverse', '--remove', f'tcp:{port}'])

        completed = []
        failures = []
        for route in selected_routes or ('direct', 'jump'):
            try:
                actual(route, 'android-' + route)
            except RuntimeError as failure:
                failures.append(f'{route}: {failure}')
            else:
                completed.append(route)
                print('PASS Android API' + api + ' mixed ' + route, flush=True)
        if failures:
            raise RuntimeError('Android mixed route failures:\n' + '\n'.join(failures))
        recipes = [
            ('android-consent-recheck', 'ExtensionController',
             b'decision == ExtensionSession.Decision.ALLOW && channelAdmission != null',
             b'decision == ExtensionSession.Decision.DENY && channelAdmission != null', 1,
             'revoked consent must not advance the Native Session'),
            ('android-stop-consent', 'ExtensionPackagesViewModel',
             b'activeRuntime == null -> requestSessionStop(owner, includePendingConsent = true)',
             b'activeRuntime == null -> requestSessionStop(owner, includePendingConsent = false)', 1,
             'revocation stop must request cancellation of pending consent'),
        ] if revocation else [
            ('android-parent-pump', 'ExtensionController', b'            parent?.multiplexer?.step()\n', b'', 2, None),
        ]
        restored = baseline
        for label, stem, anchor, replacement, occurrences, assertion in recipes if mutate else []:
            filename = f'android/app/src/main/java/com/hanxi/ghosterm/{stem}.kt'
            path = source / filename
            original = path.read_bytes()
            if original.count(anchor) != occurrences:
                raise RuntimeError('ambiguous copied Android mutation: ' + label)
            compiled = source / 'android/app/build/tmp/kotlin-classes/debug/com/hanxi/ghosterm'
            objects = list(compiled.glob(stem + '*.class'))
            before = {str(p): sha(p) for p in objects}
            if not before: raise RuntimeError('missing compiled Android worker classes')
            try:
                path.write_bytes(original.replace(anchor, replacement))
                compile_apks(label + '-build')
                if not any(p.is_file() and sha(p) != before[str(p)] for p in objects):
                    raise RuntimeError('mutated Kotlin worker did not recompile')
                install(label)
                actual('direct', label + '-red', red=True, expected_assertion=assertion)
                test(label + '-control', target=CONTROL)
            finally:
                path.write_bytes(original)
            restored = compile_apks(label + '-restored-build')
            after = {str(p): sha(p) for p in compiled.glob(stem + '*.class')}
            changed = [Path(name).name for name in sorted(before.keys() | after.keys())
                if before.get(name) != after.get(name)]
            (out / (label + '-restoration.json')).write_text(json.dumps({
                'sourceRestored': sha(path) == inputs[filename],
                'workerClassDifferences': changed,
                'baselineAPKs': baseline,
                'restoredAPKs': restored,
            }, indent=2) + '\n')
            if changed:
                raise RuntimeError('restored Kotlin worker classes differ from baseline: ' + ', '.join(changed))
            if sha(path) != inputs[filename]: raise RuntimeError('Android worker source restoration mismatch')
            install(label + '-restored')
            actual('direct', label + '-restored')
            mutations.append(label)
            print('PASS ' + label + ': compiled assertion / same APK Core / restore / classes / SHA256', flush=True)
    return dict(routes=completed, mutations=mutations, devices=devices, actual=records, baselineAPKs=baseline, finalAPKs=restored,
        artifacts={str(p): sha(p) for p in apks})
