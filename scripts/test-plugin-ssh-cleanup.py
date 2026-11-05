#!/usr/bin/env python3
"""P0 owned SSH fixture interruption audit; never signals user processes."""
import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
from types import SimpleNamespace
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
spec = importlib.util.spec_from_file_location('plugin_ssh_probe', ROOT / 'scripts/test-plugin-ssh.py')
ssh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ssh)


def processes():
    result = subprocess.run(['ps', '-eo', 'pid,ppid,state,args'], capture_output=True, text=True, check=True)
    return {int(parts[0]): (int(parts[1]), parts[2], parts[3]) for row in result.stdout.splitlines()[1:]
            if len(parts := row.split(None, 3)) == 4 and not parts[2].startswith('Z')}


def propagation():
    # The live audit below tests real signals. These two focused checks cover
    # the previously swallowed exception branches deterministically as well.
    client = object.__new__(ssh.bridge.Client)
    client.fd, client.output = 0, b''
    with patch.object(ssh.bridge.select, 'select', return_value=([0], [], [])), \
         patch.object(ssh.bridge.os, 'read', side_effect=InterruptedError('fixture stop')):
        try:
            client.drain()
        except InterruptedError:
            pass
        else:
            raise AssertionError('PTY drain swallowed interruption')
    write = SimpleNamespace(write=lambda _: (_ for _ in ()).throw(InterruptedError('fixture stop')))
    process = SimpleNamespace(poll=lambda: None, stdin=write)
    try:
        ssh.pipe_request(process, b'probe')
    except InterruptedError:
        pass
    else:
        raise AssertionError('pipe writer collapsed interruption into business outcome')
    print('PASS interruption propagation: PTY read and exec write', flush=True)


def live_interrupt(number):
    before = set((ROOT / 'tmp').glob('plugin-ssh-*'))
    log_path = ROOT / 'tmp' / ('plugin-ssh-interrupt-' + number.name + '.log')
    with log_path.open('wb') as log:
        process = subprocess.Popen([sys.executable, str(ROOT / 'scripts/test-plugin-ssh.py')],
                                   stdout=log, stderr=log, start_new_session=True)
        tracked = set()
        try:
            directory = None
            end = time.monotonic() + 45
            while time.monotonic() < end and process.poll() is None:
                for candidate in set((ROOT / 'tmp').glob('plugin-ssh-*')) - before:
                    if candidate.is_dir() and (candidate / 'ssh-ssh-direct.ready').exists():
                        directory = candidate
                        break
                if directory is not None:
                    break
                time.sleep(0.005)
            ssh.require(directory is not None and process.poll() is None, 'live nested SSH receiver was not observed')
            # Freeze only this owned runner while inspecting its actual process
            # tree. Real SSH receivers and agents remain live; no lock file or
            # stale pid is used as the proof of liveness.
            os.kill(process.pid, signal.SIGSTOP)
            table = processes()
            ssh.require(process.pid in table, 'owned runner exited before interruption')
            tracked = {process.pid}
            while True:
                expanded = tracked | {pid for pid, (parent, _, _) in table.items() if parent in tracked}
                if expanded == tracked:
                    break
                tracked = expanded
            ssh.require(any(str(directory / 'ssh-ssh-direct') in table[pid][2] and '--peer' in table[pid][2]
                            for pid in tracked if pid in table), 'owned SSH exec receiver is no longer live')
            ssh.require(len(tracked) >= 7, 'owned agent / sshd / receiver process tree missing')
            os.kill(process.pid, number)
            os.kill(process.pid, signal.SIGCONT)
            ssh.require(process.wait(timeout=15) != 0, 'interrupted SSH probe claimed success')
            ssh.require(not directory.exists(), 'owned fixture directory survived interruption')
            end = time.monotonic() + 5
            while time.monotonic() < end:
                live = tracked & processes().keys()
                if not live:
                    break
                time.sleep(0.02)
            ssh.require(not live, 'owned descendants survived interruption: ' + repr(live))
            print('PASS', number.name, 'live SSH/agents stopped; owned directory removed', flush=True)
        except Exception:
            print(log_path.read_text(errors='replace')[-5000:], file=sys.stderr)
            raise
        finally:
            if process.poll() is None:
                os.kill(process.pid, signal.SIGCONT)
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGKILL)
                    process.wait()


if __name__ == '__main__':
    def interrupted(number, _frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    (ROOT / 'tmp').mkdir(exist_ok=True)
    propagation()
    for number in (signal.SIGINT, signal.SIGTERM):
        live_interrupt(number)
