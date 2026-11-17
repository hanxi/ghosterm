#!/usr/bin/env python3
"""Passive hints from owned invisible PTYs, never the user's terminal or hosts."""
import argparse
import base64
import errno
import fcntl
import hashlib
import os
from pathlib import Path
import pty
import re
import select
import shlex
import signal
import subprocess
import tempfile
import termios
import time

ROOT = Path(__file__).resolve().parents[1]
TAG = b'\x1b]777;ghosterm-discovery;1;'
READY = b'gterm: local endpoint ready instance='


def require(value, label):
    if not value:
        raise AssertionError(label)


def terminate(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def run(args, case, work):
    endpoint = work / 'e'
    endpoint.mkdir(mode=0o700)
    package = ROOT / 'plugins/official/preview'
    digest = hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()
    command = [str(args.gterm), 'relay', '--listen', str(endpoint),
               '--trust-development-digest', digest, '--client-digest', 'a' * 64,
               '--method', 'preview.describe', '--', str(package), 'preview', 'site/index.md']
    env = {**os.environ, 'TMPDIR': str(work), 'TERM': 'xterm-256color'}
    env.pop('TMUX', None)
    env['GTERM_DISCOVERY_TMUX_LAYERS'] = '4' if case == 'wrapped' else '0'
    if case == 'invalid-setting':
        env['GTERM_DISCOVERY_TMUX_LAYERS'] = '5'
    if case == 'pipes':
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, start_new_session=True, env=env)
        try:
            deadline = time.monotonic() + 5
            while not (endpoint / 'permit').exists() and process.poll() is None:
                require(time.monotonic() < deadline, 'owned pipe startup deadline')
                time.sleep(.01)
            terminate(process)
            out, err = process.communicate(timeout=2)
            require(READY in err and TAG not in out + err,
                    'pipe invocation emits no terminal hint')
        finally:
            terminate(process)
        return
    sockets = []
    if case.startswith('tmux'):
        layers = int(case[-1])
        env['GTERM_DISCOVERY_TMUX_LAYERS'] = str(layers)
        config = work / 'tmux.conf'
        config.write_text('set -g allow-passthrough on\nset -g status off\n')
        for index in range(layers):
            socket = work / f't{index}.sock'
            sockets.append(socket)
            command = [str(args.tmux), '-S', str(socket), '-f', str(config),
                       'new-session', '-s', 'owned', shlex.join(command)]
    pid, master = pty.fork()
    if pid == 0:
        if case == 'background':
            child = os.fork()
            if child:
                os.waitpid(child, 0)
                os._exit(0)
            os.setpgid(0, 0)
        os.execvpe(command[0], command, env)
    before = termios.tcgetattr(master)
    flags = fcntl.fcntl(master, fcntl.F_GETFL)
    output = bytearray()
    try:
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            readable, _, _ = select.select([master], [], [], .1)
            if readable:
                try:
                    data = os.read(master, 8192)
                except OSError as error:
                    if error.errno == errno.EIO:
                        break
                    raise
                if not data:
                    break
                output.extend(data)
            ready = READY in output
            # Ready is printed immediately before the independent hint write.
            if ready and (case in ('background', 'invalid-setting') or TAG in output):
                time.sleep(.05)
                while select.select([master], [], [], 0)[0]:
                    output.extend(os.read(master, 8192))
                break
        require(READY in output, f'{case}: live owned listener')
        require(fcntl.fcntl(master, fcntl.F_GETFL) == flags, 'original fd flags unchanged')
        if not case.startswith('tmux'):
            require(termios.tcgetattr(master) == before, 'original termios unchanged')
        if case in ('background', 'invalid-setting'):
            require(TAG not in output, f'{case}: no hint')
            return
        captured = bytes(output)
        if case == 'wrapped':
            # Check the emitter's explicit four-layer framing separately from
            # actual tmux interoperability (the tmux1 / tmux2 cases below).
            start = captured.index(b'\x1bPtmux;')
            captured = captured[start:]
            for _ in range(4):
                require(captured.startswith(b'\x1bPtmux;'), 'tmux envelope')
                captured = captured[7:-2].replace(b'\x1b\x1b', b'\x1b')
        match = re.search(re.escape(TAG) + rb'([A-Za-z0-9+/=]+)\x1b\\', captured)
        require(match, f'{case}: complete bounded offer')
        raw = base64.b64decode(match[1], validate=True)
        permit = (endpoint / 'permit').read_bytes()
        require(len(permit) == 56 and raw[:16] == permit[8:24], 'public instance')
        require(raw[16:48] == bytes([0xaa]) * 32, 'claimed Client digest')
        require(int.from_bytes(raw[48:50], 'big') == len(raw[50:]), 'directory byte length')
        require(raw[50:].decode() == str(endpoint), 'owned directory only')
        require(permit[24:] not in raw and permit[24:].hex().encode() not in captured,
                'private permit never emitted')
        frame = work / 'capture.bin'
        frame.write_bytes(captured)
        subprocess.run([str(args.observer), 'capture', str(frame), str(endpoint), permit[8:24].hex()],
                       check=True, timeout=3, env=env, stdout=subprocess.DEVNULL)
    finally:
        for socket in sockets:
            subprocess.run([str(args.tmux), '-S', str(socket), 'kill-server'], env=env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=3)
        exited = os.waitpid(pid, os.WNOHANG) != (0, 0)
        if not exited:
            try:
                if sockets:
                    os.kill(pid, signal.SIGTERM)  # Owned client; servers closed above.
                else:
                    os.killpg(pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 3
        while not exited and os.waitpid(pid, os.WNOHANG) == (0, 0):
            if time.monotonic() >= deadline:
                os.killpg(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
                raise RuntimeError('owned PTY process exceeded cleanup deadline')
            time.sleep(.02)
        os.close(master)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--observer', type=Path, required=True)
    parser.add_argument('--tmux', type=Path)
    args = parser.parse_args()
    args.gterm = args.gterm.resolve()
    args.observer = args.observer.resolve()
    cases = ['foreground', 'background', 'pipes', 'wrapped', 'invalid-setting']
    if args.tmux:
        args.tmux = args.tmux.resolve()
        cases += ['tmux1', 'tmux2']
    for case in cases:
        with tempfile.TemporaryDirectory(prefix='gdis-', dir=ROOT / 'tmp') as directory:
            run(args, case, Path(directory))
        print('PASS gterm discovery', case, flush=True)


if __name__ == '__main__':
    main()
