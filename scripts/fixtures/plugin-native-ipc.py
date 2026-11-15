"""Owned Portable foreground IPC fixture; no desktop window or user endpoint.

Only public readiness/instance metadata enters the Native fixture payload. The
private permit remains in gterm's own 0600 file and is never copied or logged.
"""
from contextlib import contextmanager
import errno
import hashlib
import os
from pathlib import Path
import pty
import re
import select
import signal
import stat
import subprocess
import time
import tty

ROOT = Path(__file__).resolve().parents[2]
VIEWS_METHODS = ['alpha.describe', 'alpha.page', 'beta.describe', 'beta.page', 'changes.describe', 'changes.page',
                 'work.describe', 'work.read', 'work.cancel', 'views.reopen']
TASK_METHODS = ['view.task.cancel', 'view.task.describe', 'view.task.read']


class Foreground:
    def __init__(self, binary, package, kind, arguments, work, environment):
        self.binary, self.package, self.kind = binary.resolve(), package.resolve(), kind
        self.work = work; work.mkdir()
        self.endpoint = work / 'e'; self.endpoint.mkdir(mode=0o700)
        digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
        self.call = ['views demo'] if kind == 'views' else ['task primes', *arguments]
        methods = VIEWS_METHODS if kind == 'views' else TASK_METHODS
        services = ['ui.table', 'ui.diff', 'ui.task', 'ui.views'] if kind == 'views' else ['ui.task']
        self.command = [self.binary, 'relay', '--listen', self.endpoint,
                        '--trust-development-digest', digest, '--client-digest', digest]
        for method in methods: self.command += ['--method', method]
        for service in services: self.command += ['--client-capability', service]
        if kind == 'views': self.command += ['--host-method', 'views.client.open']
        self.command += ['--', self.package, *self.call]
        self.environment = dict(environment, TMPDIR=str(ROOT / 'tmp'))
        self.master = self.slave = self.process = self.log = None
        self.result = None
        self.paused = False
        self.fault = None

    def __enter__(self):
        self.master, self.slave = pty.openpty(); tty.setraw(self.slave)
        self.log = (self.work / 'stderr').open('w+b')
        try:
            self.process = subprocess.Popen(list(map(str, self.command)), stdin=self.slave, stdout=self.slave,
                stderr=self.log, env=self.environment, start_new_session=True)
            os.close(self.slave); self.slave = None
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                text = (self.work / 'stderr').read_text()
                match = re.search(r'^gterm: local endpoint ready instance=([a-f0-9]{32})$', text, re.M)
                if match:
                    self.instance = match.group(1)
                    assert self.process.poll() is None
                    assert stat.S_IMODE(self.endpoint.stat().st_mode) == 0o700
                    permit = self.endpoint / 'permit'
                    assert stat.S_ISREG(permit.lstat().st_mode) and stat.S_IMODE(permit.stat().st_mode) == 0o600 and permit.stat().st_size == 56
                    assert stat.S_ISSOCK((self.endpoint / 'channel.sock').lstat().st_mode)
                    return self
                if self.process.poll() is not None: raise RuntimeError('Owned IPC foreground exited before readiness')
                time.sleep(.01)
            raise RuntimeError('Owned IPC foreground readiness timed out')
        except BaseException:
            self.close(); raise

    def observation(self):
        # The command contains only the public instance and invocation, not the
        # private permit. Read process identities without signalling any peer.
        table = subprocess.check_output(['ps', '-axo', 'pid=,command='], text=True, timeout=5)
        attach = []
        prefix = str(self.binary) + ' relay --attach ' + str(self.endpoint) + ' --instance ' + self.instance + ' '
        for line in table.splitlines():
            fields = line.strip().split(None, 1)
            if len(fields) == 2 and fields[1].startswith(prefix): attach.append(int(fields[0]))
        return dict(frontendPID=self.process.pid, frontendAlive=self.process.poll() is None, attacherPIDs=attach,
                    endpointRevoked=not list(self.endpoint.iterdir()), paused=self.paused, fault=self.fault)

    def control(self, action):
        if action == 'pause':
            assert self.process.poll() is None and not self.paused
            os.kill(self.process.pid, signal.SIGSTOP)
            deadline = time.monotonic() + 2
            while time.monotonic() < deadline:
                state = subprocess.check_output(['ps', '-o', 'stat=', '-p', str(self.process.pid)], text=True, timeout=5)
                if 'T' in state:
                    self.paused = True; return
                time.sleep(.01)
            raise RuntimeError('Owned foreground did not enter the stopped state')
        if action == 'kill':
            assert self.paused and self.process.poll() is None
            os.kill(self.process.pid, signal.SIGKILL)
            assert self.process.wait(timeout=8) == -signal.SIGKILL
            self.fault = 'injected-ipc-loss'; return
        raise ValueError('Unknown owned IPC control action')

    def finish(self):
        code = self.process.wait(timeout=8)
        diagnostic = (self.work / 'stderr').read_text()
        assert code == -signal.SIGKILL if self.fault else code in (0, 7), ('owned frontend transport exit', code, diagnostic[-4000:])
        assert not list(self.endpoint.iterdir()), 'one-shot endpoint must be revoked'
        output = bytearray()
        while select.select([self.master], [], [], .02)[0]:
            try: block = os.read(self.master, 4096)
            except OSError as failure:
                if failure.errno == errno.EIO: break
                raise
            if not block: break
            output.extend(block)
            if len(output) > 4096: raise RuntimeError('Unexpected terminal output budget')
        assert not output, 'IPC protocol must not reach the ordinary terminal'
        replay = subprocess.run([self.binary, 'relay', '--attach', self.endpoint, '--instance', self.instance, '--', *self.call],
            input=b'', capture_output=True, env=self.environment, timeout=8)
        assert replay.returncode == 5 and not replay.stdout, 'completed instance cannot be replayed'
        self.result = dict(pid=self.process.pid, exit=code, fault=self.fault, diagnostic=diagnostic,
                           revoked=True, terminalBytes=0, replayExit=replay.returncode)
        return self.result

    def close(self):
        try:
            if self.process is not None and self.process.poll() is None:
                if self.paused: os.kill(self.process.pid, signal.SIGCONT)
                os.killpg(self.process.pid, signal.SIGTERM)
                try: self.process.wait(timeout=8)
                except subprocess.TimeoutExpired:
                    os.killpg(self.process.pid, signal.SIGKILL); self.process.wait(timeout=3)
                    raise RuntimeError('Owned IPC foreground exceeded cleanup budget')
        finally:
            if self.log is not None: self.log.close(); self.log = None
            for name in ('master', 'slave'):
                fd = getattr(self, name)
                if fd is not None: os.close(fd); setattr(self, name, None)

    def __exit__(self, *_): self.close()


@contextmanager
def foreground(binary, package, kind, arguments, environment):
    import tempfile
    # A short path is necessary for sockaddr_un; the other fixture roots may
    # contain long descriptive labels. Both remain under the project tmp/.
    with tempfile.TemporaryDirectory(prefix='vi-', dir=ROOT / 'tmp') as temporary:
        with Foreground(binary, package, kind, arguments, Path(temporary) / 'f', environment) as owner:
            yield owner
