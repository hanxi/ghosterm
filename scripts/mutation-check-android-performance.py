#!/usr/bin/env python3
"""Verify the performance probe observes scheduling, actual pixels, metrics and IME input."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_PERFORMANCE_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial: raise RuntimeError('Set ANDROID_SERIAL to a test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    view = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    probe = ROOT / 'android/app/src/androidTest/java/com/hanxi/ghosterm/TerminalPerformanceTest.kt'
    originals = {p: p.read_bytes() for p in (view, probe)}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('mutation marker present')
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    target = 'TerminalPerformanceTest#recordRealTerminalBaseline'
    cases = [
        (view, 'postOnAnimation(tick)', 'postDelayed(tick, 5_000)', target),
        (view, 'fill.color = if (cell.flags and 256 != 0) Color.rgb(68, 91, 125) else cell.background',
         'fill.color = Color.BLACK', target),
        (probe, 'timings.getOrPut(phase) { mutableListOf() }.add(metric.getMetric(FrameMetrics.TOTAL_DURATION))',
         'timings.getOrPut(phase) { mutableListOf() }; /* omit measurement */', target),
        (view, 'this@TerminalView.commitText(text.toString())', 'true', target),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if any(n < 1 or n > len(cases) for n in selected): raise RuntimeError('case out of range')
    work = ROOT / 'tmp/android-performance-mutations'; work.mkdir(exist_ok=True)
    def build(label):
        result = helper.run(['bash', ROOT / 'scripts/build-android.sh'], 900)
        (work / (label + '-build.log')).write_text(result.stdout + result.stderr)
        helper.require(result)  # Compile failures never count as red.
        for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            helper.require(helper.run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name]))
    def test(name, green, label):
        result = helper.run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + name, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], 120)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        passed = result.returncode == 0 and 'OK (1 test)' in output
        assertion = any(kind in output for kind in ('AssertionError', 'org.junit.ComparisonFailure', 'ComposeTimeoutException'))
        if passed != green or (not green and (result.returncode != 0 or not assertion or 'FAILURES' not in output)):
            raise RuntimeError(output)
    build('baseline')
    for name in dict.fromkeys(cases[n - 1][3] for n in selected): test(name, True, 'baseline-' + name)
    try:
        for number in selected:
            path, before, after, target = cases[number - 1]
            label = f'P{number}'
            source = originals[path].decode()
            if source.count(before) != 1: raise RuntimeError(f'{label}: mutation anchor missing/ambiguous')
            path.write_text(source.replace(before, after + ' /* ' + MARK + ' */'))
            build(label)
            test(target, False, label + '-target')
            test('NativeSmokeTest#corePreservesUtf8AndLineEndings', True, label + '-control')
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[path]: raise RuntimeError('restore SHA256 mismatch')
            build(label + '-restored')
            test(target, True, label + '-restored')
            print(f'{label}: target red / same APK control green / restored green / SHA256 identical', flush=True)
    finally:
        for path, content in originals.items():
            if MARK.encode() in path.read_bytes(): path.write_bytes(content)
        if any(hashlib.sha256(p.read_bytes()).digest() != h for p, h in hashes.items()):
            raise RuntimeError('source changed during mutations')
    print(f'ANDROID PERFORMANCE MUTATIONS PASS: {len(selected)} groups')


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
