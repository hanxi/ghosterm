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
STAGES = ('preview-lstat','preview-open','preview-fstat','preview-read','preview-close')
CASES = [('normal','normal',None,None), ('allow','allow','preview-read','allow'),
         ('empty','empty','preview-read','preview-empty'),
         ('exact','exact','preview-read','preview-exact'),
         ('unknown-size','allow','preview-fstat','preview-unknown-size')]
CASES += [(stage+'-'+mode, mode, stage, action) for stage in STAGES
          for mode,action in [('cancel','hold'),('timeout','hold'),('io','disconnect'),('deny','deny'),('eof','eof')]]
CASES += [(stage+'-'+action, 'unsupported' if action in ('unknown-type','link','directory','special') else 'io',
           stage, 'preview-'+action) for stage in ('preview-lstat','preview-fstat')
          for action in ('unknown-type','link','directory','special','oversize')]
CASES += [(action,'io','preview-read','preview-'+action) for action in ('growth','shrink','overflow','read-error')]
CASES += [('late-cancel','cancel','preview-read','preview-late-cancel'),
          ('late-timeout','timeout','preview-read','preview-late-timeout')]


def main():
    if len(sys.argv) != 6:
        raise RuntimeError('Run through core/tests/scripts/ssh_e2e.sh')
    host, port, user, key, work = sys.argv[1:]
    binary = Path(os.environ.get('GHOSTERM_SFTP_PREVIEW_BIN', ROOT / 'core/build/test_sftp_preview')).resolve()
    spec = importlib.util.spec_from_file_location('channels', ROOT / 'scripts/lib/android-ssh-channels.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    selected = os.environ.get('SFTP_PREVIEW_CASE')
    if selected and selected not in [c[0] for c in CASES]:
        raise RuntimeError('Unknown SFTP_PREVIEW_CASE')
    def command(arguments):
        return ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-preview; shift; gt_timeout 20 "$@"',
                'sftp-preview', str(ROOT), str(binary), *arguments]
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
                    if not select.select([process.stdout], [], [], 8)[0]: raise RuntimeError('Preview probe not ready')
                    prefix = process.stdout.readline()
                    if not prefix.startswith('READY '): raise RuntimeError('Preview probe not admitted: ' + prefix)
                    pid = int(prefix.split()[1]); deadline = time.monotonic() + 3
                    barrier = 'preview-after-data' if name == 'late-cancel' else stage
                    while barrier not in control(op='state', user=username)['events']:
                        if time.monotonic() > deadline or process.poll() is not None: raise RuntimeError('Preview barrier not reached')
                        time.sleep(.01)
                    os.kill(pid, signal.SIGUSR1)
                out, error = process.communicate(timeout=12)
                if process.returncode or 'SFTP PREVIEW PASS' not in out:
                    print(prefix + out + error, flush=True)
                    raise RuntimeError('FAIL: SFTP preview ' + name)
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
                    elif action in ('preview-growth','preview-shrink','preview-overflow','preview-read-error'):
                        expected=steps[:-1]
                    elif name in ('late-cancel','late-timeout'):
                        expected=steps[:-1]+['preview-after-data']
                    else:
                        expected=steps[:steps.index(stage)+1]
                    if unique != expected: raise RuntimeError(f'FAIL: {name} events {unique!r}, expected {expected!r}')

            finally:
                if stage: control(op='release', user=username)
                if process.poll() is None: process.terminate(); process.communicate(timeout=25)
            print('PASS SFTP preview ' + name, flush=True)
    print('SFTP PREVIEW PROTOCOL PASS', flush=True)


if __name__ == '__main__': main()
