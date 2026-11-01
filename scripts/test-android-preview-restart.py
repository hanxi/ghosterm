#!/usr/bin/env python3
"""Real foreign descriptor across target-process death; optional compiled mutation + Core control."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_PREVIEW_RESTART_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the isolated debug test device')
    adb = ['adb', '-s', serial]
    work = ROOT / 'tmp/android-preview-restart' / str(time.time_ns())
    work.mkdir(parents=True)
    print('PREVIEW RESTART RECEIPTS', work, flush=True)
    cache = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/ExternalPreviewCache.kt'
    original = cache.read_bytes()
    mode = cache.stat().st_mode & 0o777
    if MARK.encode() in original:
        raise RuntimeError('Residual preview restart mutation')
    (work / cache.name).write_bytes(original)

    def run(label, command, seconds=60, okay=True):
        result = helper.run(command, seconds)
        (work / (label + '.log')).write_text(result.stdout + result.stderr)
        if okay:
            helper.require(result)
        return result

    def write_source(data):
        temporary = work / ('restore-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(data)
        temporary.chmod(mode)
        os.replace(temporary, cache)

    def restore():
        if MARK.encode() in cache.read_bytes():
            write_source(original)
        if cache.read_bytes() != original:
            raise RuntimeError('Preview restart source SHA256 restore mismatch')

    def build(label):
        run(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for name, package in [('apk/debug/app-debug.apk', 'com.hanxi.ghosterm'),
                              ('apk/androidTest/debug/app-debug-androidTest.apk', 'com.hanxi.ghosterm.test')]:
            apk = ROOT / 'android/app/build/outputs' / name
            run(label + '-install-' + package, [*adb, 'install', '-r', '-t', apk], 120)
            path = run(label + '-path-' + package, [*adb, 'shell', 'pm', 'path', package]).stdout.strip().removeprefix('package:')
            installed = run(label + '-sha-' + package, [*adb, 'shell', 'sha256sum', path]).stdout.split()[0]
            if installed != hashlib.sha256(apk.read_bytes()).hexdigest():
                raise RuntimeError('Installed APK does not match build: ' + package)

    def control(label):
        output = run(label + '-core-control', [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig',
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner']).stdout
        if 'OK (1 test)' not in output or 'FAILURES' in output:
            raise RuntimeError('Independent Core control did not pass')

    def foreign(label, method, report):
        return run(label, [*adb, 'shell', 'content', 'call', '--uri',
            'content://com.hanxi.ghosterm.test.previewcontrol', '--method', method, '--arg', report]).stdout

    def probe(label):
        report = None
        runner = None
        log = (work / (label + '-checkpoint.log')).open('w')
        try:
            runner = subprocess.Popen([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                'com.hanxi.ghosterm.ExternalPreviewRestartTest', '-e', 'externalPreviewHold', 'true',
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 60
            while True:
                output = (work / (label + '-checkpoint.log')).read_text()
                report_match = re.search(r'INSTRUMENTATION_STATUS: preview_report=([0-9a-f-]{36})', output)
                token_match = re.search(r'INSTRUMENTATION_STATUS: preview_token=([0-9a-f-]{36})', output)
                if report_match and token_match:
                    if runner.poll() is not None:
                        raise RuntimeError('Checkpoint producer already exited; not a live process boundary')
                    break
                if runner.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Missing live checkpoint identities: ' + output)
                time.sleep(.05)
            report, token = report_match[1], token_match[1]
            before = foreign(label + '-foreign-before', 'readHeld', report)
            if 'readable=true' not in before or 'held=true' not in before:
                raise RuntimeError('Receiver did not retain a readable descriptor before stop: ' + before)
            receiver = re.search(r'pid=(\d+)', before)[1]
            old_pid = run(label + '-owner-before', [*adb, 'shell', 'pidof', 'com.hanxi.ghosterm']).stdout.strip()
            if not old_pid:
                raise RuntimeError('No live target owner before process stop')
            run(label + '-stop-owner', [*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
            ended = run(label + '-owner-after', [*adb, 'shell', 'pidof', 'com.hanxi.ghosterm'], okay=False)
            if ended.stdout.strip():
                raise RuntimeError('Target owner is still alive after force-stop')
            runner.wait(timeout=10)
            after = foreign(label + '-foreign-after', 'readHeldTail', report)
            if 'held=true' not in after or f'pid={receiver}' not in after:
                raise RuntimeError('Receiver itself died or lost its descriptor; invalid fixture: ' + after)
            if 'readable=true' in after:
                raise AssertionError('TARGET: receiver retained the raw cache inode after the owner process died')
            if 'readable=false' not in after or 'ErrnoException' not in after:
                raise RuntimeError('Expected an actual IO error for an unread tail from the dead proxy owner: ' + after)
            run(label + '-restart-owner', [*adb, 'shell', 'am', 'start', '-W', '-n', 'com.hanxi.ghosterm/.MainActivity'])
            deadline = time.monotonic() + 10
            while True:
                exists = run(label + '-owned-copy-exists', [*adb, 'shell', 'run-as', 'com.hanxi.ghosterm',
                    'test', '-e', 'cache/external-previews/' + token], okay=False)
                if exists.returncode == 1:
                    break
                helper.require(exists)
                if time.monotonic() >= deadline:
                    raise AssertionError('Expired owned copy was not swept after restart')
                time.sleep(.05)
            return dict(owner_pid=old_pid, foreign_pid=receiver, token=token,
                        owner_terminated=True, foreign_descriptor_held=True, read_after_death_failed=True,
                        expired_owned_copy_removed_on_restart=True)
        finally:
            if runner is not None and runner.poll() is None:
                run(label + '-cancel-checkpoint', [*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
                try: runner.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    runner.kill();runner.wait()
            log.close()
            if report:
                foreign(label + '-close-foreign', 'closeHeld', report)
            else:
                foreign(label + '-close-fixture', 'closeAllHeld', 'unused')
            run(label + '-restore-foreground', [*adb, 'shell', 'am', 'start', '-W', '-n', 'com.hanxi.ghosterm/.MainActivity'])

    try:
        build('baseline')
        baseline = probe('baseline')
        evidence = dict(passed=True, serial=serial, baseline=baseline, mutation=False,
                        source_sha256=hashlib.sha256(original).hexdigest())
        if args.mutate:
            anchor = '            val storage=checkNotNull(context.getSystemService(StorageManager::class.java))'
            if original.decode().count(anchor) != 1:
                raise RuntimeError('Missing unique proxy source anchor')
            changed = original.decode().replace(anchor,
                '            if(true) return ParcelFileDescriptor.dup(owned.fileDescriptor) // ' + MARK + '\n' + anchor)
            write_source(changed.encode())
            build('mutant')
            control('mutant')
            try:
                probe('mutant')
            except AssertionError as error:
                if not str(error).startswith('TARGET: receiver retained the raw cache inode'):
                    raise
                (work / 'mutant-target-red.txt').write_text(str(error) + '\n')
            else:
                raise RuntimeError('Raw-descriptor mutation did not turn the target assertion red')
            restore()
            build('restored')
            evidence.update(mutation=True, compiled_target_red=True, same_apk_core_green=True, restored=probe('restored'))
        (work / 'result.json').write_text(json.dumps(evidence, indent=2) + '\n')
    finally:
        restore()
    print('ANDROID PREVIEW RESTART PASS' + (' / compiled mutation red / Core green / restored green / SHA256' if args.mutate else ''), work, flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
