#!/usr/bin/env python3
"""Accepted IME composition reaches the real PTY once; cancellation stays distinct."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import signal
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_IME_FINISH_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the intended test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    path = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    original = path.read_bytes()
    digest = hashlib.sha256(original).digest()
    if MARK.encode() in original:
        raise RuntimeError('Residual IME finish mutation')
    finish = 'TerminalScreenTest#finishingImeCompositionPreservesBytesOnceAndClosingStillCancels'
    enter = 'TerminalScreenTest#imeEnterAndEditorActionAcceptPendingTextBeforeTheReturn'
    line_feed = 'TerminalScreenTest#imeLineFeedAcceptsPendingCompositionBeforeReturn'
    accepted = 'if (accepted.isNotEmpty() && !this@TerminalView.commitText(accepted)) return false'
    cases = [
        (accepted, 'if (accepted.isNotEmpty()) Unit', finish),
        (accepted, 'if (accepted.isNotEmpty() && !this@TerminalView.commitText("\\u001b[200~" + accepted + "\\u001b[201~")) return false', finish),
        ('fun cancel() { valid = false;', 'fun cancel() { this@TerminalView.commitText(pending.toString()); valid = false;', finish),
        ('if (event.action == KeyEvent.ACTION_DOWN && event.keyCode == KeyEvent.KEYCODE_ENTER && !finishComposingText()) return false', 'Unit', enter),
        ('override fun performEditorAction(action: Int): Boolean = valid && finishComposingText() && sendKey(KeyEvent.KEYCODE_ENTER)',
         'override fun performEditorAction(action: Int): Boolean = valid && sendKey(KeyEvent.KEYCODE_ENTER)', enter),
        ('override fun finishComposingText(): Boolean {\n            if (!valid) return false',
         'override fun finishComposingText(): Boolean {\n            if (false && !valid) return false', finish),
        ('val sent = if (text.toString() == "\\n") finishComposingText() && sendKey(KeyEvent.KEYCODE_ENTER)',
         'val sent = if (text.toString() == "\\n") sendKey(KeyEvent.KEYCODE_ENTER)', line_feed),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if any(n < 1 or n > len(cases) for n in selected):
        raise RuntimeError('case out of range')
    work = ROOT / 'tmp/android-ime-finish-mutations' / str(time.time_ns())
    work.mkdir(parents=True)

    def build(label):
        result = helper.run(['bash', ROOT / 'scripts/build-android.sh'], 900)
        (work / (label + '-build.log')).write_text(result.stdout + result.stderr)
        helper.require(result)
        for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            helper.require(helper.run([*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name]))

    def test(name, green, label):
        result = helper.run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + name, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], 120)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        if green:
            good = 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            good = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                kind in output for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    try:
        build('baseline')
        for number, name in enumerate((finish, enter, line_feed, 'TerminalScreenTest#imeCompositionCancellationAndDeleteReachRealPtyCorrectly')):
            test(name, True, f'baseline-{number}')
        for number in selected:
            before, after, target = cases[number - 1]
            source = original.decode()
            if source.count(before) != 1:
                raise RuntimeError(f'F{number}: missing or ambiguous mutation anchor')
            path.write_text(source.replace(before, after + ' /* ' + MARK + ' */'))
            label = f'F{number}'
            build(label)
            test(target, False, label + '-target')
            test('ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True, label + '-control')
            path.write_bytes(original)
            if hashlib.sha256(path.read_bytes()).digest() != digest:
                raise RuntimeError('IME finish restore SHA256 mismatch')
            build(label + '-restored')
            test(target, True, label + '-restored')
            print(label + ': compiled single target assertion red / same APK config green / restored green / SHA256', flush=True)
    finally:
        if MARK.encode() in path.read_bytes():
            path.write_bytes(original)
        if hashlib.sha256(path.read_bytes()).digest() != digest:
            raise RuntimeError('IME finish source changed during verification')
        helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print(f'ANDROID IME FINISH MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
