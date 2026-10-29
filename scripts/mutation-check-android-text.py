#!/usr/bin/env python3
"""Android selection, clipboard and local-clear mutations on real PTY and UI."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import signal

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_TEXT_MUTATION'
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
    screen = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm/TerminalScreen.kt'
    clear_patch = ROOT / 'patches/libghostty/android-vt-8159838/0002-local-clear.patch'
    originals = {p: p.read_bytes() for p in (native, surface, view, screen, clear_patch)}
    if any(MARK.encode() in data for data in originals.values()): raise RuntimeError('mutation marker present')
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    local = 'LocalTerminalTest#'
    ui = 'TerminalScreenTest#'
    selected_text = local + 'selectionCopiesGraphemesUnwrapsSoftLinesAndWorksAfterExit'
    clear = local + 'localClearPreservesParserModesCursorAndDoesNotWriteInput'
    copy = ui + 'longPressPaintsSelectionAndCopiesRealTextToSystemClipboard'
    cases = [
        (surface, 'GhosttyResult status = ghostty_terminal_paste(s->terminal, &paste, NULL);',
         '(void)paste; GhosttyResult status = gat_surface_text(s, bytes, length) ? GHOSTTY_SUCCESS : GHOSTTY_IO_ERROR;',
         local + 'pasteUsesVtBracketedModeAndPreservesUtf8'),
        (surface, '.allow_unsafe = x != 0', '.allow_unsafe = true', local + 'rejectedPasteWritesNothingAndLeavesConnectionUsable'),
        (surface, 'opts.unwrap = true;', 'opts.unwrap = false;', selected_text),
        (surface, 'opts.trim = true;', 'opts.trim = false;', selected_text),
        (surface, 'style.blink << 7 | selected << 8', 'style.blink << 7 | selected * 0', selected_text),
        (surface, 'status = ghostty_terminal_select_word(s->terminal, &word, &selection);',
         '(void)word; status = ghostty_terminal_select_all(s->terminal, &selection);', selected_text),
        (surface, '.tag = GHOSTTY_POINT_TAG_VIEWPORT', '.tag = GHOSTTY_POINT_TAG_ACTIVE', local + 'selectionUsesScrolledViewportCoordinates'),
        (surface, 'if (kind == 8) return ghostty_terminal_clear_local(s->terminal) == GHOSTTY_SUCCESS ? 0 : 2;',
         'if (kind == 8) return 0;', clear),
        (surface, 'if (kind == 8) return ghostty_terminal_clear_local(s->terminal) == GHOSTTY_SUCCESS ? 0 : 2;',
         'if (kind == 8) return (ghostty_terminal_reset(s->terminal), 0);', clear),
        (clear_patch, '+    screen.eraseHistory(null);', '+    screen.clearSelection();', local + 'localClearRemovesHistoryAndActiveSelection'),
        (surface, 'if (kind == 8) return ghostty_terminal_clear_local(s->terminal) == GHOSTTY_SUCCESS ? 0 : 2;',
         'if (kind == 8) { (void)s->write(s->context, (const uint8_t *)"\\f", 1); return ghostty_terminal_clear_local(s->terminal) == GHOSTTY_SUCCESS ? 0 : 2; }', clear),
        (native, 's->action_id = 0; s->action_status = 0;', 's->action_id = (uint64_t)request; s->action_status = 0;',
         local + 'cancelledTextAwaitDoesNotBlockTheNextRequest'),
        (view, 'pasteSnapshot(text, true)', 'pasteSnapshot(clipboard.primaryClip!!.getItemAt(0).text.toString(), true)',
         ui + 'pasteConfirmationUsesCapturedTextAndCancelSendsNothing'),
        (view, 'val text = if (clip?.itemCount == 1 && clip.description.hasMimeType("text/*")) clip.getItemAt(0).text?.toString() else null',
         'val text = clip?.getItemAt(0)?.coerceToText(context)?.toString()', ui + 'fileClipboardIsNotCoercedIntoTerminalInput'),
        (view, 'clipboard.setPrimaryClip(ClipData.newPlainText("终端文本", text))', 'text.length', copy),
        (view, 'Color.rgb(68, 91, 125) else cell.background', 'cell.background else cell.background', copy),
        (screen, 'confirmClear = false; terminal?.clearLocal()', 'confirmClear = false', ui + 'clearMenuConfirmsLocalScopeAndKeepsRunningCommand'),
        (screen, 'menu = false; confirmClear = true', 'menu = false; terminal?.clearLocal()', ui + 'clearMenuConfirmsLocalScopeAndKeepsRunningCommand'),
        (view, 'target.extendSelection(operation, point.first, point.second, if (dragging == operation) dragScroll else 0)', 'target.select(SelectionOperation.WORD, point.first, point.second)',
         ui + 'selectionEndHandleChangesCopiedRangeThroughRealTouch'),
        (surface, 'if (status == GHOSTTY_NO_VALUE) return ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, NULL) == GHOSTTY_SUCCESS ? 0 : 2;',
         'if (status == GHOSTTY_NO_VALUE) return 0;', selected_text),
        (view, 'android.R.id.paste -> this@TerminalView.pasteClipboard()', 'android.R.id.paste -> return false',
         ui + 'inputConnectionContextPasteUsesBracketedEncoding'),
        (view, 'isLongClickable = true', 'isLongClickable = false', ui + 'accessibilityLongClickSelectsAndContextCopyUsesTheSelection'),
        (view, 'if (dragging == operation && dragPoint == null) dragPoint = point',
         'if (dragging == operation && dragPoint == null) dragPoint = null', ui + 'handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel'),
        (view, 'if (event.actionMasked == MotionEvent.ACTION_UP) finishDrag()',
         'if (event.actionMasked == MotionEvent.ACTION_UP) Unit', ui + 'handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel'),
        (view, 'if (event.actionMasked == MotionEvent.ACTION_CANCEL) stopDrag()',
         'if (event.actionMasked == MotionEvent.ACTION_CANCEL) Unit', ui + 'handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel'),
        (surface, 'selection = current;',
         '(void)ghostty_terminal_selection_ordered(s->terminal, &current, GHOSTTY_SELECTION_ORDER_FORWARD, &selection);',
         local + 'draggingAcrossTheAnchorKeepsTheOtherEndpointUntilNextGesture'),
        (surface, 'return ghostty_terminal_set(s->terminal, GHOSTTY_TERMINAL_OPT_SELECTION, &ordered) == GHOSTTY_SUCCESS ? 0 : 2;',
         'return 0;', local + 'draggingAcrossTheAnchorKeepsTheOtherEndpointUntilNextGesture'),
        (view, 'if (orderDrag) { orderDrag = false; target.select(SelectionOperation.ORDER) }',
         'if (orderDrag) { orderDrag = false }', ui + 'crossingThenRegrabbingHandleKeepsTheCorrectAnchor'),
        (surface, 'gat_surface_scroll(s, (int8_t)bytes[0]);',
         '(void)bytes;', ui + 'handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel'),
        (view, '.minByOrNull { it.second }?.takeIf',
         '.firstOrNull()?.takeIf', ui + 'handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel'),
        (view, 'if (sent && ctrl == 1) ctrl = 0\n        return sent\n    }\n    fun commitText',
         'if (sent && ctrl == 99) ctrl = 0\n        return sent\n    }\n    fun commitText', ui + 'ctrlSingleUseAndLockEncodeRealPtyInput'),
        (view, 'if (sent && ctrl == 1) ctrl = 0\n        return sent\n    }\n    fun commitText',
         'if (sent && ctrl != 0) ctrl = 0\n        return sent\n    }\n    fun commitText', ui + 'ctrlSingleUseAndLockEncodeRealPtyInput'),
        (view, 'if (!gain) { stopDrag(); connection?.cancel(); connection = null; ctrl = 0 }',
         'if (!gain) { stopDrag(); connection?.cancel(); connection = null; ctrl = 2 }', ui + 'ctrlAndCompositionClearWhenFocusMovesToAnotherView'),
        (screen, 'terminal?.ctrl = 0; terminal?.hideKeyboard()',
         'terminal?.ctrl = 2; terminal?.hideKeyboard()', ui + 'ctrlDoesNotCrossPagesSessionsOrNormalExit'),
        (screen, 'key(session.id)', 'key(Unit)', ui + 'ctrlDoesNotCrossPagesSessionsOrNormalExit'),
        (view, "text[0] in ' '..'~'", "text[0] in 'a'..'z'",
         ui + 'ctrlPunctuationUsesVtAndLeavesUnicodeAndBulkCommitsIntact'),
        (view, "text[0] in ' '..'~'", "(text[0] in ' '..'~' || text[0].lowercaseChar() in 'a'..'z')",
         ui + 'ctrlPunctuationUsesVtAndLeavesUnicodeAndBulkCommitsIntact'),
        (view, 'unshiftedCodepoint = character.code', 'unshiftedCodepoint = 0',
         ui + 'softCtrlUsesCurrentKittyAndModifyOtherKeysModes'),
        (view, 'val unshifted = event.getUnicodeChar(0).let {',
         'val unshifted = event.getUnicodeChar(event.metaState and KeyEvent.META_CTRL_MASK.inv() and KeyEvent.META_META_MASK.inv()).let {',
         ui + 'hardwareModifiersKeepLayoutCodepointsForKittyEncoding'),
        (native, '.unshifted = (uint32_t)unshifted', '.unshifted = 0',
         ui + 'softCtrlUsesCurrentKittyAndModifyOtherKeysModes'),
        (surface, 'unshifted ? unshifted :', 'unshifted ? 0 :',
         ui + 'hardwareModifiersKeepLayoutCodepointsForKittyEncoding'),
        (native, 'fail(env, "终端按键字符无效"); return;', '(void)env; return;',
         local + 'invalidLayoutCodepointsAreRejectedWithoutPoisoningInput'),
        (surface, 'ghostty_key_encoder_setopt_from_terminal(s->encoder, s->terminal);', '(void)s->encoder;',
         ui + 'softCtrlUsesCurrentKittyAndModifyOtherKeysModes'),
        (screen, 'terminal?.commitText(value)', 'terminal?.surface?.commit(value)',
         ui + 'ctrlPunctuationUsesVtAndLeavesUnicodeAndBulkCommitsIntact'),
        (view, 'text.toString() == "\\n"', 'text.toString() == "never-newline"',
         ui + 'imeSingleNewlineUsesEnterButMultilineCommitStaysText'),
        (native, '.key_action = key_action', '.key_action = 1',
         ui + 'hardwarePressRepeatAndReleaseFollowCurrentVtMode'),
        (surface, 'ghostty_key_event_set_action(s->key, (GhosttyKeyAction)action);',
         'ghostty_key_event_set_action(s->key, GHOSTTY_KEY_ACTION_PRESS);',
         ui + 'hardwarePressRepeatAndReleaseFollowCurrentVtMode'),
        (view, 'if (previous != null && event.repeatCount > 0) KeyAction.REPEAT else KeyAction.PRESS',
         'if (previous != null && event.repeatCount > 0) KeyAction.PRESS else KeyAction.PRESS',
         ui + 'hardwarePressRepeatAndReleaseFollowCurrentVtMode'),
        (view, 'return dispatchInput { surface?.key(key.code, keyModifiers(event) or key.softCtrl, "", key.unshifted, KeyAction.RELEASE) }',
         'return true', ui + 'hardwarePressRepeatAndReleaseFollowCurrentVtMode'),
        (view, 'previous?.softCtrl ?: if (ctrl != 0) 2 else 0', 'if (ctrl != 0) 2 else 0',
         ui + 'oneShotCtrlStaysWithHeldKeyUntilRelease'),
        (view, 'if (!gain) releaseHeldKeys()', 'if (false) releaseHeldKeys()',
         ui + 'heldKeysReleaseOnFocusLossAndBeforeSurfaceChanges'),
        (view, 'if (!hasWindowFocus) releaseHeldKeys()', 'if (false) releaseHeldKeys()',
         ui + 'heldKeysReleaseOnFocusLossAndBeforeSurfaceChanges'),
        (view, 'if (field === value) return\n            releaseHeldKeys()',
         'if (field === value) return\n            heldKeys.clear()',
         ui + 'heldKeysReleaseOnFocusLossAndBeforeSurfaceChanges'),
        (view, 'heldKeys.clear()\n        pending.forEach', 'Unit\n        pending.forEach',
         ui + 'heldKeysReleaseOnFocusLossAndBeforeSurfaceChanges'),
        (view, 'target.key(code, effectiveMods, "", unshiftedCodepoint, KeyAction.RELEASE)',
         'Unit', ui + 'auxiliaryAndImeKeyTapsIncludeReleaseWhenRequested'),
        (surface, 'case 60: return GHOSTTY_KEY_SHIFT_RIGHT;', 'case 60: return GHOSTTY_KEY_SHIFT_LEFT;',
         ui + 'modifierKeysLocksAndSoftCtrlKeepSeparateState'),
        (surface, 'return GHOSTTY_KEY_NUMPAD_0 + key - 144;', 'return GHOSTTY_KEY_NUMPAD_0;',
         ui + 'numericKeypadRespectsNumLockApplicationModeAndReleaseIdentity'),
        (view, '(if (event.isCapsLockOn) 16 else 0)', '(if (event.isCapsLockOn) 0 else 0)',
         ui + 'modifierKeysLocksAndSoftCtrlKeepSeparateState'),
        (view, '(if (event.isNumLockOn) 32 else 0)', '(if (event.isNumLockOn) 0 else 0)',
         ui + 'modifierKeysLocksAndSoftCtrlKeepSeparateState'),
        (view, 'if (!modifierKey && ctrl == 1) ctrl = 0', 'if (ctrl == 1) ctrl = 0',
         ui + 'modifierKeysLocksAndSoftCtrlKeepSeparateState'),
        (view, 'surface?.key(key.code, keyModifiers(event) or key.softCtrl, "", key.unshifted, KeyAction.RELEASE)',
         'surface?.key(code, keyModifiers(event) or key.softCtrl, "", key.unshifted, KeyAction.RELEASE)',
         ui + 'numericKeypadRespectsNumLockApplicationModeAndReleaseIdentity'),
        (view, 'val combined = KeyCharacterMap.getDeadChar(accent, unicode)', 'val combined = 0',
         ui + 'deadKeysComposeCancelAndPreserveLayoutText'),
        (view, 'accent != null && (code == KeyEvent.KEYCODE_ESCAPE || code == KeyEvent.KEYCODE_DEL)',
         'false', ui + 'deadKeysComposeCancelAndPreserveLayoutText'),
        (view, 'pendingAccents.clear(); invalidate()\n        val pending = heldKeys.values.toList()',
         'invalidate()\n        val pending = heldKeys.values.toList()',
         ui + 'deadKeyStateDoesNotCrossFocusDevicesOrImeCommits'),
        (view, 'val accent = pendingAccents[device]', 'val accent = pendingAccents.values.lastOrNull()',
         ui + 'deadKeyStateDoesNotCrossFocusDevicesOrImeCommits'),
        (view, 'fun commitText(text: String): Boolean {\n        pendingAccents.clear(); invalidate()',
         'fun commitText(text: String): Boolean {\n        invalidate()',
         ui + 'deadKeyStateDoesNotCrossFocusDevicesOrImeCommits'),
        (view, 'if (!dead && unicode >= 32 && unicode != 127 && event.isAltPressed',
         'if (false && unicode >= 32 && unicode != 127 && event.isAltPressed',
         ui + 'deadKeysComposeCancelAndPreserveLayoutText'),
        (view, 'else prefix + String(Character.toChars(unicode))', 'else String(Character.toChars(unicode))',
         ui + 'deadKeysComposeCancelAndPreserveLayoutText'),
        (native, 'mods > 1023', 'mods > 15', ui + 'modifierKeysLocksAndSoftCtrlKeepSeparateState'),
        (view, 'if (!dispatch { surface?.commit(accents) }) return false', 'if (false) return false',
         ui + 'auxiliaryKeysResolvePendingDeadCompositionBeforeSending'),
        (view, 'if (code == KeyEvent.KEYCODE_ESCAPE || code == KeyEvent.KEYCODE_DEL) {', 'if (false) {',
         ui + 'auxiliaryKeysResolvePendingDeadCompositionBeforeSending'),
    ]
    selected = args.selected or list(range(1, len(cases) + 1))
    if any(n < 1 or n > len(cases) for n in selected): raise RuntimeError('case out of range')
    work = ROOT / 'tmp/android-text-mutations'; work.mkdir(exist_ok=True)
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
            label = f'X{number}'
            source = originals[path].decode()
            if source.count(before) != 1: raise RuntimeError(f'{label}: mutation anchor missing/ambiguous')
            suffix = ' // ' + MARK if path == clear_patch else ' /* ' + MARK + ' */'
            path.write_text(source.replace(before, after + suffix))
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
    print(f'ANDROID TEXT MUTATIONS PASS: {len(selected)} groups')


if __name__ == '__main__':
    def interrupted(signum, frame): raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
