#!/usr/bin/env python3
"""Explicit owned-device install death checkpoints; never a normal assertion red."""
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
    work = ROOT / 'tmp/android-install-death' / str(time.time_ns()); work.mkdir(parents=True)
    def run(command, seconds=60):
        result = subprocess.run([*adb, *command], cwd=ROOT, capture_output=True, text=True, timeout=seconds)
        if result.returncode: raise RuntimeError(str(command) + '\n' + result.stdout + result.stderr)
        return result.stdout + result.stderr
    def instrument(method, token, phase):
        return ['shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.PluginInstallDeathTest#' + method,
            '-e', 'deathToken', token, '-e', 'deathPhase', phase, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner']
    receipts = []
    for phase in ['after-reservation', 'after-directory-record', 'after-file-create', 'before-rename', 'after-rename', 'after-publication-sync']:
        token = uuid.uuid4().hex
        with (work / (phase + '-owner.log')).open('w+') as log:
            child = subprocess.Popen([*adb, *instrument('waitAtRealInstallationCheckpointForExternalProcessDeath', token, phase)],
                cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + 30; marker = None
                while marker is None:
                    if child.poll() is not None: raise RuntimeError('Owner ended before checkpoint: ' + (work / (phase + '-owner.log')).read_text())
                    result = subprocess.run([*adb, 'exec-out', 'run-as', 'com.hanxi.ghosterm', 'cat', 'cache/plugin-install-death-' + token + '.json'], capture_output=True, text=True, timeout=5)
                    if result.returncode == 0:
                        try: marker = json.loads(result.stdout)
                        except json.JSONDecodeError: pass
                    if time.monotonic() > deadline: raise RuntimeError('Same owner did not reach checkpoint')
                    time.sleep(.05)
                if marker['token'] != token or marker['phase'] != phase: raise RuntimeError('Unexpected owner marker')
                pids = run(['shell', 'pidof', 'com.hanxi.ghosterm']).split()
                if str(marker['pid']) not in pids: raise RuntimeError('Checkpoint owner is not live')
                run(['shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
                child.wait(timeout=15)
                check = subprocess.run([*adb, 'shell', 'pidof', 'com.hanxi.ghosterm'], capture_output=True, text=True, timeout=5)
                if str(marker['pid']) in check.stdout.split(): raise RuntimeError('Owned PID survived force-stop')
                observed = run(instrument('freshStartupObservesHeldNamespaceWithoutReplayThenVerifiesAndCleans', token, phase))
                (work / (phase + '-observed.log')).write_text(observed)
                if 'OK (1 test)' not in observed or 'FAILURES' in observed: raise RuntimeError('Restart observation failed:\n' + observed)
                control = run(['shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings', 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                (work / (phase + '-control.log')).write_text(control)
                if 'OK (1 test)' not in control or 'FAILURES' in control: raise RuntimeError('Independent control failed')
                receipts.append(dict(phase=phase, pid=marker['pid'], held_names=marker['names'], actual_process_death=True, restart_no_replay=True, verified_and_cleaned=True, core_control=True))
                (work / 'accepted-results.json').write_text(json.dumps(receipts, indent=2) + '\n')
                print(phase + ': actual owned PID death / startup namespace retained / verified-cleaned / Core GREEN', flush=True)
            finally:
                if child.poll() is None:
                    run(['shell', 'am', 'force-stop', 'com.hanxi.ghosterm']); child.wait(timeout=15)
    print('ANDROID INSTALL DEATH PASS: ' + str(len(receipts)) + ' checkpoints', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
