#!/usr/bin/env python3
"""Native macOS/Android App -> owned loopback SSH -> actual Node/native Exec.

Keys exist only in the fixture service memory and ssh-agent. No accounts/models,
no foreground windows, no user endpoints; temporary executables stay in project tmp.
"""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import pwd
import secrets
import shlex
import shutil
import signal
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, ROOT / filename)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module); return module


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def alive(pid):
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False


def identity(pid):
    p = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=,command='], capture_output=True, text=True, timeout=5)
    return p.stdout.strip() if p.returncode == 0 else None


def execute(args, serial=None):
    build, node, sdk = args.build.resolve(strict=True), args.node.resolve(strict=True), args.sdk.resolve(strict=True)
    for p in (build / 'gterm', build / 'test_gterm_exec_peer', build / 'core/libghosterm_core.a'):
        if not p.is_file(): raise RuntimeError('Build the JS/SSH-enabled CLI fixture: ' + str(p))
    cache = (build / 'CMakeCache.txt').read_text()
    if 'GHOSTERM_WITH_LIBSSH2:BOOL=ON' not in cache or 'GHOSTERM_WITH_PLUGIN_JS:BOOL=ON' not in cache:
        raise RuntimeError('Explicit JS/SSH-enabled build required')
    hashes = {str(p.relative_to(ROOT)): sha(p) for directory in ['core/src', 'core/include', 'core/tests',
        'apple/GhostermApp/Sources', 'apple/GhostermApp/Tests', 'android/app/src', 'packages/plugin-sdk/node']
        for p in (ROOT / directory).rglob('*') if p.is_file()}
    binary_hashes = {str(p): sha(p) for p in [build / 'gterm', build / 'test_gterm_exec_peer',
        build / 'core/libghosterm_core.a', node]}
    for name in ['scripts/test-plugin-execution-ssh-app.py', 'scripts/fixtures/plugin-execution-app.mjs',
        'scripts/fixtures/plugin-execution-fault.py', 'scripts/fixtures/plugin-execution-supervisor.py',
        'scripts/test-plugin-route.py', 'scripts/test-plugin-ssh.py', 'scripts/test-plugin-ssh-stream.py',
        'scripts/test-plugin-codex-patch-android.py']:
        hashes[name] = sha(ROOT / name)
    binary_hashes.update({str(p): sha(p) for p in sdk.rglob('*') if p.is_file()})
    version = subprocess.check_output([node, '--version'], text=True).strip()[1:]
    catalog = hashlib.sha256()
    for p in sorted((p for p in sdk.rglob('*') if p.is_file()), key=lambda p: str(p.relative_to(sdk)).encode()):
        name = str(p.relative_to(sdk)).encode(); catalog.update(len(name).to_bytes(4, 'big') + p.stat().st_size.to_bytes(8, 'big') + name + sha(p).encode())
    sdk_digest = catalog.hexdigest()
    runtime_types = ('node', 'exec')
    cases = [f'{r}:{route}:{mode}' for r in runtime_types for route in ('direct', 'jump')
             for mode in ('allow', 'deny', 'no-approval', 'pending-cancel', 'pending-loss')]
    selected = args.case or cases
    if set(selected) - set(cases): raise RuntimeError('Unknown execution case')
    routes = load('execution_routes', 'scripts/test-plugin-route.py')
    keys = load('execution_keys', 'scripts/test-plugin-ssh.py')
    streams = load('execution_streams', 'scripts/test-plugin-ssh-stream.py')
    private, public = streams.memory_identity()
    fault_module = load('execution_fault', 'scripts/fixtures/plugin-execution-fault.py')
    records = []
    with tempfile.TemporaryDirectory(prefix='xs-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); logs = work / 'logs'; logs.mkdir()
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
            GHOSTERM_CORE_TEST_PLUGIN_JS='1', GHOSTERM_AUTOMATION_OFFSCREEN='1',
            JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'), GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))
        def run(label, command, extra=None, seconds=600):
            p = subprocess.run(list(map(str, command)), cwd=ROOT, env={**env, **(extra or {})}, capture_output=True, text=True, timeout=seconds)
            output = p.stdout + p.stderr; (logs / (label + '.log')).write_text(output)
            if p.returncode:
                diagnostics = [(str(f.relative_to(work)), f.read_text(errors='replace')) for f in work.glob('*/o/stderr')]
                raise RuntimeError(label + '\n' + output + '\nREMOTE STDERR: ' + repr(diagnostics))
            return output
        if args.macos:
            source = args.project.resolve(strict=True) if args.project else work / 'h'
            if not args.project:
                for folder in ['apple/GhostermCore', 'apple/GhostermTerminal', 'apple/GhostermApp']:
                    shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
                (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include', symlinks=True)
                (source / 'core/build').symlink_to(build / 'core', target_is_directory=True)
                for folder in ['build', 'patches']: (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
                (source / 'tmp').mkdir()
            run('compile', ['swift', 'build', '--build-tests', '--package-path', source / 'apple/GhostermApp'])
        else:
            source = args.project.resolve(strict=True) if args.project else ROOT
            adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]
            run('compile', [ROOT / 'android/gradlew', '-p', source / 'android', '-PghostermPluginJs=true',
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'])
            for name in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
                apk = source / 'android/app/build/outputs/apk' / name
                run('install-' + apk.name, [*adb, 'install', '-r', '-t', apk])
                package = 'com.hanxi.ghosterm.test' if 'androidTest/' in name else 'com.hanxi.ghosterm'
                installed = run('path-' + apk.name, [*adb, 'shell', 'pm', 'path', package]).strip().removeprefix('package:')
                if run('sha-' + apk.name, [*adb, 'shell', 'sha256sum', installed]).split()[0] != sha(apk): raise RuntimeError('Installed APK mismatch')
        with keys.owned_agent(work, 'agent') as agent:
            host_keys = [keys.add_memory_key(agent, 'execution-hop-' + str(i)) for i in range(2)]
            route_root = work / 'r'; route_root.mkdir()
            with routes.chain(route_root, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                for index, case in enumerate(selected):
                    runtime, route, mode = case.split(':')
                    root = work / str(index); root.mkdir()
                    package, observation, stage, home = [root / p for p in ('k', 'o', 't', 'h')]
                    for folder in [package, observation, stage, home]: folder.mkdir(mode=0o700)
                    fields = ['first', 'empty', 'literal']
                    schema = dict(type='object', properties={p: dict(type='string', maxLength=256) for p in fields}, required=fields,
                        additionalProperties=False, **{'x-ghosterm-cli': dict(version=1, fields={p: dict(position=i) for i, p in enumerate(fields)})})
                    variant = dict(id='selected', runtime=runtime, requires=['session.rpc'])
                    if runtime == 'node': variant.update(entry='executor/index.mjs', runtimeVersionRange='>=20.0.0 <1000.0.0')
                    else: variant.update(executable='executor/main', os=[{'Darwin': 'macos', 'Linux': 'linux'}[platform.system()]],
                        arch=[{'arm64': 'aarch64', 'aarch64': 'aarch64', 'x86_64': 'x86_64'}[platform.machine()]])
                    manifest = dict(schemaVersion=1, id='org.ghosterm.execution', pluginVersion='0.1.0', sdkApiRange='^1.0.0',
                        bridgeProtocolRange=[1], publisher='fixture', license='MIT',
                        commands=[dict(path=['fixture'], title='Fixture', argsSchema='schemas/input.json')],
                        client=dict(runtime='portable-js', entry='client/index.js', requires=['session.rpc']),
                        executor=dict(variants=[variant]), permissions=[])
                    files = {'plugin.json': json.dumps(manifest).encode(), 'schemas/input.json': json.dumps(schema).encode(),
                        'client/index.js': b'export function activate(){return {dispose(){}}}', 'README': b'Owned execution fixture',
                        'LICENSES/MIT.txt': (ROOT / 'LICENSE').read_bytes(),
                        ('executor/index.mjs' if runtime == 'node' else 'executor/main'):
                            (ROOT / 'scripts/fixtures/plugin-execution-app.mjs').read_bytes() if runtime == 'node' else (build / 'test_gterm_exec_peer').read_bytes()}
                    for name, content in files.items():
                        p = package / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(content)
                    members = [dict(path=name, bytes=len(content), sha256=hashlib.sha256(content).hexdigest()) for name, content in sorted(files.items())]
                    (package / 'integrity.json').write_text(json.dumps(dict(schemaVersion=1, members=members)))
                    digest = sha(package / 'integrity.json')
                    wrapper = root / 'g'
                    wrapper.write_text('#!/bin/sh\nexec ' + shlex.join([sys.executable, str(ROOT / 'scripts/fixtures/plugin-execution-supervisor.py'),
                        str(build / 'gterm'), str(observation), str(home)]) + ' "$@"\n'); wrapper.chmod(0o700)
                    scratch = root / 'c'; scratch.mkdir()
                    payload = dict(runtime=runtime, route=route, mode=mode, ports=ports, user=pwd.getpwuid(os.getuid()).pw_name,
                        pem=private.decode('ascii'), packageFiles={str(p.relative_to(package)): base64.b64encode(p.read_bytes()).decode()
                        for p in package.rglob('*') if p.is_file()}, packageDigest=digest, packageDirectory=str(package),
                        wrapper=str(wrapper), temporary=str(stage), scratch=str(scratch), node=str(node), nodeVersion=version,
                        nodeDigest=sha(node), sdk=str(sdk), sdkDigest=sdk_digest)
                    token = secrets.token_hex(24); owned_pids = {}
                    def observe():
                        answer = {'wrapperStarted': (observation / 'gterm-pid').exists(), 'activated': (observation / 'activated').exists(),
                            'handler': (observation / 'handler').exists(), 'entered': (observation / 'entered').exists()}
                        for file in ('gterm-pid', 'entered', 'activated'):
                            if (observation / file).exists():
                                pid = int((observation / file).read_text())
                                answer[{'entered': 'entry-pid', 'activated': 'runtime-pid'}.get(file, file)] = pid
                                observed = identity(pid)
                                if observed and pid not in owned_pids:
                                    expected = str(package) if file == 'gterm-pid' else str(stage)
                                    if expected not in observed: raise RuntimeError('Fixture PID does not match its owned command')
                                    owned_pids[pid] = observed
                        if (observation / 'environment').exists(): answer['environment'] = (observation / 'environment').read_text()
                        if (observation / 'exit').exists(): answer.update(json.loads((observation / 'exit').read_text()))
                        return answer
                    class Handler(socketserver.StreamRequestHandler):
                        def handle(self):
                            self.connection.settimeout(5)
                            if self.rfile.readline(128).rstrip(b'\n') != token.encode(): return
                            action = self.rfile.readline(32).strip()
                            if action == b'fixture': result = payload
                            elif action in (b'audit', b'kill'):
                                result = observe()
                                if action == b'kill':
                                    if fault is None: raise RuntimeError('No owned network fault wire')
                                    fault.disconnect()
                            else: raise RuntimeError('Unknown fixture action')
                            data = json.dumps(result, separators=(',', ':')).encode()
                            if len(data) > 1048576: raise RuntimeError('Fixture response limit')
                            self.wfile.write(struct.pack('>I', len(data)) + data)
                    mapped = []
                    fault = None
                    if mode == 'pending-loss':
                        fault = fault_module.FaultWire(ports[0] if route == 'jump' else ports[1])
                        fault.__enter__()
                        client_ports = list(ports); client_ports[0 if route == 'jump' else 1] = fault.port
                        payload['ports'] = client_ports
                    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                        server.daemon_threads = True; thread = threading.Thread(target=server.serve_forever); thread.start()
                        try:
                            port = server.server_address[1]
                            if args.macos:
                                output = run(str(index), ['swift', 'test', '--skip-build', '--package-path', source / 'apple/GhostermApp',
                                    '--filter', 'ExecutorSSHAppTests.testRemoteExecutionThroughAppModel'],
                                    {'GHOSTERM_EXECUTION_PORT': str(port), 'GHOSTERM_EXECUTION_TOKEN': token}, 180)
                                if 'Executed 1 test, with 0 failures' not in output or 'skipped' in output.lower(): raise RuntimeError('Expected one executed Swift test')
                            else:
                                for p in (port, payload['ports'][0] if route == 'jump' else payload['ports'][1]):
                                    run('reverse', [*adb, 'reverse', f'tcp:{p}', f'tcp:{p}']); mapped.append(p)
                                output = run(str(index), [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                                    'com.hanxi.ghosterm.ExecutorSSHAppTest#remoteExecutionThroughViewModelAndConsentUI',
                                    '-e', 'fixturePort', port, '-e', 'fixtureToken', token,
                                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], seconds=180)
                                if 'OK (1 test)' not in output or 'INSTRUMENTATION_STATUS: numtests=1\n' not in output or any(
                                    mark in output for mark in ['INSTRUMENTATION_FAILED', 'assumption failed:', 'INSTRUMENTATION_STATUS_CODE: -4']): raise RuntimeError(case + '\n' + output)
                            deadline = time.monotonic() + 15
                            while time.monotonic() < deadline:
                                audit = observe()
                                if not any(identity(pid) == expected for pid, expected in owned_pids.items()) and not list(stage.iterdir()): break
                                time.sleep(.05)
                            else: raise RuntimeError(case + ': remote process/staging retained: ' + repr(observe()) + '\n' + (observation / 'stderr').read_text(errors='replace'))
                            if 'status' not in audit: audit['exitObservation'] = 'not-started' if mode == 'no-approval' else 'unknown'
                            records.append(dict(case=case, audit=audit)); print('PASS ' + case + ' ' + json.dumps(audit), flush=True)
                        finally:
                            for p in mapped: run('reverse-remove', [*adb, 'reverse', '--remove', f'tcp:{p}'])
                            server.shutdown(); thread.join(); server.server_close()
                            if fault is not None: fault.__exit__(None, None, None)
                            observe()
                            for pid, expected in owned_pids.items():
                                if identity(pid) == expected:
                                    # Only this fixture's observed child IDs; never the caller/SSHD group.
                                    os.kill(pid, signal.SIGKILL)
                                    for _ in range(100):
                                        if not alive(pid): break
                                        time.sleep(.01)
                    for p in package.rglob('*'):
                        if p.is_file() and (b'PRIVATE KEY-----' in p.read_bytes() or b'openssh-key-v1\0' in p.read_bytes()): raise RuntimeError('Fixture key serialized')
        if any(sha(ROOT / p) != value for p, value in hashes.items()) or any(sha(Path(p)) != value for p, value in binary_hashes.items()):
            raise RuntimeError('Inputs changed during conformance')
        receipt = dict(accepted=True, platform='macos' if args.macos else 'android', cases=records, inputs=hashes,
            binaries=binary_hashes, nodeVersion=version, sdkCatalog=sdk_digest, scope='owned loopback direct/ProxyJump; actual Native App and Node/Exec; no model/tmux')
        if args.receipt:
            if not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): raise RuntimeError('Use project tmp for receipt')
            args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print('EXECUTION SSH APP PASS: ' + str(len(records)) + ' actual cases', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--macos', action='store_true'); target.add_argument('--android'); target.add_argument('--headless-api', choices=['29', '36'])
    parser.add_argument('--build', type=Path, required=True); parser.add_argument('--node', type=Path, required=True)
    parser.add_argument('--project', type=Path, help='explicit isolated source project under this repository tmp')
    parser.add_argument('--sdk', type=Path, required=True); parser.add_argument('--case', action='append'); parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.project and not args.project.resolve().is_relative_to(ROOT / 'tmp'): parser.error('--project must be an owned project tmp directory')
    if args.headless_api:
        helper = load('execution_owned_headless', 'scripts/test-plugin-codex-patch-android.py')
        with helper.headless(args.headless_api) as serial: execute(args, serial)
    else: execute(args, args.android)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
