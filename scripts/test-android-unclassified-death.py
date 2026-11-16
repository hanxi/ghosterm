#!/usr/bin/env python3
"""Explicit owned-device unknown-directory deletion death checkpoints, never assertion red."""
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set the explicitly owned ANDROID_SERIAL')
    adb = [str(Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk')) / 'platform-tools/adb'), '-s', serial]
    work = ROOT / 'tmp/android-unclassified-death' / str(time.time_ns()); work.mkdir(parents=True)
    def run(command, seconds=60):
        result = subprocess.run([*adb, *command], cwd=ROOT, capture_output=True, text=True, timeout=seconds)
        if result.returncode: raise RuntimeError(str(command) + '\n' + result.stdout + result.stderr)
        return result.stdout + result.stderr
    def instrument(method, token, phase):
        return ['shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.PluginUnclassifiedDeathTest#' + method,
            '-e', 'deathToken', token, '-e', 'deathPhase', phase, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner']
    def kill_owner(method, token, phase, label):
        with (work / (label + '-owner.log')).open('w+') as log:
            child = subprocess.Popen([*adb, *instrument(method, token, phase)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + 30; marker = None
                while marker is None:
                    if child.poll() is not None: raise RuntimeError('Owner ended before checkpoint: ' + (work / (label + '-owner.log')).read_text())
                    result = subprocess.run([*adb, 'exec-out', 'run-as', 'com.hanxi.ghosterm', 'cat', 'cache/plugin-unclassified-death-' + token + '.json'], capture_output=True, text=True, timeout=5)
                    if result.returncode == 0:
                        try: candidate = json.loads(result.stdout)
                        except json.JSONDecodeError: candidate = None
                        if candidate and candidate['token'] == token and candidate['phase'] == phase: marker = candidate
                    if time.monotonic() > deadline: raise RuntimeError('Same owner did not reach checkpoint')
                    time.sleep(.05)
                if str(marker['pid']) not in run(['shell', 'pidof', 'com.hanxi.ghosterm']).split(): raise RuntimeError('Checkpoint PID not live')
                run(['shell', 'am', 'force-stop', 'com.hanxi.ghosterm']); child.wait(timeout=15)
                check = subprocess.run([*adb, 'shell', 'pidof', 'com.hanxi.ghosterm'], capture_output=True, text=True, timeout=5)
                if str(marker['pid']) in check.stdout.split(): raise RuntimeError('Owned PID survived force-stop')
                return marker
            finally:
                if child.poll() is None: run(['shell', 'am', 'force-stop', 'com.hanxi.ghosterm']); child.wait(timeout=15)
    receipts = []
    phases = ['before-intent', 'after-intent', 'after-detach', 'after-unlink', 'before-clear']
    for phase in phases:
        token = uuid.uuid4().hex
        marker = kill_owner('waitAtRealUnknownRemovalCheckpointForExternalDeath', token, phase, phase)
        observed = run(instrument('freshStartupDoesNotReplayThenExplicitUnknownMaintenanceCompletes', token, phase))
        (work / (phase + '-observed.log')).write_text(observed)
        if 'OK (1 test)' not in observed or 'FAILURES' in observed: raise RuntimeError('Restart maintenance failed:\n' + observed)
        control = run(['shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings', 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        (work / (phase + '-control.log')).write_text(control)
        if 'OK (1 test)' not in control or 'FAILURES' in control: raise RuntimeError('Independent Core control failed')
        receipts.append(dict(phase=phase, pid=marker['pid'], held_names=marker['names'], actual_process_death=True,
            restart_no_replay=True, production_maintenance=True, old_ownership_preserved=True, core_control=True))
        (work / 'accepted-results.json').write_text(json.dumps(receipts, indent=2) + '\n')
        print(phase + ': actual owned PID death / startup full tree unchanged / explicit unknown cleanup / separate metadata retirement / Core GREEN', flush=True)
    print('ANDROID UNKNOWN DEATH PASS: ' + str(len(receipts)) + ' scenarios; 5 actual owned PID deaths', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
