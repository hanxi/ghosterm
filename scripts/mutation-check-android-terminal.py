#!/usr/bin/env python3
"""Real Android PTY/Canvas/IME/lifecycle mutations; each red has an independent green."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_TERMINAL_MUTATION'
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
    native = ROOT / 'android/bridge/src/main/cpp/session_jni.c'
    surface = ROOT / 'terminal/android/native/surface.c'
    view = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    model = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/SessionsViewModel.kt'
    screen = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/TerminalScreen.kt'
    main_screen = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    originals = {p: p.read_bytes() for p in (native, surface, view, model, screen, main_screen)}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('mutation marker present')
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    local = 'LocalTerminalTest#'
    ui = 'TerminalScreenTest#'
    life = 'TerminalLifecycleTest#'
    ime = ui + 'imeCompositionCancellationAndDeleteReachRealPtyCorrectly'
    generation = local + 'staleNativeGenerationCannotAccessOrCloseReusedSlot'
    cases = [
        (surface, 'ghostty_terminal_vt_write(s->terminal, bytes, length);',
         'ghostty_terminal_vt_write(s->terminal, bytes, length * 0);', local + 'shellOutputRendersAnsiWideAndCombiningCells'),
        (surface, 'ghostty_terminal_resize(s->terminal, cols, rows, cw, ch)',
         'ghostty_terminal_resize(s->terminal, cols + 1, rows, cw, ch)', local + 'resizeReachesPtyAndExitStatusIsReal'),
        (native, 'gt_transport_resize(s->transport, cols, rows, (uint32_t)cols * cw, (uint32_t)rows * ch)',
         'gt_transport_resize(s->transport, cols + 1, rows, (uint32_t)cols * cw, (uint32_t)rows * ch)', local + 'resizeReachesPtyAndExitStatusIsReal'),
        (surface, 'ghostty_key_encoder_setopt_from_terminal(s->encoder, s->terminal);',
         '(void)s->encoder;', local + 'nativeKeyEncoderUsesApplicationCursorAndTypedUtf8'),
        (view, 'this@TerminalView.commitText(text.toString())',
         'this@TerminalView.commitText("\\u001b[200~" + text + "\\u001b[201~")', ime),
        (view, 'super.finishComposingText(); pending.clear();',
         'this@TerminalView.commitText(pending.toString()); super.finishComposingText(); pending.clear();', ime),
        (view, 'pending.delete(end, right); pending.delete(left, start)',
         'pending.delete(end, right); pending.delete(start, start)', ime),
        (view, 'fill.color = if (cell.flags and 256 != 0) Color.rgb(68, 91, 125) else cell.background', 'fill.color = Color.BLACK', ui + 'rendersRealAnsiPixelsAndHasOneKeyboardControl'),
        (model, 'fun home() { visible = false }',
         'fun home() { active?.let { close(it.id) }; visible = false }', ui + 'returningHomePreservesShellAndViewUsesActualSize'),
        (screen, 'menu = false; confirmClose = true',
         'menu = false; model.close(session.id)', ui + 'closeRequiresConfirmationAndSessionPanelCreatesIndependentSession'),
        (native, 'session *s = registry[slot].generation == generation ? registry[slot].value : NULL;\n    if (s) s->refs++;',
         'session *s = registry[slot].value; (void)generation;\n    if (s) s->refs++;', generation),
        (native, 'session *s = registry[slot].generation == generation ? registry[slot].value : NULL;\n    if (s) { registry[slot].value = NULL;',
         'session *s = registry[slot].value; (void)generation;\n    if (s) { registry[slot].value = NULL;', generation),
        (native, 'registry[slot].value = NULL; atomic_store(&s->stop, true);',
         'registry[slot].value = NULL; atomic_store(&s->stop, false);', local + 'closeUnderBackpressureReleasesNativeOwner'),
        (native, 's->exit_known = known; s->exit_code = code;',
         's->exit_known = known; s->exit_code = 0;', local + 'resizeReachesPtyAndExitStatusIsReal'),
        (native, 'if (stat(s->home, &home_stat) != 0 || !S_ISDIR(home_stat.st_mode) || access(s->home, X_OK) != 0)',
         'if (false && (stat(s->home, &home_stat) != 0 || !S_ISDIR(home_stat.st_mode) || access(s->home, X_OK) != 0))',
         local + 'unsupportedWorkingDirectoryFailsExplicitlyAndReleasesOwner'),
        (main_screen, 'sessions: SessionsViewModel = viewModel()',
         'sessions: SessionsViewModel = SessionsViewModel(androidx.compose.ui.platform.LocalContext.current.applicationContext as android.app.Application)',
         life + 'activityRecreationRetainsOwnerPidAndKeyboardResizesTheSamePty'),
        (view, 'override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) { updateSize() }',
         'override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) { if (oldh == 0) updateSize() }',
         life + 'activityRecreationRetainsOwnerPidAndKeyboardResizesTheSamePty'),
        (native, 'if (s->transport) length = gt_transport_read(s->transport, output, sizeof(output), had_input ? 0 : 20);',
         'if (s->transport && atomic_load(&s->frame_requested)) length = gt_transport_read(s->transport, output, sizeof(output), had_input ? 0 : 20);',
         life + 'returningFromBackgroundConsumesOutputWithoutReplacingTheSession'),
        (view, 'canvas.clipRect(0, 0, width, height)',
         'canvas.clipRect(-width, -height, width * 3, height * 3)', ui + 'canvasPaintingIsClippedToTerminalBounds'),
        (view, 'post { if (isAttachedToWindow && isShown) requestFocus() }',
         'post { clearFocus() }', life + 'newTerminalReceivesPhysicalKeysAndTopBackHidesImeBeforeLeaving'),
        (screen, 'navigationIcon = { TextButton(onClick = back, modifier = Modifier.testTag("terminal-back"))',
         'navigationIcon = { TextButton(onClick = model::home, modifier = Modifier.testTag("terminal-back"))',
         life + 'newTerminalReceivesPhysicalKeysAndTopBackHidesImeBeforeLeaving'),
        (view, 'currentFrame?.cursorBlink == true || currentFrame?.hasBlinkingText == true',
         'currentFrame?.cursorBlink == true', ui + 'blinkingTextRepaintsWithoutCursorOrNewOutput'),
        (view, '(cell.flags and 128 != 0 && !blink)',
         'false', ui + 'blinkingTextRepaintsWithoutCursorOrNewOutput'),
        (surface, 'style.blink << 7',
         '0 << 7', ui + 'blinkingTextRepaintsWithoutCursorOrNewOutput'),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if any(n < 1 or n > len(cases) for n in selected): raise RuntimeError('case out of range')
    work = ROOT / 'tmp/android-terminal-mutations'; work.mkdir(exist_ok=True)
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
            label = f'T{number}'
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
    print(f'ANDROID TERMINAL MUTATIONS PASS: {len(selected)} groups')


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
