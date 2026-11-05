#!/usr/bin/env python3
"""P0: real nested loopback SSH / tmux routing; never writes replies to a shell.

Three explicit topologies, distinct pinned host identities, in-memory fixture
keys in two owned ssh-agents. An independently authenticated exec pipe is a
separate candidate route, not automatic discovery or a TTY routing repair.
"""
import argparse
import base64
from contextlib import ExitStack, contextmanager
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import pwd
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
SELF = Path(__file__).resolve()
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('plugin_carrier_probe', ROOT / 'scripts/test-plugin-bridge.py')
bridge = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bridge)
timed = bridge.timed


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def checked(args, **kwargs):
    result = timed(args, capture_output=True, **kwargs)
    require(result.returncode == 0, 'command failed: ' + repr(result))
    return result


def field(value):
    return struct.pack('>I', len(value)) + value


def add_memory_key(agent, comment):
    # OpenSSL creates a fresh fixture key on stdout. No private file, command
    # argument, log, report, or snapshot ever contains unlocked key material.
    # Don't use checked(): its failure diagnostics include stdout.
    generated = timed(['openssl', 'genpkey', '-algorithm', 'ED25519', '-outform', 'DER'], capture_output=True)
    require(generated.returncode == 0, 'fixture in-memory key generation failed')
    private = generated.stdout
    require(private[:16] == bytes.fromhex('302e020100300506032b657004220420') and len(private) == 48,
            'unexpected Ed25519 private DER shape')
    public = timed(['openssl', 'pkey', '-inform', 'DER', '-pubout', '-outform', 'DER'],
                   input=private, capture_output=True)
    require(public.returncode == 0 and len(public.stdout) == 44 and
            public.stdout[:12] == bytes.fromhex('302a300506032b6570032100'), 'fixture public derivation failed')
    raw_public = public.stdout[12:]
    blob = field(b'ssh-ed25519') + field(raw_public)
    check = os.urandom(4)
    clear = check + check + field(b'ssh-ed25519') + field(raw_public) + field(private[-32:] + raw_public) + field(comment.encode())
    clear += bytes(range(1, 9 - len(clear) % 8))
    packet = b'openssh-key-v1\x00' + field(b'none') + field(b'none') + field(b'') + struct.pack('>I', 1) + field(blob) + field(clear)
    encoded = base64.b64encode(packet)
    key = b'-----BEGIN OPENSSH PRIVATE KEY-----\n' + b'\n'.join(encoded[i:i+70] for i in range(0, len(encoded), 70)) + b'\n-----END OPENSSH PRIVATE KEY-----\n'
    added = timed(['ssh-add', '-'], input=key, capture_output=True,
                  env=dict(os.environ, SSH_AUTH_SOCK=str(agent), SSH_ASKPASS_REQUIRE='never'))
    require(added.returncode == 0, 'fixture key was not accepted by the owned agent')
    # Python bytes cannot promise cryptographic erasure. Their lifetime ends
    # here; these are ephemeral test keys, not application/user credentials.
    return b'ssh-ed25519 ' + base64.b64encode(blob) + b' ' + comment.encode() + b'\n'


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()


@contextmanager
def owned_agent(work, name):
    path = work / (name + '.sock')
    with (work / (name + '.log')).open('wb') as log:
        process = subprocess.Popen(['ssh-agent', '-D', '-a', str(path)], stdout=log, stderr=log)
        try:
            bridge.wait_for(lambda: path.exists() or process.poll() is not None, [], 'owned agent start')
            require(process.poll() is None, 'owned agent exited during start')
            yield path
        finally:
            stop(process)
            require(process.poll() is not None, 'owned agent did not stop')


@contextmanager
def ssh_fixture(work):
    with ExitStack() as stack:
        hosts = stack.enter_context(owned_agent(work, 'host-agent'))
        client = stack.enter_context(owned_agent(work, 'client-agent'))
        client_public = add_memory_key(client, 'ghosterm-fixture-client')
        identity = work / 'client.pub'
        identity.write_bytes(client_public)
        authorized = work / 'authorized_keys'
        authorized.write_bytes(client_public)
        empty = work / 'empty-zdotdir'
        empty.mkdir()
        config = work / 'ssh_config'
        known = work / 'known_hosts'
        config_text = ''
        known_text = b''
        public_keys = {}
        for name in ('a', 'b'):
            public_keys[name] = add_memory_key(hosts, 'ghosterm-fixture-host-' + name)
            public = work / (name + '.pub')
            public.write_bytes(public_keys[name])
            with socket.socket() as reservation:
                reservation.bind(('127.0.0.1', 0))
                port = reservation.getsockname()[1]
            daemon_config = work / (name + '-sshd.conf')
            daemon_config.write_text(
                f'Port {port}\nListenAddress 127.0.0.1\nHostKey {public}\nHostKeyAgent {hosts}\n'
                f'PidFile {work / (name + ".pid")}\nAuthorizedKeysFile {authorized}\n'
                'PubkeyAuthentication yes\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n'
                'UsePAM no\nStrictModes no\nPerSourcePenalties no\nLogLevel ERROR\n'
                'PermitUserRC no\n'
                'AllowAgentForwarding no\nAllowTcpForwarding no\nAllowStreamLocalForwarding no\n'
                f'PrintMotd no\nPrintLastLog no\nSetEnv HOME={empty} ZDOTDIR={empty} TMPDIR={work}\n')
            log = stack.enter_context((work / (name + '-sshd.log')).open('wb'))
            daemon = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(daemon_config)], stdout=log, stderr=log)
            stack.callback(stop, daemon)

            def ready():
                if daemon.poll() is not None:
                    raise AssertionError('owned sshd start failed: ' + (work / (name + '-sshd.log')).read_text())
                with socket.socket() as probe:
                    probe.settimeout(0.05)
                    return probe.connect_ex(('127.0.0.1', port)) == 0
            bridge.wait_for(ready, [], 'sshd/' + name)
            alias = 'plugin-p0-' + name
            config_text += f'Host {alias}\n HostName 127.0.0.1\n Port {port}\n HostKeyAlias {alias}\n'
            known_text += alias.encode() + b' ' + public_keys[name]
        require(public_keys['a'].split()[1] != public_keys['b'].split()[1], 'host identities collided')
        config_text += (f'Host *\n User {pwd.getpwuid(os.getuid()).pw_name}\n IdentityFile {identity}\n'
                        f' IdentityAgent {client}\n UserKnownHostsFile {known}\n GlobalKnownHostsFile /dev/null\n'
                        ' StrictHostKeyChecking yes\n UpdateHostKeys no\n IdentitiesOnly yes\n BatchMode yes\n'
                        ' ForwardAgent no\n ControlMaster no\n ControlPath none\n PermitLocalCommand no\n'
                        ' ConnectTimeout 5\n LogLevel ERROR\n')
        config.write_text(config_text)
        known.write_bytes(known_text)
        yield config


def ssh(config, alias, command, tty=False):
    return ['/usr/bin/ssh', '-F', str(config), '-tt' if tty else '-T', 'plugin-p0-' + alias,
            'exec ' + shlex.join(list(map(str, command)))]


def peer_command(path, depth, pipe=False):
    return [sys.executable, str(SELF), '--peer', str(path), '--depth', str(depth)] + (['--pipe'] if pipe else [])


def received(path):
    file = Path(str(path) + '.received')
    return file.read_bytes() if file.exists() else b''


@contextmanager
def tmux_server(work, name):
    tmux = shutil.which('tmux')
    require(tmux is not None, 'tmux executable missing')
    config = work / (name + '.conf')
    config.write_text('set -g default-shell /bin/sh\nset -g status off\nset -g allow-passthrough all\nset -s escape-time 0\n')
    command = [tmux, '-S', str(work / (name + '.sock')), '-f', str(config)]

    def run(*args):
        return checked(command + list(args)).stdout.decode().strip()
    try:
        yield command, run
    finally:
        timed(command + ['kill-server'], capture_output=True)
        gone = timed(command + ['-N', 'list-sessions'], capture_output=True)
        require(gone.returncode == 1, 'owned tmux server did not stop: ' + repr(gone))


@contextmanager
def exec_pipe(config, work, label):
    # Explicit independently pinned A -> B exec channel. It never routes via a
    # pane, hostname guessed from an offer, or the foreground terminal input.
    path = work / (label + '-direct')
    command = ssh(config, 'a', ssh(config, 'b', peer_command(path, 0, True)))
    log = (work / (label + '-direct.log')).open('wb')
    process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log)
    try:
        bridge.wait_for(lambda: Path(str(path) + '.ready').exists() or process.poll() is not None, [], 'exec pipe ready')
        require(process.poll() is None, 'explicit exec channel exited')
        yield process, path
    finally:
        try:
            if process.stdin:
                process.stdin.close()
        finally:
            stop(process)
            process.stdout.close()
            log.close()


class RouteClosed(Exception):
    pass


class RouteUnknown(Exception):
    pass


def pipe_request(process, payload):
    if process.poll() is not None or process.stdin is None:
        raise RouteClosed('receiver is no longer live; nothing sent')
    require(0 < len(payload) <= 768, 'explicit pipe request budget exceeded')
    try:
        process.stdin.write(field(payload))
        process.stdin.flush()
    except InterruptedError:
        raise
    except OSError as error:
        raise RouteUnknown('receiver vanished during request write') from error
    result = b''
    end = time.monotonic() + 8
    target = 4 + len(payload)
    while len(result) < target and time.monotonic() < end:
        if select.select([process.stdout], [], [], 0.02)[0]:
            data = os.read(process.stdout.fileno(), target - len(result))
            if not data:
                break
            result += data
    if result != field(payload):
        raise RouteUnknown('sent request has no complete matching response')


def trigger(path, generation):
    next_file = Path(str(path) + '.next')
    next_file.write_text(str(generation))
    next_file.replace(Path(str(path) + '.go'))


def topology(work, config, mode):
    with ExitStack() as stack:
        clients = []
        # Register client cleanup after server contexts so all SSH clients stop
        # before the exact servers they own are torn down.
        if mode == 'ssh-ssh':
            path_a = work / (mode + '-a')
            command = ssh(config, 'a', ssh(config, 'b', peer_command(path_a, 0), True), True)
            inner = None
        else:
            depth = 2 if mode == 'ssh-tmux-ssh-tmux' else 1
            inner_command, inner = stack.enter_context(tmux_server(work, 'inner-' + mode))
            path_a = work / (mode + '-a')
            a = inner('new-session', '-d', '-x', '100', '-y', '32', '-s', 'probe', '-P', '-F', '#{pane_id}',
                      shlex.join(peer_command(path_a, depth)))
            path_b = work / (mode + '-b')
            b = inner('split-window', '-h', '-t', a, '-P', '-F', '#{pane_id}', shlex.join(peer_command(path_b, depth)))
            path_c = work / (mode + '-c')
            c = inner('new-window', '-d', '-t', 'probe', '-P', '-F', '#{pane_id}', shlex.join(peer_command(path_c, depth)))
            inner('select-pane', '-t', a)
            attach_b = ssh(config, 'b', inner_command + ['attach-session', '-t', 'probe'], True)
            if depth == 2:
                outer_command, outer = stack.enter_context(tmux_server(work, 'outer-' + mode))
                outer_a = outer('new-session', '-d', '-x', '100', '-y', '32', '-s', 'probe', '-P', '-F', '#{pane_id}', shlex.join(attach_b))
                outer_path = work / (mode + '-outer')
                outer_b = outer('split-window', '-h', '-t', outer_a, '-P', '-F', '#{pane_id}', shlex.join(peer_command(outer_path, 1)))
                outer('select-pane', '-t', outer_a)
                command = ssh(config, 'a', outer_command + ['attach-session', '-t', 'probe'], True)
            else:
                command = ssh(config, 'a', attach_b, True)
        def close_clients():
            with ExitStack() as closing:
                for client in clients:
                    closing.callback(client.close)
        stack.callback(close_clients)
        def dump_on_error():
            if sys.exc_info()[0] is not None:
                print('Last SSH terminal observations:', repr([client.output[-4000:] for client in clients]), file=sys.stderr)
        stack.callback(dump_on_error)
        clients.append(bridge.Client(command))
        bridge.wait_for(lambda: Path(str(path_a) + '.ready').exists(), clients, mode + '/foreground peer')
        if inner:
            owner = outer if mode == 'ssh-tmux-ssh-tmux' else inner
            bridge.wait_for(lambda: len(owner('list-clients').splitlines()) == 1, clients, mode + '/outermost attached client')
        user_input = b'GT_PLUGIN_ECHO_TEST_TOKEN'
        os.write(clients[0].fd, user_input)
        bridge.wait_for(lambda: received(path_a) == user_input, clients, mode + '/TTY baseline')
        clients[0].drain()
        require(user_input not in clients[0].output, 'TTY unexpectedly echoed protocol/user input')
        pipe, direct = stack.enter_context(exec_pipe(config, work, mode))
        pipe_request(pipe, b'baseline')
        trigger(path_a, 1)
        expected = bridge.frame(bytes(range(256)) * 3)
        bridge.wait_for(lambda: expected in clients[0].output, clients, mode + '/carrier roundtrip')
        status, _, frames = bridge.decode(work / 'carrier', work, clients[0].output, chunk=17)
        require(status == 0 and frames == b'\x03\x00' + bytes(range(256)) * 3, 'nested SSH C decode mismatch')
        observations = ['tty-baseline-no-echo', 'carrier-integrity', 'explicit-exec-baseline']
        if inner:
            # The second attached client reaches B through A as well. It can
            # change the active pane/window without any Ghosterm action.
            clients.append(bridge.Client(ssh(config, 'a', attach_b, True)))
            bridge.wait_for(lambda: len(inner('list-clients').splitlines()) == 2, clients, mode + '/second client attached')
            os.write(clients[1].fd, b'\x02\x1b[C')
            bridge.wait_for(lambda: inner('display-message', '-p', '-t', 'probe', '#{pane_id}') == b, clients, mode + '/external pane switch')
            os.write(clients[0].fd, b'P')
            bridge.wait_for(lambda: received(path_b) == b'P', clients, mode + '/pane misroute')
            pipe_request(pipe, b'after-pane')
            require(received(path_a) == user_input, 'plain TTY reply reached original pane unexpectedly')
            observations.append('tty-pane-misroute')
            os.write(clients[1].fd, b'\x02n')
            bridge.wait_for(lambda: inner('display-message', '-p', '-t', 'probe', '#{pane_id}') == c, clients, mode + '/external window switch')
            os.write(clients[0].fd, b'W')
            bridge.wait_for(lambda: received(path_c) == b'W', clients, mode + '/window misroute')
            pipe_request(pipe, b'after-window')
            observations.append('tty-window-misroute')
            # Real detach and reattach of the second client; select A again.
            os.write(clients[1].fd, b'\x02d')
            bridge.wait_for(lambda: len(inner('list-clients').splitlines()) == 1, clients, mode + '/detach')
            clients[1].close()
            clients.pop()
            clients.append(bridge.Client(ssh(config, 'a', attach_b, True)))
            bridge.wait_for(lambda: len(inner('list-clients').splitlines()) == 2, clients, mode + '/reattach')
            pipe_request(pipe, b'after-reattach')
            observations.append('explicit-exec-survives-detach-reattach')
            if mode == 'ssh-tmux-ssh-tmux':
                outer('select-pane', '-t', outer_b)
                bridge.wait_for(lambda: Path(str(outer_path) + '.ready').exists(), clients, mode + '/outer peer')
                os.write(clients[0].fd, b'O')
                bridge.wait_for(lambda: received(outer_path) == b'O', clients, mode + '/outer pane misroute')
                pipe_request(pipe, b'after-outer-pane')
                observations.append('tty-outer-pane-misroute')
            # A is now in an inactive inner window, and for the two-tmux case
            # its entire SSH path is in an inactive outer pane as well.
            trigger(path_a, 2)
            bridge.wait_for(lambda: clients[0].output.count(expected) == 2, clients, mode + '/background output')
            status, _, frames = bridge.decode(work / 'carrier', work, clients[0].output, chunk=31)
            require(status == 0 and frames == (b'\x03\x00' + bytes(range(256)) * 3) * 2,
                    'background nested SSH C decode mismatch')
            observations.append('background-carrier-integrity')
        # Explicit exec stdin is fixed to one receiver. Its clean EOF ends that
        # command rather than returning to an interactive shell for late input.
        pipe.stdin.close()
        pipe.stdin = None
        bridge.wait_for(lambda: pipe.poll() is not None, clients, mode + '/exec EOF')
        require(pipe.returncode == 0, 'exec EOF did not close cleanly')
        try:
            pipe_request(pipe, b'late-after-close')
        except RouteClosed:
            pass
        else:
            raise AssertionError('closed explicit channel accepted a late request')
        require(received(direct) == (b'baseline' + (b'after-paneafter-windowafter-reattach' if inner else b'') +
                (b'after-outer-pane' if mode == 'ssh-tmux-ssh-tmux' else b'')), 'explicit exec request accounting mismatch')
        observations.append('explicit-exec-EOF-closed')
        with exec_pipe(config, work, mode + '-failure') as (failing, failure_path):
            try:
                pipe_request(failing, b'NO_REPLY')
            except RouteUnknown:
                pass
            else:
                raise AssertionError('lost reply was classified as completed')
            bridge.wait_for(lambda: failing.poll() is not None, clients, mode + '/lost reply child exited')
            require(failing.returncode == 23 and received(failure_path) == b'NO_REPLY',
                    'lost-reply boundary did not execute in the actual receiver')
            observations.append('explicit-exec-lost-reply-unknown')
        print('PASS', mode, ', '.join(observations), flush=True)
        return observations


def peer_main(path, depth, pipe_mode):
    saved = None
    if not pipe_mode:
        import tty
        saved = termios.tcgetattr(0)
        tty.setraw(0)
    try:
        Path(str(path) + '.ready').touch()
        buffer = b''
        emitted = None
        while True:
            go = Path(str(path) + '.go')
            generation = go.read_text() if not pipe_mode and go.exists() else None
            if generation is not None and generation != emitted:
                output = bridge.frame(bytes(range(256)) * 3)
                for _ in range(depth):
                    output = b'\x1bPtmux;' + output.replace(b'\x1b', b'\x1b\x1b') + bridge.ST
                os.write(1, output)
                emitted = generation
            if select.select([0], [], [], 0.01)[0]:
                data = os.read(0, 4096)
                if not data:
                    require(not buffer, 'truncated explicit pipe frame')
                    return
                if pipe_mode:
                    buffer += data
                    while len(buffer) >= 4:
                        size = struct.unpack('>I', buffer[:4])[0]
                        require(0 < size <= 768, 'pipe frame budget exceeded')
                        if len(buffer) < size + 4:
                            break
                        data, buffer = buffer[4:4+size], buffer[4+size:]
                        with Path(str(path) + '.received').open('ab') as receipt:
                            receipt.write(data)
                        if data == b'NO_REPLY':
                            os._exit(23)  # request observed, deliberately no response
                        os.write(1, field(data))
                else:
                    with Path(str(path) + '.received').open('ab') as receipt:
                        receipt.write(data)
    finally:
        if saved is not None:
            termios.tcsetattr(0, termios.TCSANOW, saved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--peer', type=Path, help=argparse.SUPPRESS)
    parser.add_argument('--depth', type=int, default=0, help=argparse.SUPPRESS)
    parser.add_argument('--pipe', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.peer:
        peer_main(args.peer, args.depth, args.pipe)
        return
    inputs = (SELF, ROOT / 'scripts/test-plugin-bridge.py', bridge.SOURCE)
    hashes = {str(path.relative_to(ROOT)): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='plugin-ssh-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        os.environ['TMPDIR'] = str(work)
        os.environ['ZDOTDIR'] = str(work)
        bridge.compile_probe(bridge.SOURCE, work / 'carrier', sanitize=True)
        with ssh_fixture(work) as config:
            # Prove wrong alias/host identity is rejected before protocol input.
            wrong = timed(['/usr/bin/ssh', '-F', str(config), '-o', 'HostKeyAlias=plugin-p0-b',
                           'plugin-p0-a', 'exit 0'], capture_output=True)
            require(wrong.returncode == 255 and b'Host key verification failed' in wrong.stderr,
                    'wrong pinned host identity was accepted: ' + repr(wrong))
            bad_inner = work / 'wrong-inner-ssh.conf'
            text = config.read_text()
            require(text.count('HostKeyAlias plugin-p0-b') == 1, 'inner host-key fixture anchor drift')
            bad_inner.write_text(text.replace('HostKeyAlias plugin-p0-b', 'HostKeyAlias plugin-p0-a'))
            forbidden_peer = work / 'wrong-host-peer'
            wrong = timed(ssh(bad_inner, 'a', ssh(bad_inner, 'b', peer_command(forbidden_peer, 0, True))),
                          capture_output=True)
            require(wrong.returncode == 255 and b'Host key verification failed' in wrong.stderr and
                    not Path(str(forbidden_peer) + '.ready').exists(),
                    'inner pinned identity did not fail before peer activation: ' + repr(wrong))
            outcomes = {mode: topology(work, config, mode) for mode in
                        ('ssh-ssh', 'ssh-ssh-tmux', 'ssh-tmux-ssh-tmux')}
            # Audit regular files only; agent sockets are never opened as files.
            for path in work.rglob('*'):
                if path.is_file():
                    data = path.read_bytes()
                    require(b'PRIVATE KEY-----' not in data and b'openssh-key-v1\x00' not in data,
                            'private key serialization reached a fixture file')
        require(all(hashlib.sha256(path.read_bytes()).hexdigest() == hashes[str(path.relative_to(ROOT))]
                    for path in inputs), 'probe input changed during run')
    print(json.dumps({'production': False, 'scope': 'two isolated loopback sshd on one host',
                      'inputSHA256': hashes, 'topologies': outcomes, 'plainTmuxTtyWrites': 'unsafe',
                      'wrongPinnedIdentityRejected': ['outer-a', 'inner-b'], 'privateFileSerialization': False,
                      'independentRoute': 'explicit pinned SSH exec pipe only'}, sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
