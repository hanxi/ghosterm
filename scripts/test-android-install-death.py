#!/usr/bin/env python3
"""Explicit owned-device install/maintenance death checkpoints, never assertion red."""
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
import time
import uuid

ROOT = Path(__file__).resolve().parents[1]


def main():
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set the explicitly owned ANDROID_SERIAL')
    project_tmp = ROOT / 'tmp'
    project_tmp.mkdir(exist_ok=True)
    scratch = Path(os.environ.get('TMPDIR', project_tmp)).resolve()
    if not scratch.is_relative_to(project_tmp) or not scratch.is_dir():
        scratch = project_tmp
    with tempfile.TemporaryDirectory(prefix='installation-death-', dir=scratch) as directory:
        execute(serial, Path(directory))
    assert not Path(directory).exists()


def execute(serial, work):
    adb = [str(Path(os.environ.get('ANDROID_HOME', ROOT / 'build/android-toolchain/sdk')) / 'platform-tools/adb'), '-s', serial]
    def run(command, seconds=60):
        result = subprocess.run([*adb, *command], cwd=ROOT, capture_output=True, text=True, timeout=seconds)
        if result.returncode: raise RuntimeError(str(command) + '\n' + result.stdout + result.stderr)
        return result.stdout + result.stderr
    def instrument(method, token, phase):
        return ['shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.PluginInstallDeathTest#' + method,
            '-e', 'deathToken', token, '-e', 'deathPhase', phase, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner']
    def kill_owner(method, token, phase, label):
        with (work / (label + '-owner.log')).open('w+') as log:
            child = subprocess.Popen([*adb, *instrument(method, token, phase)], cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                deadline = time.monotonic() + 30; marker = None
                while marker is None:
                    if child.poll() is not None: raise RuntimeError('Owner ended before checkpoint: ' + (work / (label + '-owner.log')).read_text())
                    result = subprocess.run([*adb, 'exec-out', 'run-as', 'com.hanxi.ghosterm', 'cat', 'cache/plugin-install-death-' + token + '.json'], capture_output=True, text=True, timeout=5)
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
    phases = ['after-reservation', 'after-directory-create', 'after-directory-record', 'after-file-create',
        'after-file-write', 'before-rename', 'after-rename', 'after-publication-sync', 'after-stage-unlink']
    for phase in phases:
        token = uuid.uuid4().hex
        if phase == 'after-stage-unlink':
            first = kill_owner('waitAtRealInstallationCheckpointForExternalProcessDeath', token, 'before-rename', phase + '-setup')
            marker = kill_owner('waitAtStageRemovalCheckpointForExternalProcessDeath', token, phase, phase)
            if first['pid'] == marker['pid']: raise RuntimeError('New removal owner reused setup PID')
        else: marker = kill_owner('waitAtRealInstallationCheckpointForExternalProcessDeath', token, phase, phase)
        observed = run(instrument('freshStartupObservesHeldNamespaceWithoutReplayThenVerifiesAndCleans', token, phase))
        (work / (phase + '-observed.log')).write_text(observed)
        if 'OK (1 test)' not in observed or 'FAILURES' in observed: raise RuntimeError('Restart maintenance failed:\n' + observed)
        control = run(['shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.NativeSmokeTest#corePreservesUtf8AndLineEndings', 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        (work / (phase + '-control.log')).write_text(control)
        if 'OK (1 test)' not in control or 'FAILURES' in control: raise RuntimeError('Independent Core control failed')
        receipts.append(dict(phase=phase, pid=marker['pid'], held_names=marker['names'], actual_process_death=True,
            restart_no_replay=True, production_maintenance=(phase != 'after-directory-create'), unconfirmed_preserved=(phase == 'after-directory-create'), core_control=True))
        (work / 'accepted-results.json').write_text(json.dumps(receipts, indent=2) + '\n')
        print(phase + ': actual owned PID death / startup full tree unchanged / explicit maintenance or unconfirmed preservation / Core GREEN', flush=True)
    print('ANDROID INSTALL DEATH PASS: ' + str(len(receipts)) + ' scenarios; 10 actual owned PID deaths', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt('signal ' + str(signum))
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    main()
