"""Optional real password servers. Disposable container accounts, never host accounts."""
import contextlib
import secrets
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


@contextlib.contextmanager
def fixture(root, work, key, enabled, public_info=False):
    if not enabled:
        yield {}
        return

    def run(args, data=None, timeout=30):
        result = subprocess.run(args, input=data, capture_output=True, timeout=timeout)
        if result.returncode:
            # Never print argv/stdin: the key-encryption command carries a fixture secret.
            raise RuntimeError('SSH credential fixture command failed: ' + str(args[0]))
        return result.stdout.decode()

    run(['docker', 'info', '--format', '{{.ServerVersion}}'])
    image = 'ghosterm-android-ssh-auth:1'
    build = subprocess.run(['docker', 'build', '-t', image, str(root/'scripts/fixtures/android-ssh-auth')],
                           capture_output=True, timeout=600)
    (root/'tmp/android-ssh-credentials-build.log').write_bytes(build.stdout + build.stderr)
    if build.returncode:
        raise RuntimeError('Password fixture image build failed; see tmp/android-ssh-credentials-build.log')
    encrypted = Path(work)/'encrypted-clientkey'
    passphrase = '测试口令-' + secrets.token_hex(16)
    containers = []
    try:
        shutil.copyfile(key, encrypted)
        encrypted.chmod(0o600)
        run(['ssh-keygen', '-p', '-q', '-f', str(encrypted), '-P', '', '-N', passphrase])
        fields = dict(encryptedPem=encrypted.read_text(), passphrase=passphrase)
        if public_info:
            fields['publicInfoFixtures'] = []
            for algorithm, bits, expected in [('rsa',2048,'ssh-rsa'),('ecdsa',256,'ecdsa-sha2-nistp256'),
                                              ('ecdsa',384,'ecdsa-sha2-nistp384'),('ecdsa',521,'ecdsa-sha2-nistp521')]:
                path = Path(work)/('info-'+algorithm+'-'+str(bits))
                try:
                    run(['ssh-keygen','-q','-t',algorithm,'-b',str(bits),'-f',str(path),'-N',''])
                    fingerprint = run(['ssh-keygen','-lf',str(path)+'.pub','-E','sha256']).split()[1]
                    fields['publicInfoFixtures'].append(dict(pem=path.read_text(),algorithm=expected,fingerprint=fingerprint))
                finally:
                    path.unlink(missing_ok=True)
                    Path(str(path)+'.pub').unlink(missing_ok=True)
        for label, public_key in [('password', False), ('mixed', True)]:
            name = 'ghosterm-android-auth-' + secrets.token_hex(6)
            containers.append(name)
            password = '临时密码-' + secrets.token_hex(16)
            run(['docker', 'run', '-d', '--name', name, '--tmpfs', '/run',
                 '-p', '127.0.0.1::22', image, '-o', 'PubkeyAuthentication='+('yes' if public_key else 'no')])
            run(['docker', 'exec', '-i', name, 'chpasswd'], ('fixture:'+password+'\n').encode())
            run(['docker', 'exec', '-i', name, '/bin/sh', '-c',
                 'cat > /home/fixture/.ssh/authorized_keys; chmod 600 /home/fixture/.ssh/authorized_keys; chown fixture:fixture /home/fixture/.ssh/authorized_keys'],
                Path(str(key)+'.pub').read_bytes())
            port = int(run(['docker', 'port', name, '22/tcp']).strip().rsplit(':', 1)[1])
            deadline = time.monotonic()+10
            while True:
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=1) as probe:
                        if probe.recv(255).startswith(b'SSH-'): break
                except OSError: pass
                if time.monotonic() >= deadline: raise RuntimeError('Password fixture SSH banner timed out')
                time.sleep(.05)
            fingerprint = run(['docker', 'exec', name, 'ssh-keygen', '-lf', '/run/ghosterm/hostkey.pub', '-E', 'sha256']).split()[1]
            fields[label] = dict(port=port, user='fixture', password=password, fingerprint=fingerprint)
        yield fields
    finally:
        failed = []
        for name in reversed(containers):
            try:
                run(['docker', 'rm', '-f', name])
            except (RuntimeError, OSError, subprocess.TimeoutExpired):
                failed.append(name)
        encrypted.unlink(missing_ok=True)
        if failed:
            message = 'Could not remove disposable SSH containers: ' + ', '.join(failed)
            if sys.exc_info()[0] is None:
                raise RuntimeError(message)
            print(message, file=sys.stderr, flush=True)
