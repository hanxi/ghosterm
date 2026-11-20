#!/usr/bin/env python3
"""Production Android Settings over owned SSH, with an explicit no-model patch protocol fixture."""
import argparse
from contextlib import contextmanager
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
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run(command, seconds=120):
    bounded = ['bash', '-c',
        '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require codex-android; shift; gt_timeout "$@"',
        'codex-android', str(ROOT), str(seconds), *map(str, command)]
    result = subprocess.run(bounded, cwd=ROOT, env=dict(os.environ, TMPDIR=str(ROOT / 'tmp')),
                            capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f'Command failed ({result.returncode}): {command}\n{result.stdout}{result.stderr}')
    return result.stdout + result.stderr


@contextmanager
def headless(api, scratch=None):
    """Own the AVD, process group and crash configuration; never open a desktop window."""
    sdk = ROOT / 'build/android-toolchain/sdk'
    with tempfile.TemporaryDirectory(prefix='codex-headless-', dir=scratch or ROOT / 'tmp') as temporary:
        work = Path(temporary)
        for name in ('home', 'user', 'avds', 'emulator', 'crash'):
            (work / name).mkdir()
        avd = work / 'avds/patch.avd'; avd.mkdir()
        image = sdk / f'system-images/android-{api}/google_apis/arm64-v8a'
        if not image.is_dir(): raise RuntimeError(f'Pinned local API {api} image is missing')
        (work / 'avds/patch.ini').write_text(f'avd.ini.encoding=UTF-8\npath={avd}\ntarget=android-{api}\n')
        config = {'AvdId': 'patch', 'avd.ini.encoding': 'UTF-8', 'image.sysdir.1': str(image),
            'abi.type': 'arm64-v8a', 'hw.cpu.arch': 'arm64', 'hw.cpu.ncore': '2', 'hw.ramSize': '2048',
            'hw.lcd.width': '1080', 'hw.lcd.height': '1920', 'hw.lcd.density': '420', 'hw.keyboard': 'yes',
            'hw.gpu.enabled': 'yes', 'hw.gpu.mode': 'swiftshader', 'hw.audioInput': 'no', 'hw.audioOutput': 'no',
            'disk.dataPartition.size': '2G', 'showDeviceFrame': 'no', 'tag.id': 'google_apis',
            'PlayStore.enabled': 'false'}
        (avd / 'config.ini').write_text(''.join(f'{key}={value}\n' for key, value in config.items()))
        port = None
        devices = run([sdk / 'platform-tools/adb', 'devices'])
        for candidate in range(5554, 5586, 2):
            if f'emulator-{candidate}' in devices: continue
            try:
                with socket.socket() as console, socket.socket() as transport:
                    console.bind(('127.0.0.1', candidate)); transport.bind(('127.0.0.1', candidate + 1))
                    port = candidate; break
            except OSError: continue
        if port is None: raise RuntimeError('No unused emulator console/ADB pair')
        environment = dict(os.environ, HOME=str(work / 'home'), TMPDIR=str(ROOT / 'tmp'),
            ANDROID_AVD_HOME=str(work / 'avds'), ANDROID_USER_HOME=str(work / 'user'),
            ANDROID_EMULATOR_HOME=str(work / 'emulator'), ANDROID_TMP=str(work / 'emulator'), ANDROID_SDK_ROOT=str(sdk),
            ANDROID_EMU_ENABLE_CRASH_REPORTING='0', ANDROID_EMU_CRASH_REPORTING_DATABASE=str(work / 'crash'))
        with (work / 'emulator.log').open('w+') as log:
            child = subprocess.Popen([sdk / 'emulator/emulator', '-avd', 'patch', '-port', str(port),
                '-no-window', '-no-audio', '-no-boot-anim', '-no-snapshot', '-no-metrics',
                '-crash-report-mode', 'disabled', '-gpu', 'swiftshader', '-cores', '2', '-memory', '2048'],
                cwd=ROOT, env=environment, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            serial = f'emulator-{port}'
            try:
                deadline = time.monotonic() + 120
                while True:
                    if child.poll() is not None:
                        log.seek(0); raise RuntimeError('Owned headless emulator exited:\n' + log.read())
                    boot = subprocess.run([sdk / 'platform-tools/adb', '-s', serial, 'shell', 'getprop', 'sys.boot_completed'],
                        env=environment, capture_output=True, text=True, timeout=10)
                    if boot.returncode == 0 and boot.stdout.strip() == '1': break
                    if time.monotonic() >= deadline: raise RuntimeError('Owned headless boot timed out')
                    time.sleep(.5)
                log.flush(); log.seek(0); startup = log.read()
                if 'Crashreporting disabled, not reporting crashes.' not in startup:
                    raise RuntimeError('Emulator did not confirm crash reporting is disabled:\n' + startup)
                if 'Storing crashdata in: /tmp/' in startup:
                    raise RuntimeError('Emulator attempted to use system /tmp')
                print(f'HEADLESS READY: API {api}; -no-window; crash reporting disabled; owned project tmp AVD', flush=True)
                yield serial
            finally:
                if child.poll() is None:
                    try:
                        subprocess.run([sdk / 'platform-tools/adb', '-s', serial, 'emu', 'kill'],
                            env=environment, capture_output=True, timeout=10)
                    except subprocess.TimeoutExpired:
                        pass  # Observe/wait on this same owned process before escalation.
                    try: child.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGTERM)
                        try: child.wait(timeout=3)
                        except subprocess.TimeoutExpired:
                            os.killpg(child.pid, signal.SIGKILL); child.wait()


def execute(args):
    (ROOT / 'tmp').mkdir(exist_ok=True)
    package, gterm = args.package.resolve(strict=True), args.gterm.resolve(strict=True)
    adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', args.android]
    api = run([*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip()
    abi = run([*adb, 'shell', 'getprop', 'ro.product.cpu.abi']).strip()
    page = run([*adb, 'shell', 'getconf', 'PAGE_SIZE']).strip()
    if not api.isdigit() or not page.isdigit() or not abi:
        raise RuntimeError('Could not identify the explicit device')
    artifacts = (
        ('com.hanxi.ghosterm', 'debug/app-debug.apk', []),
        ('com.hanxi.ghosterm.test', 'androidTest/debug/app-debug-androidTest.apk', ['-t']),
    )
    for application, relative, flags in artifacts:
        apk = ROOT / 'android/app/build/outputs/apk' / relative
        with apk.open('rb') as stream:
            expected = hashlib.file_digest(stream, 'sha256').hexdigest()
        run([*adb, 'install', '-r', *flags, apk])
        installed = run([*adb, 'shell', 'pm', 'path', application]).strip().splitlines()
        if len(installed) != 1 or not installed[0].startswith('package:'):
            raise RuntimeError(f'Expected one installed APK for {application}')
        actual = run([*adb, 'shell', 'sha256sum', installed[0].removeprefix('package:')]).split()
        if not actual or actual[0] != expected:
            raise RuntimeError(f'Installed APK differs from {relative}')
        print(f'INSTALLED APK {relative}: {expected}', flush=True)
    package_files = {p.relative_to(package).as_posix(): base64.b64encode(p.read_bytes()).decode('ascii')
                     for p in sorted(package.rglob('*')) if p.is_file()}
    digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
    routes = load('codex_android_routes', ROOT / 'scripts/test-plugin-route.py')
    keys = load('codex_android_keys', ROOT / 'scripts/test-plugin-ssh.py')
    streams = load('codex_android_streams', ROOT / 'scripts/test-plugin-ssh-stream.py')
    private, public = streams.memory_identity()
    selected = ('direct', 'jump') if args.route == 'both' else (args.route,)
    with tempfile.TemporaryDirectory(prefix='codex-patch-android-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        with keys.owned_agent(work, 'codex-host-agent') as agent:
            host_keys = [keys.add_memory_key(agent, f'codex-patch-hop-{index}') for index in range(2)]
            route_dir = work / 'route'; route_dir.mkdir()
            with routes.chain(route_dir, 2, public, host_keys, agent, 'echo') as (ports, _, _):
                for route in selected:
                    owned = work / ('codex-patch-' + route); owned.mkdir()
                    workspace = owned / 'workspace'; workspace.mkdir()
                    (workspace / 'fixture.json').write_text('{"fixture":"ghosterm-patch-rpc","version":1}')
                    if args.lost_reply:
                        (workspace / "drop-turn-reply").touch()
                    if args.interrupt_turn:
                        (workspace / "interrupt-turn").touch()
                    if args.close_executor:
                        (workspace / "hold-turn-reply").touch()
                    edit_while_sending = not (args.lost_reply or args.interrupt_turn or args.close_executor)
                    if edit_while_sending:
                        (workspace / 'defer-turn-reply').touch()
                    environment = {'TMPDIR': str(ROOT / 'tmp'), 'PATH': os.environ.get('PATH', '/usr/bin:/bin')}
                    for variable, name in (('HOME', 'home'), ('CODEX_HOME', 'codex'), ('XDG_CONFIG_HOME', 'config')):
                        directory = owned / name; directory.mkdir(); environment[variable] = str(directory)
                    program = owned / 'codex-protocol-fixture'
                    program.write_text('#!/bin/sh\nexec ' + shlex.quote(sys.executable) + ' ' +
                        shlex.quote(str(ROOT / 'scripts/probes/codex-patch-server.py')) + ' "$@"\n')
                    program.chmod(0o700)
                    error, exited = owned / 'gterm.stderr', owned / 'gterm.exit'
                    wrapper = owned / 'gterm-fixture'
                    close_marker = owned / 'close-executor'
                    command = ('/usr/bin/env -i ' +
                        ' '.join(shlex.quote(f'{name}={value}') for name, value in environment.items()) + ' ' +
                        shlex.quote(str(gterm)) + ' "$@" 2>' + shlex.quote(str(error)))
                    if args.close_executor:
                        command = ('exec 3<&0\n' + command + ' <&3 &\nchild=$!\n'
                                   "trap 'kill \"$child\" 2>/dev/null; wait \"$child\"; exit 1' HUP INT TERM\n"
                                   'while kill -0 "$child" 2>/dev/null; do\n'
                                   '  if [ -f ' + shlex.quote(str(close_marker)) + ' ]; then\n'
                                   '    kill -KILL "$child"; break\n'
                                   '  fi\n  sleep 0.05\ndone\nwait "$child"')
                    wrapper.write_text('#!/bin/sh\n' + command +
                        '\nstatus=$?\nprintf "%s" "$status" >' + shlex.quote(str(exited)) + '\nexit "$status"\n')
                    wrapper.chmod(0o700)
                    fixture = dict(fixture='ghosterm-patch-rpc', ports=ports, user=pwd.getpwuid(os.getuid()).pw_name,
                        pem=private.decode('ascii'), packageFiles=package_files, packageDigest=digest,
                        packageDirectory=str(package), workspace=str(workspace), gtermWrapper=str(wrapper),
                        program=str(program), programDigest=hashlib.sha256(program.read_bytes()).hexdigest())
                    token = secrets.token_hex(24)

                    class Handler(socketserver.StreamRequestHandler):
                        def handle(self):
                            self.connection.settimeout(5)
                            if self.rfile.readline(128).rstrip(b'\n') != token.encode():
                                return
                            action = self.rfile.readline(32).rstrip(b'\n')
                            if action == b'fixture': value = fixture
                            elif action == b'update':
                                (workspace / 'request-update').touch(); value = {'ok': True}
                            elif (args.lost_reply or args.close_executor or edit_while_sending) and action == b'turn-receipt':
                                receipt = workspace / 'turn-received.json'
                                value = json.loads(receipt.read_bytes()) if receipt.exists() else {}
                            elif edit_while_sending and action == b'release-turn-reply':
                                (workspace / 'release-turn-reply').touch()
                                value = {'ok': True}
                            elif args.close_executor and action == b'close-executor':
                                if not (workspace / 'turn-received.json').is_file():
                                    value = {'ok': False}
                                else:
                                    close_marker.touch()
                                    value = {'ok': True}
                            elif args.interrupt_turn and action == b'interrupt-receipt':
                                receipt = workspace / 'interrupt-received.json'
                                value = json.loads(receipt.read_bytes()) if receipt.exists() else {}
                            elif args.interrupt_turn and action == b'complete-interrupt':
                                (workspace / 'complete-interrupt').touch()
                                value = {'ok': True}
                            elif action == b'audit':
                                audit = workspace / 'approved.json'
                                value = json.loads(audit.read_bytes()) if audit.exists() else {}
                            else: value = {'error': 'unknown fixture action'}
                            payload = json.dumps(value, separators=(',', ':')).encode()
                            if len(payload) > 1_048_576:
                                raise RuntimeError('Fixture payload exceeds one MiB')
                            self.wfile.write(struct.pack('>I', len(payload)) + payload)

                    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler) as server:
                        server.daemon_threads = True
                        worker = threading.Thread(target=server.serve_forever, name='codex-patch-fixture')
                        worker.start()
                        fixture_port = server.server_address[1]
                        mapped = []
                        try:
                            # Direct may dial only the target; jump may dial only the forwarding-only first hop.
                            for port in (fixture_port, ports[0] if route == 'jump' else ports[1]):
                                run([*adb, 'reverse', f'tcp:{port}', f'tcp:{port}']); mapped.append(port)
                            method = 'completePatchAndVersionedApprovalOver'
                            if args.lost_reply:
                                method = 'lostTurnReplyOver'
                            elif args.interrupt_turn:
                                method = 'interruptWaitsForTerminalEventOver'
                            elif args.close_executor:
                                method = 'executorChannelClosesAfterSendOver'
                            target = 'com.hanxi.ghosterm.CodexPatchRPCAppTest#' + method + (
                                'ProxyJump' if route == 'jump' else 'DirectSSH')
                            transcript = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', target,
                                '-e', 'fixturePort', fixture_port, '-e', 'fixtureToken', token,
                                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], 420)
                            print(transcript, flush=True)
                            if ('OK (1 test)' not in transcript or 'INSTRUMENTATION_STATUS: numtests=1' not in transcript or
                                any(marker in transcript for marker in ('FAILURES', 'INSTRUMENTATION_FAILED',
                                    'INSTRUMENTATION_STATUS_CODE: -4', 'assumption failed:', 'initializationError'))):
                                raise RuntimeError(f'{route}: expected exactly one executed passing test; remote stderr=' +
                                    (error.read_text(errors='replace') if error.exists() else 'missing'))
                            deadline = time.monotonic() + 15
                            while not exited.exists() and time.monotonic() < deadline:
                                time.sleep(.05)
                            exit_status = exited.read_text() if exited.exists() else 'not-exited'
                            exit_error = error.read_text().strip() if error.exists() else 'missing'
                            expected_error = ('gterm: runtime phase=3 error=6 execution-error=0 child-exit=0 '
                                              'child-signal=0 session-end=6 session-error=3 cleanup=0\n'
                                              'gterm: executor channel outcome unknown')
                            if args.lost_reply:
                                if not exited.exists() or not (workspace / 'turn-received.json').is_file():
                                    raise RuntimeError(f'{route}: missing send receipt or owned process exit')
                            elif args.close_executor:
                                if exit_status != '137' or not (workspace / 'turn-received.json').is_file():
                                    raise RuntimeError(f'{route}: owned Executor did not exit after the send: {exit_status}')
                            elif args.interrupt_turn:
                                if not exited.exists() or not (workspace / 'interrupt-received.json').is_file():
                                    raise RuntimeError(f'{route}: missing interrupt receipt or owned process exit')
                            elif exit_status != '7' or exit_error != expected_error:
                                raise RuntimeError(f'Remote close did not retain its independent unknown outcome: {exit_status}; {exit_error}')
                        finally:
                            run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
                            for port in reversed(mapped): run([*adb, 'reverse', '--remove', f'tcp:{port}'])
                            server.shutdown(); worker.join(5)
                            if worker.is_alive(): raise RuntimeError('Fixture server did not stop')
        for path in work.rglob('*'):
            if path.is_file() and any(marker in path.read_bytes() for marker in (b'PRIVATE KEY-----', b'openssh-key-v1\0')):
                raise RuntimeError('Fixture serialized private-key material')
    print(f'ANDROID CODEX PATCH RPC PASS: API {api}; {abi}; page {page}; {len(selected)} routes; production Settings, explicit protocol fixture, no model')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    device = parser.add_mutually_exclusive_group(required=True)
    device.add_argument('--android', help='explicit dedicated test device/headless emulator')
    device.add_argument('--headless-api', choices=('29', '36'), help='create and clean an owned no-window arm64 AVD')
    parser.add_argument('--gterm', required=True, type=Path)
    parser.add_argument('--package', type=Path, default=ROOT / 'build/plugins/ai-codex')
    parser.add_argument('--route', choices=('direct', 'jump', 'both'), default='both')
    fault = parser.add_mutually_exclusive_group()
    fault.add_argument('--lost-reply', action='store_true', help='close the fixture app-server after it receives the send')
    fault.add_argument('--interrupt-turn', action='store_true', help='verify interrupt acknowledgement before terminal event')
    fault.add_argument('--close-executor', action='store_true', help='close the owned SSH Executor after it receives the send')
    args = parser.parse_args()
    (ROOT / 'tmp').mkdir(exist_ok=True)
    if args.headless_api:
        with headless(args.headless_api) as serial:
            args.android = serial
            execute(args)
    else:
        execute(args)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
