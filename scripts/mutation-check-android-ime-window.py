#!/usr/bin/env python3
"""Prove IME window, transactional replacement and finalized-byte ownership gates."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import signal
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_IME_WINDOW_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', type=int, action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to an API 31+ test device')
    keyboard_serial = os.environ.get('ANDROID_GBOARD_SERIAL', serial)
    devices = list(dict.fromkeys((serial, keyboard_serial)))
    adb = {device: ['adb', '-s', device] for device in devices}
    for device in devices:
        helper.require(helper.run([*adb[device], 'get-state']))
    api = helper.run([*adb[serial], 'shell', 'getprop', 'ro.build.version.sdk'])
    helper.require(api)
    if int(api.stdout.strip()) < 31:
        raise RuntimeError('Surrounding-text mutation targets require API 31+')
    path = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    original = path.read_bytes()
    mode = path.stat().st_mode & 0o777
    digest = hashlib.sha256(original).hexdigest()
    if MARK.encode() in original:
        raise RuntimeError('Residual IME window mutation')
    ui = 'TerminalScreenTest#'
    coordinates = ui + 'imeWindowUsesAcceptedUtf16OffsetsForSelectionAndRegions'
    selection = ui + 'imeFinishPreservesSelectionInsideTheAcceptedText'
    partial = ui + 'imePartialCommitPreservesTextOutsideTheComposingRegion'
    positions = ui + 'imeCursorPositionsKeepAnEditableTailUntilAcceptance'
    rejected = ui + 'imeRejectedCommitKeepsTheOriginalEditableAndRetry'
    tail = ui + 'imePlainSuffixIsDeliveredOnceWhenTheConnectionCloses'
    finalized = ui + 'imeFinalizedSelectionCannotReplayOrRewriteAcceptedBytes'
    recovery = ui + 'terminalKeysReleaseReadOnlyImeContextForFurtherInput'
    deletion = ui + 'imeSurroundingDeletePreservesReversedSelection'
    cases = [
        ('notification-offset', 'if (index < 0) -1 else origin + index',
         'if (index < 0) -1 else index', 'SystemImeProductTest#systemKeyboardProducesTheExactCommandFile', keyboard_serial),
        ('accepted-offset', 'origin += cursor', 'origin += 0', coordinates, serial),
        ('surrounding-offset', 'local.selectionEnd, origin + local.offset)',
         'local.selectionEnd, local.offset)', coordinates, serial),
        ('region-offset', '(index.toLong() - origin).coerceIn',
         '(index.toLong()).coerceIn', coordinates, serial),
        ('finish-selection', 'compactFinalized(); changed(); return true',
         'compactFinalized(force = true); changed(); return true', selection, serial),
        ('finish-marker', 'super.finishComposingText()', 'Unit', selection, serial),
        ('finalized-replay', 'if (!finalized && accepted.isNotEmpty() && !this@TerminalView.commitText(accepted)) return false',
         'if (accepted.isNotEmpty() && !this@TerminalView.commitText(accepted)) return false', finalized, serial),
        ('replacement-sides', 'val accepted = candidate.subSequence(delivered, cursor).toString()',
         'val accepted = text.toString()', partial, serial),
        ('replacement-region', 'editor.setComposingRegion(composingStart, composingEnd)',
         'editor.setComposingRegion(0, 1)', partial, serial),
        ('cursor-position', 'editor.commitText(text, position)',
         'editor.commitText(text, 1)', positions, serial),
        ('cursor-offset', 'origin += cursor', 'origin += candidate.length', positions, serial),
        ('candidate-rejection', 'if (!validInputText(candidate)) return false', 'Unit', rejected, serial),
        ('direct-rejection', 'if (!validInputText(text)) return false', 'Unit', rejected, serial),
        ('plain-tail', 'if (!finalized && getComposingSpanStart(pending) < 0 && pending.isNotEmpty())',
         'if (false && !finalized && getComposingSpanStart(pending) < 0 && pending.isNotEmpty())', tail, serial),
        ('cancel-replay', 'if (!finalized && getComposingSpanStart(pending) < 0 && pending.isNotEmpty())',
         'if (getComposingSpanStart(pending) < 0 && pending.isNotEmpty())', finalized, serial),
        ('auxiliary-recovery', 'if (sent) connection?.terminalKeyAccepted()', 'Unit', recovery, serial),
        ('hardware-recovery', 'if (!modifierKey) connection?.terminalKeyAccepted()', 'Unit', recovery, serial),
        ('delete-selection', 'Selection.setSelection(pending, first - removedBefore, last - removedBefore)',
         'Selection.setSelection(pending, left)', deletion, serial),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if len(selected) != len(set(selected)) or any(number < 1 or number > len(cases) for number in selected):
        raise RuntimeError('Invalid or duplicate mutation case')
    for number in selected:
        if original.decode().count(cases[number - 1][1]) != 1:
            raise RuntimeError(f'W{number}: missing or ambiguous source anchor')
    work = ROOT / 'tmp/android-ime-window-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    (work / 'original.kt').write_bytes(original)

    def write_source(data):
        temporary = work / ('source-' + str(time.time_ns()) + '.kt')
        temporary.write_bytes(data)
        temporary.chmod(mode)
        os.replace(temporary, path)

    def restore():
        if MARK.encode() in path.read_bytes():
            write_source(original)
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('IME window source restore SHA256 mismatch')

    def run_logged(label, command, seconds=120):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        run_logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for device in devices:
            for name in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
                run_logged(label + '-install-' + device + '-' + Path(name).name,
                           [*adb[device], 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / name])

    def test(label, method, device, green):
        output = run_logged(label, [*adb[device], 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
            'com.hanxi.ghosterm.' + method, 'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        if green:
            good = 'OK (1 test)' in output and 'FAILURES' not in output
        else:
            good = bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and any(
                error in output for error in ('java.lang.AssertionError', 'org.junit.ComparisonFailure'))
            # Language, keyboard readiness and stock-editor failures do not prove
            # that a production terminal mutation was observed.
            good = good and not any(message in output for message in (
                'System keyboard display precondition:',
                'System IME command driver must preserve text in an independent stock editor expected:',
                'Stock editor control also preserves the second line expected:',
            ))
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    try:
        build('baseline')
        targets = list(dict.fromkeys((cases[number - 1][3], cases[number - 1][4]) for number in selected))
        for index, (target, device) in enumerate(targets):
            test('baseline-' + str(index), target, device, True)
        for number in selected:
            name, before, after, target, device = cases[number - 1]
            label = f'W{number:02d}-{name}'
            write_source((original.decode().replace(before, after) + '\n// ' + MARK + '\n').encode())
            build(label)
            test(label + '-target', target, device, False)
            test(label + '-control', 'ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', device, True)
            restore()
            build(label + '-restored')
            test(label + '-restored', target, device, True)
            print(label + ': compiled single assertion red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for device in devices:
            result = helper.run([*adb[device], 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
            if result.returncode:
                print(result.stdout + result.stderr, flush=True)
        restore()
    print(f'ANDROID IME WINDOW MUTATIONS PASS: {len(selected)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    main()
