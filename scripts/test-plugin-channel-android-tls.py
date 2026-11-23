#!/usr/bin/env python3
"""Exercise the Android channel client against an owned loopback TLS server."""
import argparse
import base64
from contextlib import contextmanager, ExitStack, nullcontext
import hashlib
import http.server
import importlib.util
import json
import os
from pathlib import Path
import signal
import ssl
import subprocess
import sys
import tempfile
import threading
import time

from fixtures.plugin_release_signer import memory_signer, sha, sign as sign_bytes, signer_config

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'android/app/src/androidTest/assets/plugin-release'


def fixture(work, env, host, settings=False):
    cert, key = work / 'fixture.crt', work / 'fixture.key'
    subprocess.run(['openssl', 'req', '-new', '-x509', '-newkey', 'rsa:2048', '-nodes',
                    '-subj', f'/CN={host}', '-addext', f'subjectAltName=IP:{host}',
                    '-addext', 'extendedKeyUsage=serverAuth', '-days', '1', '-out', cert, '-keyout', key],
                   cwd=work, env=env, capture_output=True, check=True)
    policy = work / 'original-policy.json'
    def cli(*arguments):
        result = subprocess.run([sys.executable, '-B', ROOT / 'scripts/gterm-plugin', *map(str, arguments)],
                                cwd=work, env=env, capture_output=True, timeout=45)
        report = json.loads(result.stdout)
        if result.returncode:
            raise RuntimeError(f'Fixture producer failed: {report}')
        return report

    if settings:
        publisher_dir = work / 'publisher'
        publisher_dir.mkdir()
        with memory_signer(publisher_dir) as (public, endpoint):
            key_id = sha(base64.b64decode(public))
            policy.write_text(json.dumps(dict(schemaVersion=1, channel='fixture', keys=[
                dict(id=key_id, algorithm='ed25519', publicKey=public, publisher='Ghosterm',
                     pluginIds=['org.ghosterm.preview'])], revokedKeyIds=[])))
            signer = work / 'publisher.json'
            signer.write_text(json.dumps(signer_config(endpoint)))
            archive = work / 'preview.zip'
            cli('pack', ROOT / 'plugins/official/preview', '--out', archive)
            cli('release', 'sign', '--artifact', archive, '--policy', policy, '--key-id', key_id,
                '--signer-json', signer, '--out', work / 'signed-preview')
    else:
        policy.write_bytes(json.loads((ASSETS / 'vector.json').read_bytes())['policy'].encode())
    revoked = json.loads(policy.read_bytes())
    revoked['revokedKeyIds'] = [key['id'] for key in revoked['keys']]
    revoked_policy = work / 'revoked-policy.json'
    revoked_policy.write_text(json.dumps(revoked))
    signer_host = work / 'signer'
    signer_host.mkdir()
    configuration = work / 'signer.json'
    bundle = work / 'signed-channel'
    with memory_signer(signer_host) as (root, endpoint):
        configuration.write_text(json.dumps(signer_config(endpoint)))
        now = int(time.time())
        def sign(policy, generation, bundle):
            report = cli('release', 'sign-policy', '--channel', 'fixture', '--channel-root', root,
                         '--policy', policy, '--generation', generation, '--issued-at', now - 60,
                         '--expires-at', now + 3600, '--signer-json', configuration, '--out', bundle)
            if report.get('kind') != 'signed-channel-policy':
                raise RuntimeError(f'Channel producer failed: {report}')
        sign(policy, 1 if settings else 2, bundle)
        revoked_bundle = work / 'revoked-channel'
        sign(revoked_policy, 3, revoked_bundle)
        expired_statement = json.loads((revoked_bundle / 'channel-policy.json').read_bytes())
        expired_statement.update(generation='4', expiresAt=str(now - 1))
        expired_bytes = json.dumps(expired_statement, separators=(',', ':')).encode()
        expired_signature = sign_bytes(endpoint, b'ghosterm.channel-policy.v1\0' + expired_bytes)
    return cert, key, root, bundle, revoked_bundle, (expired_bytes, expired_signature)


@contextmanager
def tls_fixture(work, env, host, settings=False):
    cert, key, root, bundle, revoked_bundle, expired = fixture(work, env, host, settings)
    names = ('channel-policy.json', 'channel-policy.sig', 'policy.json')
    bodies = {name: (bundle / name).read_bytes() for name in names}
    revoked_bodies = {name: (revoked_bundle / name).read_bytes() for name in names}
    expired_bodies = dict(revoked_bodies, **{'channel-policy.json': expired[0], 'channel-policy.sig': expired[1]})
    routes = {'revoked': revoked_bodies, 'expired': expired_bodies}
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
            body = routes.get(mode, bodies)[name]
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
    parser.add_argument('--apks', nargs=2, type=Path, metavar=('APP_APK', 'TEST_APK'),
                        help='reuse APKs built with -PghostermPluginLua=true for the HTTPS client test')
    parser.add_argument('--serial', help='use an explicitly authorized test device without Ghosterm installed')
    parser.add_argument('--api', type=int, choices=(29, 36), default=36,
                        help='headless AVD API level (default: 36)')
    parser.add_argument('--system-image', type=Path, help='use this installed system image for the headless AVD')
    parser.add_argument('--expect-page-size', type=int, choices=(4096, 16384),
                        help='require this actual device memory page size')
    args = parser.parse_args()
    if args.serial and args.system_image:
        parser.error('--system-image requires a headless AVD')
    if args.system_image:
        args.system_image = args.system_image.resolve(strict=True)
    if args.apks:
        if args.settings_cache:
            parser.error('--settings-cache requires a fresh build with this fixture’s channel root')
        args.apks = [apk.resolve() for apk in args.apks]
        if any(not apk.is_file() for apk in args.apks):
            parser.error('--apks requires two existing APK files')
    else:
        archive = Path(os.environ.get('GHOSTERM_LUA_ARCHIVE', '')).resolve()
        if not archive.is_file():
            parser.error('Set GHOSTERM_LUA_ARCHIVE to the pinned Lua source archive')
    sys.dont_write_bytecode = True
    spec = importlib.util.spec_from_file_location('android_release_runner', ROOT / 'scripts/test-plugin-release-android.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    project_tmp = (ROOT / 'tmp').resolve()
    requested_tmp = Path(os.environ.get('TMPDIR', project_tmp)).resolve()
    work_tmp = requested_tmp if requested_tmp.is_dir() and requested_tmp.is_relative_to(project_tmp) else project_tmp
    with tempfile.TemporaryDirectory(dir=work_tmp, prefix='android-channel-tls-') as path:
        work = Path(path)
        env = dict(os.environ, TMPDIR=path, TMP=path, TEMP=path, GHOSTERM_AUTOMATION_OFFSCREEN='1')
        host = '127.0.0.1' if args.serial else '10.0.2.2'
        with tls_fixture(work, env, host, args.settings_cache) as (origin, cert, root):
            if not args.apks:
                build = ['bash', ROOT / 'scripts/build-android.sh', '-PghostermPluginLua=true',
                         f'-PghostermLuaArchive={archive}', ':app:assembleDebug', ':app:assembleDebugAndroidTest']
            if args.settings_cache:
                bundle = work / 'signed-channel'
                revoked = work / 'revoked-channel'
                release = work / 'signed-preview'
                build.extend(['-PghostermPluginChannel=fixture', f'-PghostermPluginChannelRoot={root}',
                              f'-PghostermPluginChannelUrl={origin}/valid/'])
                test = 'com.hanxi.ghosterm.PluginInstallUITest#explicitInstallationPublishesReadonlyVersionWithoutApprovingOrActivating'
                extras = [
                    '-e', 'settingsChannelStatement', base64.b64encode((bundle / 'channel-policy.json').read_bytes()).decode(),
                    '-e', 'settingsChannelSignature', base64.b64encode((bundle / 'channel-policy.sig').read_bytes()).decode(),
                    '-e', 'settingsRevokedStatement', base64.b64encode((revoked / 'channel-policy.json').read_bytes()).decode(),
                    '-e', 'settingsRevokedSignature', base64.b64encode((revoked / 'channel-policy.sig').read_bytes()).decode(),
                    '-e', 'settingsRevokedPolicy', base64.b64encode((revoked / 'policy.json').read_bytes()).decode(),
                    '-e', 'luaReleaseArchive', base64.b64encode((release / 'plugin.zip').read_bytes()).decode(),
                    '-e', 'luaReleaseDescriptor', base64.b64encode((release / 'release.json').read_bytes()).decode(),
                    '-e', 'luaReleaseSignature', base64.b64encode((release / 'release.sig').read_bytes()).decode(),
                    '-e', 'luaReleasePolicy', base64.b64encode((bundle / 'policy.json').read_bytes()).decode(),
                ]
            else:
                test = 'com.hanxi.ghosterm.PluginChannelRefreshTlsTest'
                extras = ['-e', 'channelTlsOrigin', origin, '-e', 'channelTlsRoot', root,
                          '-e', 'channelTlsCertificate', base64.b64encode(cert.read_bytes()).decode()]
            if args.apks:
                apks = args.apks
            else:
                print(runner.run(build, env, 900)[-400:], flush=True)
                output_dir = ROOT / 'android/app/build/outputs/apk'
                apks = [output_dir / name for name in
                        ('debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk')]
            if args.serial:
                sdk = Path(env.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk')).resolve()
                target = nullcontext(([sdk / 'platform-tools/adb', '-s', args.serial], env))
            else:
                device = work / 'device'
                device.mkdir()
                target = runner.headless(args.api, device, env, args.system_image)
            with target as (adb, device_env), ExitStack() as cleanup:
                page_size = runner.run([*adb, 'shell', 'getconf', 'PAGE_SIZE'], device_env).strip()
                if args.expect_page_size is not None and page_size != str(args.expect_page_size):
                    raise RuntimeError(f'Expected page size {args.expect_page_size}, got {page_size}')
                print(f'ANDROID PAGE_SIZE={page_size}', flush=True)
                packages = ('com.hanxi.ghosterm', 'com.hanxi.ghosterm.test')
                if args.serial:
                    aapt_tools = sorted(sdk.glob('build-tools/*/aapt'))
                    if not aapt_tools:
                        raise RuntimeError('Android build-tools aapt is required to verify APK identities')
                    for package, apk in zip(packages, apks):
                        identity = runner.run([aapt_tools[-1], 'dump', 'badging', apk], device_env).splitlines()
                        if not identity or not identity[0].startswith(f"package: name='{package}' "):
                            raise RuntimeError(f'Unexpected APK identity for {apk}')
                    installed = runner.run([*adb, 'shell', 'pm', 'list', 'packages', '-u'], device_env)
                    if any(f'package:{package}' in installed.splitlines() for package in packages):
                        raise RuntimeError('Explicit test device already has Ghosterm or its test package')
                    port = 'tcp:' + origin.rsplit(':', 1)[1]
                    runner.run([*adb, 'reverse', '--no-rebind', port, port], device_env)
                    cleanup.callback(runner.run, [*adb, 'reverse', '--remove', port], device_env)
                for package, apk in zip(packages, apks):
                    with apk.open('rb') as source:
                        expected_digest = hashlib.file_digest(source, 'sha256').hexdigest()
                    runner.run([*adb, 'install', '-t', apk], device_env)
                    if args.serial:
                        cleanup.callback(runner.run, [*adb, 'uninstall', package], device_env)
                    installed = runner.run([*adb, 'shell', 'pm', 'path', package], device_env).splitlines()
                    if len(installed) != 1 or not installed[0].startswith('package:'):
                        raise RuntimeError(f'Expected a single installed APK for {package}')
                    digest = runner.run([*adb, 'shell', 'sha256sum', installed[0].removeprefix('package:')], device_env).split()
                    if not digest or digest[0] != expected_digest:
                        raise RuntimeError(f'Installed APK digest does not match the input for {package}')
                    print(f'INSTALLED APK SHA256 {package} {expected_digest}', flush=True)
                output = runner.run([*adb, 'shell', 'am', 'instrument', '-w', '-e', 'class',
                    test, *extras,
                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], device_env)
                print(output, flush=True)
                if 'OK (1 test)' not in output or 'FAILURES' in output:
                    raise RuntimeError(f'Android channel test did not pass: {test}')


if __name__ == '__main__':
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, interrupted)
    main()
