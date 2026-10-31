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
STAGES = ('receive-lstat','receive-open','receive-fstat','receive-read','receive-finalstat','receive-close')
CASES = [('normal','normal',None,None), ('allow','allow','receive-read','allow'),
         ('empty','empty','receive-read','receive-empty'),
         ('exact','exact','receive-read','receive-exact'),
         ('unknown-size','allow','receive-fstat','receive-unknown-size')]
CASES += [(stage+'-'+mode, mode, stage, action) for stage in STAGES
          for mode,action in [('cancel','hold'),('timeout','hold'),('io','disconnect'),('deny','deny'),('eof','eof')]]
CASES += [(stage+'-'+action, 'unsupported' if action in ('unknown-type','link','directory','special') else 'io',
           stage, 'receive-'+action) for stage in ('receive-lstat','receive-fstat','receive-finalstat')
          for action in ('unknown-type','link','directory','special','oversize')]
CASES += [(action,'io','receive-read','receive-'+action) for action in ('growth','shrink','overflow','read-error')]
CASES += [(stage+'-'+action,'io',stage,'receive-'+action) for stage in ('receive-fstat','receive-finalstat')
          for action in ('changed-size','changed-mtime','missing-size','missing-mtime')]
CASES += [('late-cancel','cancel','receive-read','receive-late-cancel'),
          ('late-timeout','timeout','receive-read','receive-late-timeout')]


def main():
    if len(sys.argv) != 6:
        raise RuntimeError('Run through core/tests/scripts/ssh_e2e.sh')
    host, port, user, key, work = sys.argv[1:]
    binary = Path(os.environ.get('GHOSTERM_SFTP_RECEIVE_BIN', ROOT / 'core/build/test_sftp_receive')).resolve()
    spec = importlib.util.spec_from_file_location('channels', ROOT / 'scripts/lib/android-ssh-channels.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    selected = os.environ.get('SFTP_RECEIVE_CASE')
    if selected and selected not in [c[0] for c in CASES]:
        raise RuntimeError('Unknown SFTP_RECEIVE_CASE')
    def command(arguments):
        return ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-receive; shift; gt_timeout 20 "$@"',
                'sftp-receive', str(ROOT), str(binary), *arguments]
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
            arguments = ['127.0.0.1', str(peer['port']), username, key, str(directory), mode] if stage else [host, port, user, key, str(directory)]
            process = subprocess.Popen(command(arguments), env=dict(os.environ, **({"SFTP_RECEIVE_EXPECT_UNKNOWN":"1"} if name in ("unknown-size","overflow") else {})), stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            prefix = ''
            try:
                if mode == 'cancel':
                    if not select.select([process.stdout], [], [], 8)[0]: raise RuntimeError('Receive probe not ready')
                    prefix = process.stdout.readline()
                    if not prefix.startswith('READY '): raise RuntimeError('Receive probe not admitted: ' + prefix)
                    pid = int(prefix.split()[1]); deadline = time.monotonic() + 3
                    barrier = 'receive-after-data' if name == 'late-cancel' else stage
                    while barrier not in control(op='state', user=username)['events']:
                        if time.monotonic() > deadline or process.poll() is not None: raise RuntimeError('Receive barrier not reached')
                        time.sleep(.01)
                    os.kill(pid, signal.SIGUSR1)
                out, error = process.communicate(timeout=12)
                if process.returncode or 'SFTP RECEIVE PASS' not in out:
                    print(prefix + out + error, flush=True)
                    raise RuntimeError('FAIL: SFTP receive ' + name)
                if stage:
                    events = control(op='state', user=username)['events']
                    # libssh2 can pipeline READ requests; compare phase transitions,
                    # and ensure faults do not advance to another protocol stage.
                    unique=[]
                    for event in events:
                        if not unique or event != unique[-1]: unique.append(event)
                    steps=['channel','subsystem','version',*STAGES]
                    if mode in ('allow','empty','exact'):
                        expected=steps
                    elif action in ('receive-growth','receive-shrink','receive-overflow','receive-read-error'):
                        expected=steps[:steps.index('receive-read')+1]
                    elif name in ('late-cancel','late-timeout'):
                        expected=steps[:steps.index('receive-read')+1]+['receive-after-data']
                    else:
                        expected=steps[:steps.index(stage)+1]
                    if unique != expected: raise RuntimeError(f'FAIL: {name} events {unique!r}, expected {expected!r}')

            finally:
                if stage: control(op='release', user=username)
                if process.poll() is None: process.terminate(); process.communicate(timeout=25)
            print('PASS SFTP receive ' + name, flush=True)
    print('SFTP RECEIVE PROTOCOL PASS', flush=True)


if __name__ == '__main__': main()
