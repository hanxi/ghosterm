"""Second real OpenSSH endpoint for global file queue isolation tests."""
from contextlib import contextmanager
from pathlib import Path
import os
import shutil
import signal
import shlex
import socket
import subprocess
import time


@contextmanager
def fixture(work, enabled):
    if not enabled:
        yield {}
        return
    work = Path(work)
    peer = work / 'queue-peer'
    peer.mkdir()
    root = peer / 'files'
    root.mkdir()
    (root / '文本.txt').write_text('第二端点的独立内容 ☃\n')
    hostkey = peer / 'hostkey'
    subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(hostkey)],
                   check=True, capture_output=True, timeout=10)
    authorized = peer / 'authorized_keys'
    command = 'internal-sftp -d ' + shlex.quote(str(root))
    option = command.replace('\\', '\\\\').replace('"', '\\"')
    authorized.write_text('command="' + option + '" ' + (work / 'task-file-clientkey.pub').read_text())
    with socket.socket() as listener:
        listener.bind(('127.0.0.1', 0))
        port = listener.getsockname()[1]
    def quoted(path):
        return '"' + str(path).replace('\\', '\\\\').replace('"', '\\"') + '"'
    changes = {'Port': str(port), 'HostKey': quoted(hostkey),
               'PidFile': quoted(peer / 'sshd.pid'), 'AuthorizedKeysFile': quoted(authorized)}
    config = '\n'.join(
        line.split()[0] + ' ' + changes[line.split()[0]]
        if line.split() and line.split()[0] in changes else line
        for line in (work / 'sshd_config').read_text().splitlines())
    config += '\n'
    path = peer / 'sshd_config'
    path.write_text(config)
    sshd = os.environ.get('SSHD') or shutil.which('sshd') or '/usr/sbin/sshd'
    with (peer / 'sshd.log').open('wb') as log:
        process = subprocess.Popen([sshd, '-D', '-e', '-f', str(path)],
                                   stdout=subprocess.DEVNULL, stderr=log, start_new_session=True)
        try:
            deadline = time.monotonic() + 5
            while True:
                if process.poll() is not None:
                    raise RuntimeError('Second queue SSH endpoint exited before readiness')
                try:
                    with socket.create_connection(('127.0.0.1', port), timeout=.2):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError('Second queue SSH endpoint did not become ready')
                    time.sleep(.05)
            yield {'queuePeer': {'port': port, 'root': str(root)}}
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=5)
