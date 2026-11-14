"""Owned SSH PTYs and tmux; independent from the App's plugin connection.

Input peers only record bytes. No user shell input, keys, tmux socket or config.
"""
import errno
import json
import os
from pathlib import Path
import select
import shlex
import shutil
import subprocess
import sys
import termios
import time
import tty


def peer(path):
    saved = termios.tcgetattr(0)
    tty.setraw(0)
    Path(str(path) + '.ready').write_text(str(os.getpid()))
    try:
        total = 0
        while True:
            try: data = os.read(0, 4096)
            except OSError as failure:
                if failure.errno == errno.EIO: return
                raise
            if not data: return
            total += len(data)
            if total > 4096: raise RuntimeError('Inert terminal input exceeded fixture budget')
            with Path(str(path) + '.received').open('ab') as stream: stream.write(data)
    finally:
        try: termios.tcsetattr(0, termios.TCSANOW, saved)
        except termios.error: pass  # A closed owned PTY cannot be restored.


class Terminal:
    def __init__(self, work, topology, ports, public, host_keys, agent, bridge, username):
        self.work, self.topology, self.bridge = work, topology, bridge
        self.clients = []; self.servers = {}; self.actions = []; self.stopped = False; self.owners = {}
        self.front=None;self.front_killed=False;self.front_owners={}
        self.tmux = Path(shutil.which('tmux')).resolve(strict=True)
        self.version = subprocess.check_output([self.tmux, '-V'], text=True).strip()
        (work / 'home').mkdir()
        self.env = dict(HOME=str(work / 'home'), TMPDIR=str(Path(__file__).resolve().parents[2] / 'tmp'),
                        PATH='/usr/bin:/bin', TERM='xterm-256color', LANG='C')
        identity = work / 'id.pub'; identity.write_bytes(public)
        known = work / 'known'; config = work / 'ssh.conf'
        known.write_bytes(b''.join(f'terminal-{i} '.encode() + key for i, key in enumerate(host_keys)))
        config.write_text(''.join(f'Host terminal-{i}\n HostName 127.0.0.1\n Port {port}\n HostKeyAlias terminal-{i}\n'
            for i, port in enumerate(ports)) + f'Host *\n User {username}\n IdentityFile {identity}\n IdentityAgent {agent}\n'
            f' UserKnownHostsFile {known}\n GlobalKnownHostsFile /dev/null\n StrictHostKeyChecking yes\n'
            ' IdentitiesOnly yes\n BatchMode yes\n ForwardAgent no\n ControlMaster no\n ControlPath none\n'
            ' PermitLocalCommand no\n UpdateHostKeys no\n ConnectTimeout 5\n LogLevel ERROR\n')
        self.config = config
        self.paths = {name: work / name for name in ('a', 'b', 'c', 'outer')}

    @staticmethod
    def identity(pid):
        result = subprocess.run(['ps', '-p', str(pid), '-o', 'lstart=,command='], capture_output=True, text=True, timeout=5)
        return result.stdout.strip() if result.returncode == 0 else None

    def command(self, name, *args):
        base = self.servers[name]
        result = subprocess.run([*base, *args], env=self.env, capture_output=True, text=True, timeout=8)
        if result.returncode: raise RuntimeError('Owned tmux command failed: ' + result.stderr)
        return result.stdout.strip()

    def server(self, name):
        config = self.work / (name + '.conf')
        config.write_text('set -g default-shell /bin/sh\nset -g status off\nset -s escape-time 0\n')
        self.servers[name] = [str(self.tmux), '-S', str(self.work / (name + '.sock')), '-f', str(config)]

    def inert(self, name):
        return shlex.join([sys.executable, str(Path(__file__).resolve()), '--peer', str(self.paths[name])])

    def ssh(self, hop, command):
        return ['/usr/bin/ssh', '-F', str(self.config), '-tt', 'terminal-' + str(hop),
                'exec ' + shlex.join(command)]

    def attach(self, outer=False):
        command = self.ssh(0 if outer else 1,
                           self.servers['outer' if outer else 'inner'] + ['attach-session', '-t', 'probe'])
        # Client's fork helper inherits its caller env; env -i removes all user
        # TMUX / SSH variables before either ssh or the named tmux server runs.
        command = ['/usr/bin/env', '-i', *[f'{k}={v}' for k, v in self.env.items()], *command]
        return self.bridge.Client(command)

    def wait(self, predicate, label): self.bridge.wait_for(predicate, self.clients, 'Native App tmux/' + label)
    def received(self, name):
        path = Path(str(self.paths[name]) + '.received')
        return path.read_bytes() if path.exists() else b''

    def __enter__(self):
        try:
            self.server('inner')
            self.a = self.command('inner', 'new-session', '-d', '-x', '100', '-y', '32', '-s', 'probe', '-P', '-F', '#{pane_id}', self.inert('a'))
            self.b = self.command('inner', 'split-window', '-h', '-t', self.a, '-P', '-F', '#{pane_id}', self.inert('b'))
            self.c = self.command('inner', 'new-window', '-d', '-t', 'probe', '-P', '-F', '#{pane_id}', self.inert('c'))
            self.command('inner', 'select-pane', '-t', self.a)
            if self.topology == 'nested':
                self.server('outer')
                nested = self.ssh(1, self.servers['inner'] + ['attach-session', '-t', 'probe'])
                self.outer_a = self.command('outer', 'new-session', '-d', '-x', '100', '-y', '32', '-s', 'probe', '-P', '-F', '#{pane_id}', shlex.join(nested))
                self.outer_b = self.command('outer', 'split-window', '-h', '-t', self.outer_a, '-P', '-F', '#{pane_id}', self.inert('outer'))
                self.command('outer', 'select-pane', '-t', self.outer_a)
            self.clients.append(self.attach(self.topology == 'nested'))
            self.clients.append(self.attach())
            self.wait(lambda: len(self.command('inner', 'list-clients').splitlines()) == 2 and
                      all(Path(str(self.paths[n]) + '.ready').exists() for n in ('a', 'b', 'c')), 'two authenticated terminal clients')
            os.write(self.clients[0].fd, b'B')
            self.wait(lambda: self.received('a') == b'B', 'baseline input owner')
            for name in self.servers:
                ids = [int(self.command(name, 'display-message', '-p', '#{pid}'))]
                ids += [int(value) for value in self.command(name, 'list-panes', '-a', '-F', '#{pane_pid}').splitlines()]
                for pid in ids:
                    command = self.identity(pid)
                    if not command: raise RuntimeError('Owned terminal process disappeared during setup')
                    self.owners[pid] = command
            self.actions.append('baseline')
            return self
        except BaseException:
            self.close(); raise

    def foreground(self, command, environment):
        if self.front is not None:raise RuntimeError('One foreground pane per owned terminal')
        self.paths['front']=self.work/'front'
        wrapper=Path(__file__).with_name('plugin-foreground-tty.py')
        arguments=['/usr/bin/env','-i',*[f'{key}={value}' for key,value in self.env.items() if key!='TMPDIR'],
            'TMPDIR='+environment['TMPDIR'],sys.executable,str(wrapper),str(self.paths['front']),*map(str,command)]
        self.front=self.command('inner','new-window','-d','-t','probe','-n','plugin','-P','-F','#{pane_id}',shlex.join(arguments))
        self.wait(lambda:Path(str(self.paths['front'])+'.ready').exists(),'foreground PTY child ready')
        for pid in [int(self.command('inner','display-message','-p','-t',self.front,'#{pane_pid}')),
                    int(Path(str(self.paths['front'])+'.ready').read_text())]:
            identity=self.identity(pid)
            if not identity:raise RuntimeError('Owned foreground pane process disappeared')
            self.owners[pid]=identity
            self.front_owners[pid]=identity

    def front_input(self, value):
        previous=self.command('inner','display-message','-p','-t','probe','#{pane_id}')
        for pane in (self.front,previous):
            index=self.command('inner','display-message','-p','-t',pane,'#{window_index}')
            if len(index)!=1 or not index.isdigit():raise RuntimeError('Owned fixture needs one digit window index')
            os.write(self.clients[1].fd,b'\x02'+index.encode('ascii'))
            self.wait(lambda:self.command('inner','display-message','-p','-t','probe','#{pane_id}')==pane,'second client selects foreground/previous window')
            if pane==previous:break
            os.write(self.clients[1].fd,value)
            expected=(b'F' if 'tmux-front' in self.actions else b'')+value
            self.wait(lambda:self.received('front')==expected,'inert input in foreground pane')

    def kill_foreground(self):
        if self.front is None or self.front_killed:raise RuntimeError('No owned live foreground pane')
        self.command('inner','kill-pane','-t',self.front);self.front_killed=True
        self.actions.append('front-pane-closed')

    def action(self, action):
        if self.stopped: raise RuntimeError('Owned terminal has already stopped')
        if action == 'tmux-pane':
            os.write(self.clients[1].fd, b'\x02\x1b[C')
            self.wait(lambda: self.command('inner', 'display-message', '-p', '-t', 'probe', '#{pane_id}') == self.b, 'second client switches pane')
            os.write(self.clients[0].fd, b'P')
            self.wait(lambda: self.received('b') == b'P', 'typed input follows selected pane')
        elif action == 'tmux-window':
            os.write(self.clients[1].fd, b'\x02n')
            self.wait(lambda: self.command('inner', 'display-message', '-p', '-t', 'probe', '#{pane_id}') == self.c, 'second client switches window')
            os.write(self.clients[0].fd, b'W')
            self.wait(lambda: self.received('c') == b'W', 'typed input follows selected window')
            os.write(self.clients[1].fd, b'\x02d')
            self.wait(lambda: len(self.command('inner', 'list-clients').splitlines()) == 1, 'second client detached')
            self.clients.pop().close(); self.clients.append(self.attach())
            self.wait(lambda: len(self.command('inner', 'list-clients').splitlines()) == 2, 'second client reattached')
            if self.topology == 'nested':
                self.command('outer', 'select-pane', '-t', self.outer_b)
                self.wait(lambda: Path(str(self.paths['outer']) + '.ready').exists(), 'outer input peer')
                os.write(self.clients[0].fd, b'O')
                self.wait(lambda: self.received('outer') == b'O', 'outer pane routing')
        elif action == 'tmux-front':self.front_input(b'F')
        elif action == 'tmux-stop-terminal': self.close()
        else: raise RuntimeError('Unknown owned terminal action')
        self.actions.append(action)
        return self.info()

    def info(self):
        return dict(topology=self.topology, version=self.version, actions=list(self.actions), stopped=self.stopped,
                    received={name: self.received(name).decode('ascii') for name in self.paths})

    def finish(self):
        if self.front is not None and not self.front_killed:
            self.wait(lambda:Path(str(self.paths['front'])+'.status').exists(),'foreground supervisor reaped')
            self.front_input(b'Z')
        if not self.stopped:
            if self.topology == 'nested' and 'tmux-window' in self.actions:
                os.write(self.clients[0].fd, b'S')
                self.wait(lambda: self.received('outer') == b'OS', 'terminal survives plugin shutdown')
            else:
                name = 'c' if 'tmux-window' in self.actions else 'b' if 'tmux-pane' in self.actions else 'a'
                expected = {'a': b'B', 'b': b'P', 'c': b'W'}[name] + b'S'
                os.write(self.clients[0].fd, b'S')
                self.wait(lambda: self.received(name) == expected, 'terminal survives plugin shutdown')
            self.actions.append('after-plugin-stop')
        expected = dict(a='B', b='P' if 'tmux-pane' in self.actions else '',
                        c='W' if 'tmux-window' in self.actions else '',
                        outer='O' if self.topology == 'nested' and 'tmux-window' in self.actions else '')
        if 'after-plugin-stop' in self.actions:
            last = 'outer' if self.topology == 'nested' and 'tmux-window' in self.actions else 'c' if 'tmux-window' in self.actions else 'b' if 'tmux-pane' in self.actions else 'a'
            expected[last] += 'S'
        if self.front is not None:expected['front']=('F' if 'tmux-front' in self.actions else '')+('' if self.front_killed else 'Z')
        if self.info()['received'] != expected: raise AssertionError('Plugin/terminal input ownership changed: ' + repr(self.info()))
        return self.info()

    def close(self):
        if self.stopped: return
        errors = []
        for client in reversed(self.clients):
            try: client.close()
            except Exception as failure: errors.append(str(failure))
        self.clients.clear()
        for base in reversed(list(self.servers.values())):
            subprocess.run([*base, 'kill-server'], env=self.env, capture_output=True, timeout=8)
            result = subprocess.run([*base, '-N', 'list-sessions'], env=self.env, capture_output=True, timeout=8)
            if result.returncode != 1: errors.append('Owned tmux server did not exit')
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if all(self.identity(pid) != expected for pid, expected in self.owners.items()): break
            time.sleep(.01)
        else: errors.append('Owned terminal process identity remained live')
        self.stopped = not errors
        if errors: raise RuntimeError('Owned tmux cleanup: ' + repr(errors))

    def __exit__(self, *_): self.close()


if __name__ == '__main__':
    if len(sys.argv) != 3 or sys.argv[1] != '--peer': raise RuntimeError('Owned inert peer only')
    peer(Path(sys.argv[2]))
