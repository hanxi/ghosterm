#!/usr/bin/env python3
"""Android 10 real PTY, physical focus, Return and visible validation guards."""
import hashlib
import os
import re

MARK = 'GT_ANDROID_PTY_FIXTURE_MUTATION'


def execute(root, build, test):
    terminal = root / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    child = root / 'android/app/src/androidTest/cpp/pty_fixture.c'
    main = root / 'android/app/src/main/java/com/hanxi/ghosterm/MainActivity.kt'
    settings = root / 'android/app/src/androidTest/java/com/hanxi/ghosterm/SettingsScreenTest.kt'
    picker = root / 'android/app/src/androidTest/java/com/hanxi/ghosterm/SafImportTest.kt'
    screen = root / 'android/app/src/main/java/com/hanxi/ghosterm/TerminalScreen.kt'
    bar = root / 'android/app/src/main/java/com/hanxi/ghosterm/CompactTerminalBar.kt'
    originals = {p: p.read_bytes() for p in (terminal, child, main, settings, picker, screen, bar)}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual PTY fixture mutation')
    cases = [
        ('raw-mode', child, 'cfmakeraw(&state)', '(void)state',
         'LocalTerminalTest#nativeKeyEncoderUsesApplicationCursorAndTypedUtf8'),
        ('mode-syscall', child, 'tcsetattr(STDIN_FILENO, TCSANOW, &state)',
         'tcsetattr(STDIN_FILENO, TCSAFLUSH, &state)',
         'LocalTerminalTest#nativeKeyEncoderUsesApplicationCursorAndTypedUtf8'),
        ('validation-visible', main, 'focus.clearFocus(); keyboard?.hide(); hosts.saveHost()',
         'hosts.saveHost()', 'HostScreenTest#invalidExplicitValueKeepsDraftAndCanReturnToInheritance'),
        ('framework-focus', terminal, 'defaultFocusHighlightEnabled = false',
         'defaultFocusHighlightEnabled = true',
         'TerminalPreferencesTest#liveCanvasStyleResizesTheSamePtyAndPaintsTheNewPalette'),
        ('physical-return', terminal,
         'if (code == KeyEvent.KEYCODE_ENTER && composition.isNotEmpty() && connection?.acceptForReturn() == false) return false',
         'Unit', 'TerminalScreenTest#imeEnterAndEditorActionAcceptPendingTextBeforeTheReturn'),
        ('locked-night-setting', settings,
         'command.isNotEmpty() && InstrumentationRegistry.getArguments().getString("privilegedNightMode")=="true"',
         'false && command.isNotEmpty() && InstrumentationRegistry.getArguments().getString("privilegedNightMode")=="true"',
         'SettingsScreenTest#systemAppearanceTracksAndroidNightModeInBothDirections'),
    ]
    cases.append(('provider-choice', picker, 'clickSystem("$id.conf")', 'clickSystem("valid.conf")', 'SafImportTest#invalidDocumentsExplainFailureWithoutReplacingConfig'))
    text=originals[bar].decode()
    start=text.index('            TextButton(onClick = onCtrl,')
    end=text.index('            Row(Modifier.weight(1f)',start)
    block=text[start:end]
    scrolled=text[:start]+text[end:]
    anchor='                verticalAlignment = Alignment.CenterVertically) {\n'
    assert scrolled.count(anchor)==1
    scrolled=scrolled.replace(anchor,anchor+block)
    cases.append(('pinned-ctrl', bar, text, scrolled, 'TerminalScreenTest#ctrlPunctuationUsesVtAndLeavesUnicodeAndBulkCommitsIntact'))
    cases.append(('toolbar-copy-route', terminal, 'android.R.id.copy -> copySelection()', 'android.R.id.copy -> Unit', 'TerminalScreenTest#handleEdgesScrollWhileHeldAndStopOnReleaseOrCancel'))
    # Recreate the old split implementation: IME sendKeyEvent still accepts
    # preedit, but a physical View key does not. Only the new physical assertion
    # may turn this mutant red; an earlier IME failure would not prove that path.
    source=originals[terminal].decode()
    central='if (code == KeyEvent.KEYCODE_ENTER && composition.isNotEmpty() && connection?.acceptForReturn() == false) return false'
    wrapper='        override fun sendKeyEvent(event: KeyEvent): Boolean {\n            if (!valid) return false\n'
    assert source.count(central)==source.count(wrapper)==1
    old_return=source.replace(central,'Unit').replace(wrapper,wrapper+'            if (event.action == KeyEvent.ACTION_DOWN && event.keyCode == KeyEvent.KEYCODE_ENTER && !acceptForReturn()) return false\n')
    cases=[(name,path,source,old_return,method) if name=='physical-return' else (name,path,before,after,method)
           for name,path,before,after,method in cases]
    selected = os.environ.get('ANDROID_PTY_FIXTURE_CASES', '').split(',')
    if selected != ['']:
        if set(selected) - {c[0] for c in cases}:
            raise RuntimeError('Unknown PTY fixture mutation')
        cases = [c for c in cases if c[0] in selected]
    try:
        for label, path, before, after, method in cases:
            source = originals[path].decode()
            if source.count(before) != 1:
                raise RuntimeError('Missing or ambiguous mutation anchor: ' + label)
            path.write_text(source.replace(before, after) + '\n// ' + MARK + '\n')
            label = 'pty-fixture-' + label
            target = 'com.hanxi.ghosterm.' + method
            build(label)
            result = test(label, target, False)
            if label=='pty-fixture-physical-return' and 'physical Return accepts preedit before the key' not in result:
                raise RuntimeError('Physical Return mutant must fail its physical assertion')
            if not re.search(r'Tests run: 1,\s+Failures: 1', result):
                raise RuntimeError('Expected one compiled target assertion failure')
            test(label + '-control', 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(originals[path]).digest():
                raise RuntimeError('PTY fixture restore hash differs')
            build(label + '-restored')
            test(label + '-restored', target, True)
            print('PASS ' + label + ': compiled target red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(data)
        if any(path.read_bytes() != data for path, data in originals.items()):
            raise RuntimeError('PTY fixture restoration failed')
    print(f'ANDROID PTY FIXTURE MUTATIONS PASS: {len(cases)} groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Use ANDROID_PTY_FIXTURE_MUTATE=1 with scripts/test-android-ssh.sh on API 29')
