#!/usr/bin/env python3
"""Build pinned libssh2 patches in isolation and verify EOF/abandoned-init cleanup."""
import argparse
import hashlib
import importlib.util
import inspect
import json
import os
from pathlib import Path
import select
import secrets
import shlex
import signal
import socket
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
VERSION = '1.11.1'
SHA256 = 'd9ec76cbe34db98eec3539fe2c899d26b0c837cb3eb466a56b0f109cabf658f7'
PATCHES = ROOT / 'patches/libssh2' / VERSION
CASES = {
    'normal': ('allow', 'admit'),
    'header-eof': ('header-eof', 'io'),
    'body-eof': ('eof', 'io'),
    'partial-cancel': ('partial', 'cancel'),
    'partial-timeout': ('partial', 'timeout'),
    'queued-cancel': ('queued-partial', 'cancel'),
}


def fixture(binary, case, args):
    host, port, user, key, work = args
    del host, port, user
    spec = importlib.util.spec_from_file_location('channels', ROOT / 'scripts/lib/android-ssh-channels.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with module.fixture(work, True) as data:
        peer = data['channels']
        def control(**fields):
            with socket.create_connection(('127.0.0.1', peer['controlPort']), timeout=2) as sock:
                sock.sendall((json.dumps(dict(token=peer['token'], **fields)) + '\n').encode())
                return json.loads(sock.makefile().readline())
        action, mode = CASES[case]
        username = control(op='new', stage='version', action=action)['user']
        device = os.environ.get('LIBSSH2_SFTP_SERIAL')
        adb = [str(ROOT / 'build/android-toolchain/sdk/platform-tools/adb'), '-s', device] if device else None
        if adb:
            subprocess.run([*adb, 'reverse', 'tcp:' + str(peer['port']), 'tcp:' + str(peer['port'])], check=True, capture_output=True)
        process = subprocess.Popen([binary, '127.0.0.1', str(peer['port']), username, key, work, mode],
                                   stdin=subprocess.PIPE if adb else None,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        prefix = ''
        pid = None
        def stop_probe():
            if adb and pid:
                subprocess.run([*adb, 'shell', 'kill', '-KILL', str(pid)], capture_output=True)
            if process.poll() is None:
                process.kill()
        try:
            if adb:
                process.stdin.write(Path(key).read_text())
                process.stdin.close()
                process.stdin = None
            if mode == 'cancel' or adb:
                if not select.select([process.stdout], [], [], 8)[0]:
                    raise RuntimeError('Probe did not reach authenticated state')
                prefix = process.stdout.readline()
                if not prefix.startswith('READY '):
                    raise RuntimeError('Probe exited before authenticated state')
                pid = int(prefix.split()[1])
                if pid <= 1:
                    raise RuntimeError('Invalid probe PID')
            if mode == 'cancel':
                deadline = time.monotonic() + 3
                while 'version' not in control(op='state', user=username)['events']:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError('Protocol did not reach VERSION')
                    time.sleep(.01)
                if adb:
                    subprocess.run([*adb, 'shell', 'kill', '-USR1', str(pid)], check=True, capture_output=True)
                else:
                    process.send_signal(signal.SIGUSR1)
            try:
                stdout, stderr = process.communicate(timeout=8)
            except subprocess.TimeoutExpired:
                stop_probe()
                stdout, stderr = process.communicate()
                print(prefix + stdout + stderr)
                print('FAIL: dependency init call did not return within 8 seconds')
                return 1
            print(prefix + stdout + stderr, end='')
            events = control(op='state', user=username)['events']
            if events != ['channel', 'subsystem', 'version']:
                raise RuntimeError('Unexpected protocol stage sequence: ' + repr(events))
            if process.returncode or 'SFTP OPEN PASS' not in stdout:
                return process.returncode or 1
            print('DEPENDENCY SFTP PASS ' + case)
            return 0
        finally:
            if process.poll() is None:
                stop_probe()
                process.communicate()
            try:
                control(op='release', user=username)
            finally:
                if adb:
                    subprocess.run([*adb, 'reverse', '--remove', 'tcp:' + str(peer['port'])], check=True, capture_output=True)


def verify_android_dependency(prefix):
    saved = json.loads((prefix / 'receipt.json').read_text())
    patches = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(PATCHES.glob('*.patch'))}
    if saved['inputs'].get('ssh2_patches') != patches:
        raise RuntimeError('Android dependency receipt does not contain current SFTP patches')
    for name in ('libssh2.a', 'libcrypto.a'):
        if saved['sha256'][name] != hashlib.sha256((prefix / 'lib' / name).read_bytes()).hexdigest():
            raise RuntimeError('Android dependency archive changed: ' + name)


def receipt_mutations(work, archive):
    prefix = work / 'receipt-probe'
    (prefix / 'lib').mkdir(parents=True)
    # Both filenames use a real archive; this probe tests identity receipts, not ABI.
    for name in ('libssh2.a', 'libcrypto.a'):
        (prefix / 'lib' / name).symlink_to(archive)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    good = {'inputs': {'ssh2_patches': {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(PATCHES.glob('*.patch'))}},
            'sha256': {'libssh2.a': digest, 'libcrypto.a': digest}}
    def check(function, kind):
        value = json.loads(json.dumps(good))
        if kind == 'patch': value['inputs']['ssh2_patches'] = {}
        if kind == 'archive': value['sha256']['libssh2.a'] = '0' * 64
        (prefix / 'receipt.json').write_text(json.dumps(value))
        try:
            function(prefix)
        except RuntimeError as error:
            expected = 'current SFTP patches' if kind == 'patch' else 'archive changed'
            if kind and expected in str(error): return
            raise
        if kind: raise AssertionError('receipt accepted ' + kind + ' mismatch')
    text = inspect.getsource(verify_android_dependency)
    for kind, old in [('patch', "saved['inputs'].get('ssh2_patches') != patches"),
                      ('archive', "saved['sha256'][name] != hashlib.sha256((prefix / 'lib' / name).read_bytes()).hexdigest()")]:
        check(verify_android_dependency, kind)
        scope = dict(globals())
        assert text.count(old) == 1
        exec(compile(text.replace(old, 'False'), str(Path(__file__)), 'exec'), scope)
        mutant = scope['verify_android_dependency']
        try:
            check(mutant, kind)
        except AssertionError:
            pass
        else:
            raise RuntimeError('Receipt mutation did not trigger target assertion: ' + kind)
        check(mutant, 'archive' if kind == 'patch' else 'patch')
        check(mutant, None)
        check(verify_android_dependency, kind)
        print('PASS receipt mutation ' + kind + ': compiled red / independent green / restored green', flush=True)


def android(serial):
    sdk = ROOT / 'build/android-toolchain/sdk'
    adb = [str(sdk / 'platform-tools/adb'), '-s', serial]
    host = 'darwin-x86_64' if sys.platform == 'darwin' else 'linux-x86_64'
    cc = sdk / 'ndk/28.2.13676358/toolchains/llvm/prebuilt' / host / 'bin/aarch64-linux-android29-clang'
    prefix = ROOT / 'build/android-native/arm64-v8a'
    verify_android_dependency(prefix)
    logs = ROOT / 'tmp/libssh2-sftp-device'
    logs.mkdir(parents=True, exist_ok=True)
    remote = '/data/local/tmp/ghosterm-sftp-' + secrets.token_hex(8)
    with tempfile.TemporaryDirectory(prefix='libssh2-sftp-device-', dir=ROOT / 'tmp') as directory:
        work = Path(directory)
        binary = work / 'probe'
        subprocess.run([str(cc), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(prefix / 'include'),
                        str(ROOT / 'scripts/lib/libssh2-sftp-probe.c'), str(prefix / 'lib/libssh2.a'),
                        str(prefix / 'lib/libcrypto.a'), '-ldl', '-o', str(binary)], check=True)
        subprocess.run([*adb, 'shell', 'mkdir', '-m', '700', remote], check=True, capture_output=True)
        try:
            subprocess.run([*adb, 'push', str(binary), remote + '/probe'], check=True, capture_output=True)
            subprocess.run([*adb, 'shell', 'chmod', '700', remote + '/probe'], check=True, capture_output=True)
            launcher = work / 'launch'
            launcher.write_text('#!/bin/bash\nexec ' + shlex.join([*adb, 'shell', '-T', remote + '/probe']) +
                                ' "$1" "$2" "$3" - "$5" "$6"\n')
            launcher.chmod(0o700)
            for case in CASES:
                wrapper = work / 'fixture'
                wrapper.write_text('#!/bin/bash\nexec ' + shlex.join(map(str, [ROOT / 'build/ssh-channel-toolchain/bin/python',
                    Path(__file__).resolve(), '--fixture', launcher, case])) + ' "$@"\n')
                wrapper.chmod(0o700)
                result = subprocess.run(['bash', str(ROOT / 'core/tests/scripts/ssh_e2e.sh'), str(wrapper)],
                                        env=dict(os.environ, LIBSSH2_SFTP_SERIAL=serial), capture_output=True, text=True)
                output = result.stdout + result.stderr
                (logs / (case + '.log')).write_text(output)
                if result.returncode or 'DEPENDENCY SFTP PASS ' + case not in output:
                    print(output)
                    raise RuntimeError('Android dependency test failed: ' + case)
                print('PASS Android dependency ' + case, flush=True)
        finally:
            subprocess.run([*adb, 'shell', 'rm', '-rf', remote], check=True, capture_output=True)
    print('ANDROID LIBSSH2 SFTP PATCH PASS: 6 scenarios')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path)
    parser.add_argument('--fetch', action='store_true', help='Download the pinned source if absent')
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--android', metavar='SERIAL', help='Test actual Android dependency archives on this device')
    args = parser.parse_args()
    if args.android:
        if args.mutate:
            parser.error('--mutate builds isolated host libraries; use separately from --android')
        return android(args.android)
    archive = (args.archive or ROOT / 'tmp/libssh2-sources/libssh2-1.11.1.tar.gz').resolve()
    if not archive.exists():
        if not args.fetch:
            raise RuntimeError('Source missing; pass --fetch or --archive (no implicit network access)')
        archive.parent.mkdir(parents=True, exist_ok=True)
        partial = archive.with_suffix('.download')
        urllib.request.urlretrieve('https://libssh2.org/download/libssh2-1.11.1.tar.gz', partial)
        if hashlib.sha256(partial.read_bytes()).hexdigest() != SHA256:
            raise RuntimeError('Downloaded libssh2 SHA256 mismatch')
        partial.replace(archive)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != SHA256:
        raise RuntimeError('libssh2 archive SHA256 mismatch')
    python = ROOT / 'build/ssh-channel-toolchain/bin/python'
    if not python.exists():
        raise RuntimeError('Run bash scripts/bootstrap-ssh-channel-tests.sh first')
    logs = ROOT / 'tmp/libssh2-sftp-tests'
    logs.mkdir(parents=True, exist_ok=True)
    production = [*sorted(PATCHES.glob('*.patch')), ROOT / 'scripts/lib/libssh2-sftp-probe.c',
                  ROOT / 'scripts/lib/android-ssh-channels.py']
    hashes = {p: hashlib.sha256(p.read_bytes()).hexdigest() for p in production}
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'))
    def run(command, name, red=None):
        result = subprocess.run(['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; '
                                 'gt_timeout_require libssh2-sftp; shift; gt_timeout 180 "$@"',
                                 'libssh2-sftp', str(ROOT), *map(str, command)],
                                env=env, capture_output=True, text=True)
        output = result.stdout + result.stderr
        (logs / (name + '.log')).write_text(output)
        okay = result.returncode == 0 if red is None else result.returncode == 1 and red in output
        if not okay:
            print(output)
            raise RuntimeError(name + ': expected ' + ('green' if red is None else 'specific assertion red'))
        return output
    try:
        with tempfile.TemporaryDirectory(prefix='libssh2-sftp-', dir=ROOT / 'tmp') as directory:
            work = Path(directory)
            with tarfile.open(archive) as source:
                source.extractall(work, filter='data')
            src = work / ('libssh2-' + VERSION)
            for patch in sorted(PATCHES.glob('*.patch')):
                run(['patch', '--batch', '--forward', '-d', src, '-p1', '-i', patch], patch.stem)
            build = work / 'build'
            options = []
            if sys.platform == 'darwin':
                sdk = subprocess.check_output(['xcrun', '--sdk', 'macosx', '--show-sdk-path'], text=True).strip()
                options = ['-DCMAKE_OSX_SYSROOT=' + sdk, '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0']
            run(['cmake', '-S', src, '-B', build, '-DBUILD_SHARED_LIBS=OFF', '-DBUILD_STATIC_LIBS=ON',
                 '-DBUILD_EXAMPLES=OFF', '-DBUILD_TESTING=OFF', '-DENABLE_ZLIB_COMPRESSION=OFF',
                 '-DCRYPTO_BACKEND=OpenSSL', *options], 'configure')
            crypto = shlex.split(subprocess.check_output(['pkg-config', '--libs', 'libcrypto'], text=True))
            binary = work / 'probe'
            def compile_probe(name):
                run(['cmake', '--build', build, '-j4'], name + '-build')
                run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(src / 'include'),
                     ROOT / 'scripts/lib/libssh2-sftp-probe.c', build / 'src/libssh2.a', *crypto,
                     '-o', binary], name + '-link')
            wrapper = work / 'fixture'
            def test(case, name, red=None):
                wrapper.write_text('#!/bin/bash\nexec ' + shlex.join(map(str, [python, Path(__file__).resolve(),
                                   '--fixture', binary, case])) + ' "$@"\n')
                wrapper.chmod(0o700)
                out = run(['bash', ROOT / 'core/tests/scripts/ssh_e2e.sh', wrapper], name, red)
                if red is None and 'DEPENDENCY SFTP PASS ' + case not in out:
                    raise RuntimeError(name + ': fixture did not execute')
            compile_probe('baseline')
            for case in CASES:
                test(case, 'baseline-' + case)
                print('PASS dependency ' + case, flush=True)
            if args.mutate:
                sftp = src / 'src/sftp.c'
                session = src / 'src/session.c'
                cases = [
                    ('header', sftp, 'else if(rc == 0) {\n                sftp->packet_header_len = 0;',
                     'else if(rc == 0 && 0) {\n                sftp->packet_header_len = 0;', 'header-eof',
                     'FAIL: dependency init did not return expected state'),
                    ('body', sftp, 'else if(rc <= 0) {\n                LIBSSH2_FREE(session, packet);',
                     'else if(rc < 0) {\n                LIBSSH2_FREE(session, packet);', 'body-eof',
                     'FAIL: dependency init call did not return within 8 seconds'),
                    ('partial', sftp, 'if(sftp->partial_packet)\n        LIBSSH2_FREE(session, sftp->partial_packet);',
                     'if(sftp->partial_packet)\n        (void)sftp->partial_packet;', 'partial-cancel',
                     'FAIL: SFTP init leaked'),
                    ('queued', sftp, 'sftp_packet_flush(sftp);\n    if(sftp->partial_packet)',
                     '(void)sftp;\n    if(sftp->partial_packet)', 'queued-cancel', 'FAIL: SFTP init leaked'),
                    ('owner', session, '_libssh2_sftp_init_free(session);', '(void)session;',
                     'partial-timeout', 'FAIL: SFTP init leaked'),
                ]
                for name, path, old, new, case, failure in cases:
                    clean = path.read_bytes()
                    assert clean.decode().count(old) == 1, name
                    try:
                        path.write_text(clean.decode().replace(old, new))
                        compile_probe(name)
                        test(case, name + '-red', failure)
                        test('normal', name + '-control')
                    finally:
                        path.write_bytes(clean)
                    compile_probe(name + '-restored')
                    test(case, name + '-restored')
                    assert path.read_bytes() == clean
                    print('PASS dependency mutation ' + name + ': compiled red / normal green / restored green / SHA256', flush=True)
                receipt_mutations(work, build / 'src/libssh2.a')
    finally:
        for path, digest in hashes.items():
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise RuntimeError('Production changed during isolated dependency tests: ' + str(path))
    print('LIBSSH2 SFTP PATCH PASS: 6 scenarios' + (' / 5 native + 2 receipt mutations' if args.mutate else ''))


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    if len(sys.argv) > 1 and sys.argv[1] == '--fixture':
        sys.exit(fixture(sys.argv[2], sys.argv[3], sys.argv[4:]))
    main()
