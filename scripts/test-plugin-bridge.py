#!/usr/bin/env python3
"""P0 isolated carrier / real tmux probes. No App, CLI or SDK capability claim.

Run --mutate for compiled, assertion-based isolated codec mutations; --tmux for
two real attached clients. Missing requested dependencies fail, never SKIP.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import pty
import random
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'scripts/probes/plugin-carrier.c'
PREFIX = b'\x1bP+ghosterm-probe;'
ST = b'\x1b\\'
MARK = 'GT_PLUGIN_PROBE_MUTATION'
LAST_OBSERVATION = None


def require(ok, message):
    if not ok:
        if LAST_OBSERVATION is not None:
            print('Last probe observation:', repr(LAST_OBSERVATION)[:2000], file=sys.stderr, flush=True)
        raise AssertionError(message)


def timed(args, limit=30, **kwargs):
    command = ['bash', '-c', 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require; shift; gt_timeout "$@"',
               '_', str(ROOT), str(limit)] + [str(x) for x in args]
    return subprocess.run(command, **kwargs)


def frame(payload):
    return PREFIX + payload.hex().encode() + ST


def compile_probe(source, binary, sanitize=False):
    args = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g']
    if sanitize:
        args += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']
    result = timed(args + [source, '-o', binary], capture_output=True)
    require(result.returncode == 0, 'compile failed: ' + result.stderr.decode(errors='replace'))


def decode(binary, work, data, chunk=1, expiry=None):
    global LAST_OBSERVATION
    output = work / 'frames'
    args = [binary, output, chunk] + ([] if expiry is None else [expiry])
    result = timed(args, input=data, capture_output=True)
    LAST_OBSERVATION = result
    require(result.returncode in (0, 2) and not result.stderr, 'probe runtime failure: ' + repr(result))
    LAST_OBSERVATION = result.returncode, result.stdout, output.read_bytes()
    return LAST_OBSERVATION


def codec_case(binary, work, case):
    if case == 'control':
        require(decode(binary, work, frame(b'control')) == (0, b'', b'\x00\x07control'),
                'independent complete frame failed')
    elif case == 'ordinary':
        data = bytes(range(256)) * 50
        require(decode(binary, work, data, 73) == (0, data, b''), 'ordinary bytes changed')
    elif case == 'opaque':
        # A matching substring inside another control string is NOT our frame.
        data = b''.join(b'\x1b' + kind + b'unknown;' + frame(b'fake') + ST
                        for kind in (b'P', b']', b'_', b'^', b'X'))
        data += b'\x1b_Ga=T;AAAA' + ST + b'\x1b]52;c;YWJj\x07'
        require(decode(binary, work, data) == (0, data, b''), 'opaque control string stolen')
    elif case == 'chunks':
        payload = bytes(range(256)) * 3
        wire = b'before' + frame(payload) + frame(b'next') + b'after'
        expected = struct.pack('>H', len(payload)) + payload + b'\x00\x04next'
        for chunk in (1, 2, 3, 7, 17, 31, 257, 8192):
            require(decode(binary, work, wire, chunk) == (0, b'beforeafter', expected),
                    'fragmented frame mismatch/' + str(chunk))
    elif case == 'oversize':
        data = b'before' + frame(b'x' * 769) + frame(b'later') + b'after'
        require(decode(binary, work, data) == (2, b'beforeafter', b''), 'oversize frame accepted')
    elif case == 'malformed':
        for payload in (b'', b'1', b'zz', b'00\n', b'00\x1bQ'):
            data = b'before' + PREFIX + payload + ST + frame(b'later') + b'after'
            require(decode(binary, work, data) == (2, b'beforeafter', b''), 'malformed frame accepted')
    elif case == 'incomplete':
        data = b'before' + PREFIX + b'1234'
        require(decode(binary, work, data) == (2, b'before', b''), 'half frame published or leaked')
        for n in range(1, len(PREFIX)):
            data = b'before' + PREFIX[:n]
            require(decode(binary, work, data) == (0, data, b''), 'partial prefix lost at EOF')
    elif case == 'expiry':
        data = PREFIX + b'12' + ST + frame(b'later') + b'after'
        require(decode(binary, work, data, expiry=len(PREFIX) + 2) == (2, b'after', b''),
                'expired frame accepted')
        data = PREFIX[:6] + b'unknown' + frame(b'fake') + ST
        require(decode(binary, work, data, expiry=6) == (0, data, b''), 'expired prefix lost or stolen')
    elif case == 'random':
        rng = random.Random(20261105)
        for _ in range(24):
            # Unknown terminated strings + binary text + genuine candidate frames.
            ordinary = bytes(rng.randrange(32, 256) for _ in range(1000))
            payload = rng.randbytes(rng.randrange(1, 769))
            data = ordinary + frame(payload) + ordinary[::-1]
            expected = struct.pack('>H', len(payload)) + payload
            require(decode(binary, work, data, rng.randrange(1, 400)) ==
                    (0, ordinary + ordinary[::-1], expected), 'random chunk SHA/byte mismatch')
    else:
        raise ValueError(case)


CASES = ('ordinary', 'opaque', 'chunks', 'oversize', 'malformed', 'incomplete', 'expiry', 'random')


def codec_checks(work, mutate):
    original = SOURCE.read_bytes()
    require(MARK.encode() not in original, 'mutation marker found before start')
    digest = hashlib.sha256(original).hexdigest()
    binary = work / 'carrier'
    compile_probe(SOURCE, binary, sanitize=True)
    for case in CASES:
        codec_case(binary, work, case)
        print('PASS carrier', case, flush=True)
    if mutate:
        mutations = (
            ('ordinary', '            } else {\n                terminal(&c, 1);\n', '            } else { /* drop */\n', 'ordinary bytes changed'),
            ('opaque', '        case OPAQUE:\n            opaque(m, c);\n', '        case OPAQUE:\n            m->state = NORMAL;\n            feed(m, c);\n', 'opaque control string stolen'),
            ('oversize', 'MAX_PAYLOAD = 768', 'MAX_PAYLOAD = 769', 'oversize frame accepted'),
            ('malformed', '(m->digits & 1) || m->digits == 0', '(m->digits & 1)', 'malformed frame accepted'),
            ('incomplete', 'terminal(prefix, m->matched);\n        m->state = OPAQUE;', '/* lost prefix */\n        m->state = OPAQUE;', 'partial prefix lost at EOF'),
            ('expiry', 'reject(m);\n    }\n}', '/* ignore expiry */\n    }\n}', 'expired frame accepted'),
        )
        copy = work / 'carrier.c'
        for case, old, new, assertion in mutations:
            text = original.decode()
            require(text.count(old) == 1, 'mutation anchor drift/' + case)
            try:
                copy.write_text(text.replace(old, new) + '\n/* ' + MARK + ' */\n')
                compile_probe(copy, binary, sanitize=True)
                try:
                    codec_case(binary, work, case)
                except AssertionError as error:
                    require(str(error) == assertion, 'wrong target failure: ' + str(error))
                else:
                    raise AssertionError('mutation survived/' + case)
                codec_case(binary, work, 'control')  # same mutated binary, independent control
            finally:
                copy.write_bytes(original)
                require(hashlib.sha256(copy.read_bytes()).hexdigest() == digest, 'copy restore SHA mismatch')
                require(hashlib.sha256(SOURCE.read_bytes()).hexdigest() == digest, 'source SHA changed')
            compile_probe(copy, binary, sanitize=True)
            codec_case(binary, work, case)
            print('PASS mutation', case, 'assertion red / complete frame green / restored green / SHA256', flush=True)
    return digest


class Client:
    def __init__(self, command):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.execve(command[0], command, dict(os.environ, TERM='xterm-256color'))
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', 24, 80, 0, 0))
        self.output = b''

    def drain(self):
        while select.select([self.fd], [], [], 0)[0]:
            try:
                data = os.read(self.fd, 65536)
            except InterruptedError:
                raise
            except OSError:
                return
            if not data:
                return
            self.output = (self.output + data)[-131072:]

    def close(self):
        os.close(self.fd)
        try:
            os.kill(self.pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        end = time.monotonic() + 5
        while time.monotonic() < end:
            if os.waitpid(self.pid, os.WNOHANG)[0]:
                return
            time.sleep(0.02)
        os.kill(self.pid, signal.SIGKILL)
        os.waitpid(self.pid, 0)


def wait_for(predicate, clients, label):
    end = time.monotonic() + 8
    while time.monotonic() < end:
        for client in clients:
            client.drain()
        if predicate():
            return
        time.sleep(0.01)
    raise AssertionError('tmux observation missing/' + label + ': ' + repr([c.output[-400:] for c in clients]))


def tmux_checks(work):
    tmux = shutil.which('tmux')
    require(tmux is not None, 'requested tmux executable missing')
    config = work / 'tmux.conf'
    config.write_text('set -g status off\nset -g allow-passthrough all\nset -s escape-time 0\n')
    command = [tmux, '-S', str(work / 'socket'), '-f', str(config)]
    clients = []

    def run(*args):
        result = timed(command + list(args), capture_output=True)
        require(result.returncode == 0, 'tmux command failed: ' + result.stderr.decode(errors='replace'))
        return result.stdout.decode().strip()

    def peer(name):
        return shlex.join([sys.executable, str(Path(__file__).resolve()), '--peer', str(work / name)])

    def received(name):
        path = work / (name + '.received')
        return path.read_bytes() if path.exists() else b''

    try:
        a = run('new-session', '-d', '-x', '80', '-y', '24', '-s', 'probe', '-P', '-F', '#{pane_id}', peer('a'))
        b = run('split-window', '-h', '-t', a, '-P', '-F', '#{pane_id}', peer('b'))
        clients.append(Client(command + ['attach-session', '-t', 'probe']))
        clients.append(Client(command + ['attach-session', '-t', 'probe']))
        wait_for(lambda: (work / 'a.ready').exists() and (work / 'b.ready').exists() and
                 len(run('list-clients').splitlines()) == 2, clients, 'two clients and peers ready')
        run('select-pane', '-t', a)
        os.write(clients[0].fd, b'A')
        wait_for(lambda: received('a') == b'A', clients, 'baseline roundtrip')
        # Visible and background passthrough, including the maximum codec fragment.
        for name in ('a', 'b'):
            (work / (name + '.go')).touch()
        for name in ('a', 'b'):
            expected = frame(name.encode() * 768)
            wait_for(lambda: expected in clients[0].output, clients, 'passthrough/' + name)
        status, _, messages = decode(work / 'carrier', work, clients[0].output, chunk=31)
        expected_messages = {struct.pack('>H', 768) + name.encode() * 768 for name in ('a', 'b')}
        require(status == 0 and len(messages) == 1540 and
                {messages[:770], messages[770:]} == expected_messages,
                'real tmux output failed C decoder/integrity check')
        # The OTHER real client sends the prefix + Right Arrow binding to switch
        # the session's active pane. A terminal reply is then sent by client 0.
        os.write(clients[1].fd, b'\x02\x1b[C')
        wait_for(lambda: run('display-message', '-p', '-t', 'probe', '#{pane_id}') == b,
                 clients, 'second client switched pane')
        os.write(clients[0].fd, b'R')
        wait_for(lambda: received('b') == b'R', clients, 'reply misrouted to b')
        require(received('a') == b'A', 'reply unexpectedly reached original peer')
        print('PASS tmux:', len(frame(b'a' * 768)),
              'byte candidate output from both panes decoded intact; reply reached WRONG pane after second-client switch', flush=True)
        return {'version': run('-V'), 'clients': 2, 'baseline': 'a', 'replyAfterSwitch': 'b',
                'unsafePlainTtyReply': True, 'carrierBytes': len(frame(b'a' * 768))}
    finally:
        try:
            timed(command + ['kill-server'], capture_output=True)
        finally:
            for client in clients:
                client.close()
        gone = timed(command + ['list-sessions'], capture_output=True)
        require(gone.returncode == 1, 'owned tmux server did not stop: ' + repr(gone))


def peer_main(path):
    # A bounded inert receiver, never a shell: negative routing probes cannot
    # execute commands. The only output is a private experimental DCS frame.
    import tty
    saved = termios.tcgetattr(0)
    try:
        tty.setraw(0)
        Path(str(path) + '.ready').touch()
        emitted = False
        while True:
            if not emitted and Path(str(path) + '.go').exists():
                raw = frame(path.name.encode() * 768)
                wrapped = b'\x1bPtmux;' + raw.replace(b'\x1b', b'\x1b\x1b') + ST
                os.write(1, wrapped)
                emitted = True
            if select.select([0], [], [], 0.01)[0]:
                data = os.read(0, 1024)
                if not data:
                    return
                with Path(str(path) + '.received').open('ab') as receipt:
                    receipt.write(data)
    finally:
        termios.tcsetattr(0, termios.TCSANOW, saved)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--tmux', action='store_true')
    parser.add_argument('--peer', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.peer:
        peer_main(args.peer)
        return
    (ROOT / 'tmp').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='plugin-p0-', dir=ROOT / 'tmp') as temp:
        work = Path(temp)
        # Compiler temporary files also stay inside this run's private directory.
        os.environ['TMPDIR'] = str(work)
        source_hash = codec_checks(work, args.mutate)
        result = {'sourceSHA256': source_hash, 'codecCases': list(CASES), 'sanitizers': ['ASan', 'UBSan'],
                  'driverSHA256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  'production': False, 'mutations': 6 if args.mutate else 0}
        if args.tmux:
            result['tmux'] = tmux_checks(work)
        print(json.dumps(result, sort_keys=True), flush=True)


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
