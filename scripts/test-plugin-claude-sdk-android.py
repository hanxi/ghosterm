#!/usr/bin/env python3
"""Owned Android Native/SSH/Node/real SDK chain, with an explicit in-memory CLI fixture; no model."""
import argparse
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
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

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module


def run(command, seconds=180):
    shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-sdk-android; shift; gt_timeout "$@"'
    result = subprocess.run(['bash', '-c', shell, 'claude-sdk-android', str(ROOT), str(seconds), *map(str, command)],
        cwd=ROOT, env=dict(os.environ, TMPDIR=str(ROOT / 'tmp')), text=True, capture_output=True)
    if result.returncode: raise RuntimeError(f'Command failed: {command}\n{result.stdout}{result.stderr}')
    return result.stdout + result.stderr


def execute(args, component, helper):
    adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', args.android]
    print('DEVICE: API ' + run([*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip() + '; page ' +
        run([*adb, 'shell', 'getconf', 'PAGESIZE']).strip(), flush=True)
    for name in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
        apk = ROOT / 'android/app/build/outputs/apk' / name
        run([*adb, 'install', '-r', '-t', apk])
        package_id = 'com.hanxi.ghosterm.test' if 'androidTest/' in name else 'com.hanxi.ghosterm'
        installed = run([*adb, 'shell', 'pm', 'path', package_id]).strip().removeprefix('package:')
        if run([*adb, 'shell', 'sha256sum', installed]).split()[0] != hashlib.sha256(apk.read_bytes()).hexdigest():
            raise RuntimeError('Installed APK hash differs')
    package = args.package.resolve(strict=True); gterm = args.gterm.resolve(strict=True)
    package_files = {p.relative_to(package).as_posix(): base64.b64encode(p.read_bytes()).decode() for p in sorted(package.rglob('*')) if p.is_file()}
    package_digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
    node = Path(shutil.which('node')); node_digest = hashlib.sha256(node.read_bytes()).hexdigest()
    node_version = run([node, '--version']).strip()
    routes = load('claude_sdk_routes', ROOT / 'scripts/test-plugin-route.py')
    keys = load('claude_sdk_keys', ROOT / 'scripts/test-plugin-ssh.py')
    streams = load('claude_sdk_streams', ROOT / 'scripts/test-plugin-ssh-stream.py')
    private, public = streams.memory_identity()
    selected = ['direct', 'jump', 'arguments'] if args.route == 'all' else ['direct', 'jump'] if args.route == 'both' else [args.route]
    for route in selected:
        # Keep the actual bootstrap below the unchanged 1024-byte SSH limit.
        with tempfile.TemporaryDirectory(prefix='', dir=ROOT / 'tmp') as temporary:
            owned = Path(temporary)
            for name in ['w', 'h', 'c']: (owned / name).mkdir()
            if args.observation: (owned / 'w/hold-observation').touch()
            target_package = owned / 'k'; shutil.copytree(package, target_package)
            entry = owned / 'e.mjs'; entry.symlink_to(ROOT / 'scripts/fixtures/claude-node-sdk.mjs')
            native = next(component.glob('node_modules/@anthropic-ai/claude-agent-sdk-darwin-*/claude'))
            profile = owned / 'p'
            profile.write_text(json.dumps(dict(version=1, cwd=str(owned / 'w'), executable=str(native), cliVersion='2.1.323',
                sha256=hashlib.sha256(native.read_bytes()).hexdigest(), environmentNames=['HOME', 'PATH', 'TMPDIR', 'CLAUDE_CONFIG_DIR'], tools=[])))
            error, exited = owned / 'stderr', owned / 'exit'
            environment = dict(HOME=str(owned / 'h'), CLAUDE_CONFIG_DIR=str(owned / 'c'),
                TMPDIR=str(ROOT / 'tmp'), PATH=os.environ.get('PATH', '/usr/bin:/bin'))
            policy = '(version 1)(allow default)(deny network*)(deny mach-lookup (global-name "com.apple.securityd"))'
            wrapper = owned / 'g'
            wrapper.write_text('#!/bin/sh\n/usr/bin/env -i ' + ' '.join(shlex.quote(f'{k}={v}') for k, v in environment.items()) +
                ' /usr/bin/sandbox-exec -p ' + shlex.quote(policy) + ' ' + shlex.quote(str(gterm)) + ' "$@" 2>' + shlex.quote(str(error)) +
                '\nstatus=$?\nprintf "%s" "$status" >' + shlex.quote(str(exited)) + '\nexit "$status"\n')
            wrapper.chmod(0o700)
            with keys.owned_agent(owned, 'claude-host-agent') as agent:
                host_keys = [keys.add_memory_key(agent, f'claude-sdk-hop-{index}') for index in range(2)]
                route_dir = owned / 'route'; route_dir.mkdir()
                with routes.chain(route_dir, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                    payload = dict(fixture='ghosterm-claude-sdk', observation=args.observation, ports=ports, user=pwd.getpwuid(os.getuid()).pw_name, pem=private.decode(),
                        packageFiles=package_files, packageDigest=package_digest, packageDirectory=str(target_package), workspace=str(owned / 'w'),
                        node=str(node), nodeVersion=node_version, nodeDigest=node_digest, entry=str(entry), profile=str(profile), gtermWrapper=str(wrapper))
                    token = secrets.token_hex(24)
                    class Handler(socketserver.StreamRequestHandler):
                        def handle(self):
                            self.connection.settimeout(5)
                            request = self.rfile.readline(128).rstrip(b'\n').split(b' ', 1)
                            if request[0] != token.encode(): return
                            command = request[1].decode('ascii') if len(request) == 2 else 'fixture'
                            if command == 'fixture': value = payload
                            elif args.observation and command in ['state', 'release']:
                                if command == 'release': (owned / 'w/observation-release').touch()
                                held = owned / 'w/observation-held.json'; calls = owned / 'w/observation-calls.json'
                                value = dict(held=held.exists(), response=base64.b64encode(held.read_bytes()).decode() if held.exists() else '',
                                    calls=json.loads(calls.read_text()) if calls.exists() else [])
                            else: return
                            data = json.dumps(value, separators=(',', ':')).encode()
                            if len(data) > 1048576: raise RuntimeError('Fixture exceeds one MiB')
                            self.wfile.write(struct.pack('>I', len(data)) + data)
                    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                        server.daemon_threads = True; thread = threading.Thread(target=server.serve_forever); thread.start()
                        port = server.server_address[1]; mapped = []
                        try:
                            for value in [port, ports[0] if route == 'jump' else ports[1]]:
                                run([*adb, 'reverse', f'tcp:{value}', f'tcp:{value}']); mapped.append(value)
                            target = 'com.hanxi.ghosterm.ClaudeSDKAppTest#' + ('emptyArgumentsMatchTheApprovedCandidateSchema' if route == 'arguments'
                                else 'nativeClaudeConversationOver' + ('ProxyJump' if route == 'jump' else 'DirectSSH'))
                            output = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', target,
                                '-e', 'fixturePort', port, '-e', 'fixtureToken', token,
                                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], 600)
                            print(output, flush=True)
                            if ('OK (1 test)' not in output or 'INSTRUMENTATION_STATUS: numtests=1' not in output or any(x in output for x in
                                ['FAILURES', 'INSTRUMENTATION_FAILED', 'INSTRUMENTATION_STATUS_CODE: -4', 'assumption failed:', 'initializationError'])):
                                diagnostic = (owned / 'w/sdk-fixture-failure.txt')
                                raise RuntimeError('Native SDK route failed: ' + (error.read_text() if error.exists() else 'no gterm stderr') + '\n' +
                                    (diagnostic.read_text() if diagnostic.exists() else 'no SDK failure diagnostic'))
                            if route == 'arguments':
                                print('PASS arguments: actual approved package, repeated empty argv and Core status propagation; no backend started', flush=True)
                                continue
                            end = time.monotonic() + 15
                            while not exited.exists() and time.monotonic() < end: time.sleep(.05)
                            status = exited.read_text().strip() if exited.exists() else 'not-exited'
                            stderr = error.read_text().strip() if error.exists() else 'missing'
                            unknown = ('gterm: runtime phase=3 error=6 execution-error=0 child-exit=0 '
                                       'child-signal=0 session-end=6 session-error=3 cleanup=0\n'
                                       'gterm: executor channel outcome unknown')
                            if not (status == '0' and not stderr or status == '7' and stderr == unknown):
                                raise RuntimeError(f'Remote lifecycle did not confirm cleanup: {status}; {stderr}')
                            if args.observation:
                                print(f'PASS {route}: actual native timeout, exact retained reply {(owned / "w/observation-held.json").stat().st_size} bytes, readonly Native UI, global stop/release; gterm exit={status}; no model', flush=True)
                            else:
                                print(f'PASS {route}: Node stop/reap confirmed; gterm exit={status}; explicit CLI fixture, no model', flush=True)
                        finally:
                            if args.observation: (owned / 'w/observation-release').touch()
                            run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
                            for value in reversed(mapped): run([*adb, 'reverse', '--remove', f'tcp:{value}'])
                            server.shutdown(); thread.join(5)
                            if thread.is_alive(): raise RuntimeError('Fixture thread did not exit')
            for path in owned.rglob('*'):
                if path.is_file() and not path.is_symlink() and any(x in path.read_bytes() for x in [b'PRIVATE KEY-----', b'openssh-key-v1\0']):
                    raise RuntimeError('Fixture persisted private key material')
    print(f'ANDROID CLAUDE SDK APP PASS: {sum(route != "arguments" for route in selected)} SSH routes; '
        f'{sum(route == "arguments" for route in selected)} argument fixture; actual Native/SSH/Node/locked SDK; no model', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    device = parser.add_mutually_exclusive_group(required=True)
    device.add_argument('--android'); device.add_argument('--headless-api', choices=['29', '36'])
    parser.add_argument('--gterm', required=True, type=Path)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-claude')
    parser.add_argument('--route', choices=['direct', 'jump', 'both', 'arguments', 'all'], default='both')
    parser.add_argument('--fetch', action='store_true')
    parser.add_argument('--node-tools', type=Path, help='reuse installed Node dependencies from the same lockfile')
    parser.add_argument('--observation', action='store_true', help='hold a real poll reply through the native deadline and read the retained document')
    args = parser.parse_args(); (ROOT / 'tmp').mkdir(exist_ok=True)
    if args.node_tools and args.fetch: parser.error('--node-tools and --fetch are mutually exclusive')
    if args.observation and args.route in ['arguments', 'all']: parser.error('--observation requires direct, jump or both')
    if sys.platform != 'darwin': parser.error('Owned SDK fixture currently requires macOS sandbox-exec; no fallback')
    helper = load('claude_sdk_owned_android', ROOT / 'scripts/test-plugin-codex-patch-android.py')
    with tempfile.TemporaryDirectory(prefix='claude-android-sdk-', dir=ROOT / 'tmp') as temporary:
        component = Path(temporary) / 'node'; shutil.copytree(ROOT / 'plugins/official/ai-claude/node', component)
        install = ['npm', 'ci', '--prefix', component, '--ignore-scripts', '--no-audit', '--no-fund', '--cache', ROOT / 'tmp/claude-npm-cache', '--registry', 'https://registry.npmjs.org']
        if not args.fetch: install.append('--offline')
        if args.node_tools:
            tools = args.node_tools.resolve(strict=True)
            for filename in ('package.json', 'package-lock.json'):
                if (tools / filename).read_bytes() != (component / filename).read_bytes():
                    raise RuntimeError(f'Node tools differ from the locked {filename}')
            (component / 'node_modules').symlink_to((tools / 'node_modules').resolve(strict=True), target_is_directory=True)
        else:
            run(install, 300)
        run(['node', component / 'node_modules/typescript/bin/tsc', '-p', component / 'tsconfig.json', '--noEmitOnError'])
        if args.headless_api:
            with helper.headless(args.headless_api) as serial: args.android = serial; execute(args, component, helper)
        else: execute(args, component, helper)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
