#!/usr/bin/env python3
"""Exercise the Android channel client against an owned loopback TLS server."""
import base64
from contextlib import contextmanager
import http.server
import importlib.util
import json
import os
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'android/app/src/androidTest/assets/plugin-release'


def fixture(work, env, host):
    cert, key = work / 'fixture.crt', work / 'fixture.key'
    subprocess.run(['openssl', 'req', '-new', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-subj', f'/CN={host}', '-addext', f'subjectAltName=IP:{host}',
                    '-addext', 'extendedKeyUsage=serverAuth', '-days', '1', '-out', cert, '-keyout', key],
                   cwd=work, env=env, capture_output=True, check=True)
    raw = subprocess.run(['node', '--input-type=module', '-e', '''
        import crypto from 'node:crypto';
        import fs from 'node:fs';
        const policy = JSON.parse(fs.readFileSync(process.argv[1])).policy;
        const {publicKey, privateKey} = crypto.generateKeyPairSync('ed25519');
        const root = publicKey.export({format: 'der', type: 'spki'}).subarray(-32).toString('base64');
        const hash = crypto.createHash('sha256').update(policy).digest('hex');
        const now = Math.floor(Date.now() / 1000);
        const statement = JSON.stringify({schemaVersion: 1, channel: 'fixture', generation: '2',
            issuedAt: String(now - 60), expiresAt: String(now + 3600), policySHA256: hash});
        const signature = crypto.sign(null, Buffer.from('ghosterm.channel-policy.v1\\0' + statement), privateKey);
        console.log(JSON.stringify({root, statement, policy, signature: signature.toString('base64')}));
    ''', ASSETS / 'vector.json'], cwd=work, env=env, capture_output=True, check=True)
    return cert, key, json.loads(raw.stdout)


@contextmanager
def tls_fixture(work, env, host):
    cert, key, vector = fixture(work, env, host)
    bodies = {'channel-policy.json': vector['statement'].encode(),
              'channel-policy.sig': base64.b64decode(vector['signature']), 'policy.json': vector['policy'].encode()}
    received = []

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            received.append(self.path)
            mode, _, name = self.path.lstrip('/').partition('/')
            if name not in bodies:
                self.send_error(404)
                return
            if mode == 'redirect':
                self.send_response(302)
                self.send_header('Location', '/redirect-target/' + name)
                self.end_headers()
                return
            body = bodies[name]
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
            yield f'https://{host}:{server.server_port}', cert, vector
            if any(item.startswith('/redirect-target/') for item in received):
                raise RuntimeError('Client followed a channel redirect')
        finally:
            server.shutdown()
            thread.join()


def main():
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('android_release_runner', ROOT / 'scripts/test-plugin-release-android.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    with tempfile.TemporaryDirectory(dir=ROOT / 'tmp', prefix='android-channel-tls-') as path:
        work = Path(path)
        env = dict(os.environ, TMPDIR=path, GHOSTERM_AUTOMATION_OFFSCREEN='1')
        with tls_fixture(work, env, '10.0.2.2') as (origin, cert, vector):
            device = work / 'device'
            device.mkdir()
            with runner.headless(36, device, env) as (adb, device_env):
                for apk in ('debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk'):
                    runner.run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs/apk' / apk], device_env)
                output = runner.run([*adb, 'shell', 'am', 'instrument', '-w', '-e', 'class',
                    'com.hanxi.ghosterm.PluginChannelRefreshTlsTest',
                    '-e', 'channelTlsOrigin', origin,
                    '-e', 'channelTlsRoot', vector['root'],
                    '-e', 'channelTlsCertificate', base64.b64encode(cert.read_bytes()).decode(),
                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], device_env)
                print(output, flush=True)
                if 'OK (1 test)' not in output or 'FAILURES' in output:
                    raise RuntimeError('Android TLS test did not pass')


if __name__ == '__main__':
    main()
