#!/usr/bin/env python3
"""Exercise the Android channel client against an owned loopback TLS server."""
import argparse
import base64
from contextlib import contextmanager
import http.server
import importlib.util
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'android/app/src/androidTest/assets/plugin-release'


def fixture(work, env, host, settings=False):
    cert, key = work / 'fixture.crt', work / 'fixture.key'
    subprocess.run(['openssl', 'req', '-new', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-subj', f'/CN={host}', '-addext', f'subjectAltName=IP:{host}',
                    '-addext', 'extendedKeyUsage=serverAuth', '-days', '1', '-out', cert, '-keyout', key],
                   cwd=work, env=env, capture_output=True, check=True)
    spec = importlib.util.spec_from_file_location('release_fixture', ROOT / 'scripts/test-plugin-release.py')
    helper = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(helper)
    node = Path(shutil.which('node')).resolve(strict=True)
    policy = work / 'original-policy.json'
    vector = 'archive-declarations.json' if settings else 'vector.json'
    policy.write_bytes(json.loads((ASSETS / vector).read_bytes())['policy'].encode())
    revoked = json.loads(policy.read_bytes())
    revoked['revokedKeyIds'] = [key['id'] for key in revoked['keys']]
    revoked_policy = work / 'revoked-policy.json'
    revoked_policy.write_text(json.dumps(revoked))
    signer_host = work / 'signer'
    signer_host.mkdir()
    configuration = work / 'signer.json'
    bundle = work / 'signed-channel'
    with helper.agent(node, signer_host) as (root, endpoint):
        configuration.write_text(json.dumps(dict(executable=str(node), sha256=helper.sha(node.read_bytes()),
            prefixArguments=[str(ROOT / 'scripts/fixtures/plugin-release-agent.mjs'), 'client', str(endpoint)])))
        now = int(time.time())
        def sign(policy, generation, bundle):
            result = subprocess.run([sys.executable, '-B', ROOT / 'scripts/gterm-plugin', 'release', 'sign-policy',
                                     '--channel', 'fixture', '--channel-root', root, '--policy', policy,
                                     '--generation', str(generation), '--issued-at', str(now - 60), '--expires-at', str(now + 3600),
                                     '--signer-json', configuration, '--out', bundle],
                                    cwd=work, env=env, capture_output=True, timeout=45)
            report = json.loads(result.stdout)
            if result.returncode or report.get('kind') != 'signed-channel-policy':
                raise RuntimeError(f'Channel producer failed: {report}')
        sign(policy, 1 if settings else 2, bundle)
        revoked_bundle = work / 'revoked-channel'
        sign(revoked_policy, 3, revoked_bundle)
    return cert, key, root, bundle, revoked_bundle


@contextmanager
def tls_fixture(work, env, host, settings=False):
    cert, key, root, bundle, revoked_bundle = fixture(work, env, host, settings)
    names = ('channel-policy.json', 'channel-policy.sig', 'policy.json')
    bodies = {name: (bundle / name).read_bytes() for name in names}
    revoked_bodies = {name: (revoked_bundle / name).read_bytes() for name in names}
    received = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            received.append(self.path)
            mode, _, name = self.path.lstrip('/').partition('/')
            if name not in bodies or (mode == 'missing-policy' and name == 'policy.json'):
                self.send_error(404)
                return
            if mode == 'redirect':
                self.send_response(302)
                self.send_header('Location', '/redirect-target/' + name)
                self.end_headers()
                return
            body = (revoked_bodies if mode == 'revoked' else bodies)[name]
            if name == 'policy.json':
                if mode == 'oversized':
                    body = b'x' * 131073
                if mode == 'mixed':
                    body += b'\n'
            self.send_response(200)
            # Omit Content-Length to exercise the streaming bound.
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever)
        thread.start()
        try:
            yield f'https://{host}:{server.server_port}', cert, root
            if settings and received:
                raise RuntimeError(f"Settings cache check used the network: {received}")
            if any(item.startswith('/redirect-target/') for item in received):
                raise RuntimeError('Client followed a channel redirect')
        finally:
            server.shutdown()
            thread.join()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--settings-cache', action='store_true',
                        help='build with a disposable channel root and exercise the offline Settings flow')
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('android_release_runner', ROOT / 'scripts/test-plugin-release-android.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    with tempfile.TemporaryDirectory(dir=ROOT / 'tmp', prefix='android-channel-tls-') as path:
        work = Path(path)
        env = dict(os.environ, TMPDIR=path, GHOSTERM_AUTOMATION_OFFSCREEN='1')
        with tls_fixture(work, env, '10.0.2.2', args.settings_cache) as (origin, cert, root):
            build = ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginJs=true',
                     ':app:assembleDebug', ':app:assembleDebugAndroidTest']
            if args.settings_cache:
                bundle = work / 'signed-channel'
                build.extend(['-PghostermPluginChannel=fixture', f'-PghostermPluginChannelRoot={root}',
                              f'-PghostermPluginChannelUrl={origin}/valid/'])
                test = 'com.hanxi.ghosterm.PluginInstallUITest#explicitInstallationPublishesReadonlyVersionWithoutApprovingOrActivating'
                extras = [
                    '-e', 'settingsChannelStatement', base64.b64encode((bundle / 'channel-policy.json').read_bytes()).decode(),
                    '-e', 'settingsChannelSignature', base64.b64encode((bundle / 'channel-policy.sig').read_bytes()).decode(),
                ]
            else:
                test = 'com.hanxi.ghosterm.PluginChannelRefreshTlsTest'
                extras = ['-e', 'channelTlsOrigin', origin, '-e', 'channelTlsRoot', root,
                          '-e', 'channelTlsCertificate', base64.b64encode(cert.read_bytes()).decode()]
            print(runner.run(build, env, 900)[-400:], flush=True)
            device = work / 'device'
            device.mkdir()
            with runner.headless(36, device, env) as (adb, device_env):
                for apk in ('debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk'):
                    runner.run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / apk], device_env)
                output = runner.run([*adb, 'shell', 'am', 'instrument', '-w', '-e', 'class',
                    test, *extras,
                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], device_env)
                print(output, flush=True)
                if 'OK (1 test)' not in output or 'FAILURES' in output:
                    raise RuntimeError(f'Android channel test did not pass: {test}')


if __name__ == '__main__':
    main()
