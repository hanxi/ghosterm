#!/usr/bin/env python3
"""Run through ssh_e2e.sh with the pinned protocol-test Python interpreter."""
import importlib.util
import json
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
CASES = [('normal', 'normal', None, None), ('jobs', 'jobs', None, None),
         ('allow', 'allow', 'realpath', 'allow'), ('empty', 'empty', 'readdir', 'empty')]
CASES += [(stage + '-' + mode, mode, stage, action)
          for stage in ('realpath', 'opendir', 'readdir', 'closedir')
          for mode, action in [('cancel', 'hold'), ('timeout', 'hold'), ('io', 'disconnect'),
                               ('deny', 'deny'), ('eof', 'eof')]]
CASES += [(action, 'io', 'readdir', action)
          for action in ('empty-name', 'slash-name', 'nul-name', 'duplicate', 'long-name')]
CASES += [('malformed-path', 'io', 'realpath', 'malformed-path'),
          ('relative-path', 'io', 'realpath', 'relative-path'),
          ('limit', 'limit', 'readdir', 'allow'), ('buffer', 'buffer', 'realpath', 'allow'),
          ('raw-path', 'raw-path', 'realpath', 'malformed-path'),
          ('raw-empty', 'raw-empty', 'readdir', 'empty-name'),
          ('raw-normal', 'raw-normal', 'realpath', 'allow')]


def main():
    if len(sys.argv) != 6:
        raise RuntimeError('Run through core/tests/scripts/ssh_e2e.sh')
    host, port, user, key, work = sys.argv[1:]
    binary = Path(os.environ.get('GHOSTERM_SFTP_BROWSE_BIN', ROOT / 'core/build/test_sftp_browse')).resolve()
    spec = importlib.util.spec_from_file_location('channels', ROOT / 'scripts/lib/android-ssh-channels.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    selected = os.environ.get('SFTP_BROWSE_CASE')
    if selected and selected not in [c[0] for c in CASES]:
        raise RuntimeError('Unknown SFTP_BROWSE_CASE')
    def command(arguments):
        return ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-browse; shift; gt_timeout 20 "$@"',
                'sftp-browse', str(ROOT), str(binary), *arguments]
    with module.fixture(work, True) as data:
        peer = data['channels']
        def control(**fields):
            with socket.create_connection(('127.0.0.1', peer['controlPort']), timeout=2) as sock:
                sock.sendall((json.dumps(dict(token=peer['token'], **fields)) + '\n').encode())
                return json.loads(sock.makefile().readline())
        for name, mode, stage, action in CASES:
            if selected and selected != name: continue
            directory = Path(work) / name; directory.mkdir()
            username = control(op='new', stage=stage, action=action)['user'] if stage else user
            arguments = ['127.0.0.1', str(peer['port']), username, key, str(directory), mode] if stage else [host, port, user, key, str(directory), mode]
            process = subprocess.Popen(command(arguments), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            prefix = ''
            try:
                if mode == 'cancel':
                    if not select.select([process.stdout], [], [], 8)[0]: raise RuntimeError('Browse probe not ready')
                    prefix = process.stdout.readline()
                    if not prefix.startswith('READY '): raise RuntimeError('Browse probe not admitted: ' + prefix)
                    pid = int(prefix.split()[1]); deadline = time.monotonic() + 3
                    while stage not in control(op='state', user=username)['events']:
                        if time.monotonic() > deadline or process.poll() is not None: raise RuntimeError('Browse barrier not reached')
                        time.sleep(.01)
                    os.kill(pid, signal.SIGUSR1)
                out, error = process.communicate(timeout=12)
                if process.returncode or 'SFTP BROWSE PASS' not in out:
                    print(prefix + out + error, flush=True)
                    raise RuntimeError('FAIL: SFTP browse ' + name)
                if stage:
                    events = control(op='state', user=username)['events']
                    steps = ['channel', 'subsystem', 'version', 'realpath', 'opendir', 'readdir', 'readdir', 'closedir']
                    if mode.startswith('raw-'):
                        expected = steps[:3] + (['opendir', 'readdir', 'closedir'] if mode == 'raw-empty' else ['realpath'])
                    elif mode in ('allow', 'empty'):
                        expected = (steps if mode == 'allow' else steps[:6] + ['closedir']) + ['realpath']
                    elif name == 'duplicate':
                        expected = steps
                    else:
                        expected = steps[:steps.index(stage) + 1]
                    if events != expected: raise RuntimeError(f'FAIL: {name} events {events!r}, expected {expected!r}')
            finally:
                if stage: control(op='release', user=username)
                if process.poll() is None: process.terminate(); process.communicate(timeout=25)
            print('PASS SFTP browse ' + name, flush=True)
    print('SFTP BROWSE PROTOCOL PASS', flush=True)


if __name__ == '__main__': main()
