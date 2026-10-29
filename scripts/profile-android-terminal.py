#!/usr/bin/env python3
"""Collect a real Android PTY/VT/Canvas debug baseline on an explicitly selected test device.

Reports process/instrumentation costs, not production release or device-independent budgets.
Artifacts stay in the repository's ignored tmp/android-performance/ directory.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def distribution(values):
    ordered = sorted(values)
    if not ordered:
        raise RuntimeError('Missing samples; cannot report a zero duration')
    return {'count': len(ordered), 'p50': ordered[math.ceil(len(ordered) * .50) - 1],
            'p95': ordered[math.ceil(len(ordered) * .95) - 1], 'max': ordered[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', default='baseline')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,40}', args.label):
        parser.error('label must contain 1–40 ASCII letters, digits, _ or -')
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        parser.error('Set ANDROID_SERIAL to the intended test device')
    adb_binary = shutil.which('adb') or str(ROOT / 'build/android-toolchain/sdk/platform-tools/adb')
    adb = [adb_binary, '-s', serial]
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    work = ROOT / 'tmp/android-performance' / f'{stamp}-{args.label}-{uuid.uuid4().hex[:6]}'
    work.mkdir(parents=True)

    def run(command, label, seconds=300):
        result = helper.run(command, seconds)
        (work / f'{label}.log').write_text(result.stdout + result.stderr)
        helper.require(result)
        return result.stdout

    run([*adb, 'get-state'], 'device')
    run(['bash', ROOT / 'scripts/build-android.sh'], 'build', 900)
    apks = [ROOT / 'android/app/build/outputs' / relative for relative in
            ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk')]
    for index, apk in enumerate(apks):
        run([*adb, 'install', '-r', '-t', apk], f'install-{index}')
    print(f'Collecting {args.label} on {serial}; logs: {work}', flush=True)
    output = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                  'com.hanxi.ghosterm.TerminalPerformanceTest',
                  'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], 'instrumentation', 180)
    if 'OK (1 test)' not in output:
        raise RuntimeError(f'Measurement did not complete; see {work}/instrumentation.log')
    raw = run([*adb, 'exec-out', 'run-as', 'com.hanxi.ghosterm', 'cat', 'files/terminal-performance.json'], 'device-report')
    report = json.loads(raw)
    if report.get('schema') != 1 or report.get('complete') is not True:
        raise RuntimeError('Incomplete or unknown measurement report')
    report['host'] = {
        'git_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        'git_dirty': bool(subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)),
        'apk_sha256': {apk.name: hashlib.sha256(apk.read_bytes()).hexdigest() for apk in apks},
        'emulator_property': run([*adb, 'shell', 'getprop', 'ro.kernel.qemu'], 'emulator').strip(),
    }
    (work / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2))
    frames = {}
    for name, values in report['window_frames'].items():
        frames[name] = {'total_ms': distribution([x / 1e6 for x in values['total_duration_ns']]),
                        'draw_ms': distribution([x / 1e6 for x in values['draw_duration_ns']]),
                        'callback_dropped': values['callback_dropped']}
    summary = {
        'scope': 'debug app plus instrumentation; frame metrics cover the window; pixel latency includes capture/poll overhead',
        'device': {key: report[key] for key in ('device', 'model', 'sdk', 'abis', 'page_size', 'viewport')},
        'burst': report['burst'], 'sustained': report['sustained'], 'window_frames': frames,
        'input_to_window_pixel_ms': distribution(report['input_to_window_pixel_ms']),
        'pixel_poll_calibration_ms': report['pixel_poll_calibration_ms'], 'input_timeout_ms': report['input_timeout_ms'],
        'peak_sampled_pss_kib': max(x['pss_kib'] for x in report['memory_samples']),
        'peak_sampled_native_bytes': max(x['native_allocated_bytes'] for x in report['memory_samples']),
        'foreground_idle': report['foreground_idle'], 'background_idle': report['background_idle'],
        'close_request_ms': report['close_request_ms'], 'close_owner_released_ms': report['close_owner_released_ms'],
    }
    (work / 'summary.json').write_text(json.dumps(summary, ensure_ascii=False, indent=2))
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    print(f'Raw samples and APK identity: {work}/report.json')


if __name__ == '__main__':
    main()
