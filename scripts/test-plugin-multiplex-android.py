#!/usr/bin/env python3
"""Eight production Android instances on owned no-window AVD/loopback SSH.

Only an isolated project, explicit gterm and in-memory fixture keys are used.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import secrets
import shutil
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import threading
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--lua-archive', type=Path, required=True, help='Pinned Lua 5.5.1 source archive')
    parser.add_argument('--work', type=Path, required=True)
    parser.add_argument('--api', choices=['29', '36'], default='36')
    parser.add_argument('--route', choices=['direct', 'jump'], action='append')
    parser.add_argument('--baseline-only', action='store_true', help='rerun the 25 existing regressions on the same compiled APK without route cases')
    args = parser.parse_args()
    build, work = args.build.resolve(strict=True), args.work.resolve(strict=True)
    lua_archive = args.lua_archive.resolve(strict=True)
    assert work.is_relative_to(ROOT / 'tmp') and work.is_dir()
    source, logs = work / 'project', work / 'logs'; logs.mkdir(exist_ok=True)
    if not source.exists():
        for folder in ['android', 'terminal/android']:
            shutil.copytree(ROOT / folder, source / folder, ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
        for folder in ['core', 'vendor', 'build', 'assets', 'patches', 'runtime-payload', 'scripts']:
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        (source / 'tmp').mkdir()
    inputs = {str(path.relative_to(source)): sha(path) for folder in ['android', 'terminal/android']
        for path in (source / folder).rglob('*') if path.is_file() and not any(
            part in ['build', '.gradle', '.kotlin', '.cxx'] for part in path.relative_to(source / folder).parts) and path.name != 'local.properties'}
    # Keep this test's daemon independent of other conversations/devices.
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0)); adb_port = listener.getsockname()[1]
    os.environ['ADB_SERVER_SOCKET'] = 'tcp:' + str(adb_port)
    os.environ['ANDROID_ADB_SERVER_PORT'] = str(adb_port)
    env = dict(os.environ, TMPDIR=str(work), TMP=str(work), TEMP=str(work),
        JAVA_TOOL_OPTIONS='-Djava.io.tmpdir=' + str(work),
        JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
        ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'),
        GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))

    def run(label, command, timeout=300):
        with (logs / (label + '.log')).open('w') as output:
            child = subprocess.Popen(list(map(str, command)), cwd=ROOT, env=env, stdout=output,
                stderr=subprocess.STDOUT, start_new_session=True)
            try: child.wait(timeout=timeout)
            except BaseException:
                if child.poll() is None: os.killpg(child.pid, signal.SIGTERM)
                try: child.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    if child.poll() is None: os.killpg(child.pid, signal.SIGKILL)
                    child.wait(timeout=5)
                raise
        text = (logs / (label + '.log')).read_text()
        assert child.returncode == 0, label + ': see ' + str(logs / (label + '.log'))
        return text

    package = work / 'table-package'
    if not package.exists():
        run('package', ['python3', '-B', ROOT / 'scripts/gterm-plugin', 'build',
            ROOT / 'plugins/official/table', '--out', package])
    helper = load('mux_avd', 'scripts/test-plugin-release-android.py')
    keys = load('mux_keys', 'scripts/test-plugin-ssh.py')
    streams = load('mux_streams', 'scripts/test-plugin-ssh-stream.py')
    routes = load('mux_routes', 'scripts/test-plugin-route.py')
    private, public = streams.memory_identity()
    artifacts = {}
    daemon = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb']
    run('adb-start', [*daemon, 'start-server'])
    try:
        device_work = Path(tempfile.mkdtemp(prefix='avd-', dir=work))
        with helper.headless(int(args.api), device_work, env) as (adb, _):
            # Boot before the copied build uses disk space needed by userdata.
            run('build', [source / 'android/gradlew', '-p', source / 'android', '--offline', '-PghostermPluginLua=true',
                f'-PghostermLuaArchive={lua_archive}',
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 600)
            for i, relative in enumerate(['android/app/build/outputs/apk/debug/app-debug.apk', 'android/app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk']):
                apk = source / relative; artifacts[relative] = sha(apk)
                run('install-' + str(i), [*adb, 'install', '-r', '-t', apk])
                pkg = 'com.hanxi.ghosterm' + ('.test' if i else '')
                installed = run('path-' + str(i), [*adb, 'shell', 'pm', 'path', pkg]).strip()
                assert installed.startswith('package:/data/app/') and '\n' not in installed
                actual = run('apk-sha-' + str(i), [*adb, 'shell', 'sha256sum', installed[len('package:'):]]).split()[0]
                assert actual == artifacts[relative]
            profile = {name: run(name, [*adb, 'shell', *command]).strip() for name, command in {
                'api': ['getprop', 'ro.build.version.sdk'], 'abi': ['getprop', 'ro.product.cpu.abi'], 'pagesize': ['getconf', 'PAGE_SIZE']}.items()}
            assert profile['api'] == args.api and profile['abi'] == 'arm64-v8a'
            with keys.owned_agent(work, 'agent') as agent:
                host_keys = [keys.add_memory_key(agent, 'mux-hop-' + str(i)) for i in range(2)]
                route_root = Path(tempfile.mkdtemp(prefix='r-', dir=work))
                with routes.chain(route_root, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                    for route in ([] if args.baseline_only else (args.route or ['direct', 'jump'])):
                        temporary = Path(tempfile.mkdtemp(prefix=route + '-temporary-', dir=work))
                        payload = dict(route=route, ports=list(ports), hostKeys=[p.decode() for p in host_keys],
                            user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode(), package=str(package),
                            packageFiles={p.relative_to(package).as_posix(): base64.b64encode(p.read_bytes()).decode() for p in package.rglob('*') if p.is_file()},
                            gterm=str(build / 'gterm'), temporary=str(temporary))
                        mappings = []
                        # adb 37's tcp:0 alias can abort the daemon when two
                        # reverse listeners coexist; reserve explicit device ports.
                        low, high = map(int, run(route + '-ephemeral', [*adb, 'shell', 'cat', '/proc/sys/net/ipv4/ip_local_port_range']).split())
                        start, end = (10000, min(low, 32768)) if low > 10000 else (high + 1, 65536)
                        def reverse(host):
                            for device in range(start, end):
                                command = [*adb, 'reverse', '--no-rebind', 'tcp:' + str(device), 'tcp:' + str(host)]
                                result = subprocess.run(list(map(str, command)), env=env, capture_output=True, text=True, timeout=10)
                                if result.returncode:
                                    if 'Address already in use' in result.stderr or 'cannot rebind existing socket' in result.stderr: continue
                                    raise RuntimeError('Owned reverse mapping failed: ' + result.stderr)
                                mappings.append(device)
                                listing = run(route + '-reverse-' + str(device), [*adb, 'reverse', '--list'])
                                assert any(line.split()[-2:] == ['tcp:' + str(device), 'tcp:' + str(host)] for line in listing.splitlines())
                                return device
                            raise RuntimeError('No unused explicit device listener')
                        token = secrets.token_hex(24)
                        class Handler(socketserver.StreamRequestHandler):
                            def handle(self):
                                self.connection.settimeout(5)
                                if self.rfile.readline(128).rstrip(b'\n') != token.encode(): return
                                data = json.dumps(payload, separators=(',', ':')).encode()
                                self.wfile.write(struct.pack('>I', len(data)) + data)
                        try:
                            with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                                first = 0 if route == 'jump' else 1; payload['ports'][first] = reverse(ports[first])
                                fixture_port = reverse(server.server_address[1])
                                thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
                                try:
                                    transcript = run(route, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                                        'com.hanxi.ghosterm.ExtensionConnectionTest#eightInstancesShareSSHAndCloseIndependently',
                                        '-e', 'fixturePort', fixture_port, '-e', 'fixtureToken', token,
                                        'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                                    assert 'OK (1 test)' in transcript and 'INSTRUMENTATION_STATUS: numtests=1\n' in transcript and not any(
                                        marker in transcript for marker in ['INSTRUMENTATION_FAILED', 'INSTRUMENTATION_STATUS_CODE: -4', 'FAILURES']), route
                                finally: server.shutdown(); thread.join()
                            assert list(temporary.iterdir()) == [], route + ': remote temporary resources remain'
                            print('PASS API' + args.api + ' ' + route + ': eight actual instances, separate consent/UI/RPC/close', flush=True)
                        finally:
                            primary_failed = sys.exc_info()[0] is not None
                            for device in mappings:
                                try: run(route + '-remove-' + str(device), [*adb, 'reverse', '--remove', 'tcp:' + str(device)])
                                except AssertionError:
                                    if not primary_failed: raise
            baseline = run('baseline', [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                'com.hanxi.ghosterm.PluginTableTest,com.hanxi.ghosterm.ClaudeLaunchTest,com.hanxi.ghosterm.ExtensionControllerTest',
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
            assert 'OK (25 tests)' in baseline and 'INSTRUMENTATION_STATUS: numtests=25\n' in baseline and not any(
                marker in baseline for marker in ['INSTRUMENTATION_FAILED', 'INSTRUMENTATION_STATUS_CODE: -4', 'FAILURES'])
            print('PASS existing 25 model/launch/controller tests on the same APK', flush=True)
    finally:
        run('adb-stop', [*daemon, 'kill-server'])
    for name, digest in inputs.items():
        assert sha(source / name) == digest and sha(ROOT / name) == digest, name
    for path in logs.glob('*.log'): assert b'PRIVATE KEY-----' not in path.read_bytes()
    (work / 'receipt.json').write_text(json.dumps(dict(inputs_sha256=inputs, artifacts=artifacts, profile=profile,
        routes=[] if args.baseline_only else (args.route or ['direct', 'jump']), baseline_tests=25,
        logs_sha256={p.name: sha(p) for p in logs.glob('*.log')}, gterm_sha256=sha(build / 'gterm'),
        scope='owned offscreen Android fixed eight-instance direct/two-hop SSH; no full product/device/mutation matrix'), indent=2) + '\n')


if __name__ == '__main__':
    main()
