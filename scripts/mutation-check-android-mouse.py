#!/usr/bin/env python3
"""Mouse input, remote pixel geometry and read-failure diagnostics; compile every mutant."""
import hashlib
import os

MARK = 'GT_ANDROID_MOUSE_MUTATION'


def execute(root, build, test):
    surface = root/'terminal/android/native/surface.c'
    view = root/'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    session = root/'android/bridge/src/main/cpp/session_jni.c'
    binding = root/'android/bridge/src/main/java/com/hanxi/ghosterm/bridge/NativeSession.kt'
    core = root/'core/src/ssh.c'
    controls = root/'android/app/src/main/java/com/hanxi/ghosterm/CompactTerminalBar.kt'
    originals = {p: p.read_bytes() for p in (surface, view, session, binding, core, controls)}
    hashes = {p: hashlib.sha256(data).digest() for p, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):
        raise RuntimeError('Residual mouse mutation')
    native = 'com.hanxi.ghosterm.TerminalMouseTest#'
    ui = 'com.hanxi.ghosterm.TerminalScreenTest#'
    ssh = 'com.hanxi.ghosterm.SSHChannelTest#'
    cases = [
        ('tap-route', view, 'else tapTerminal(event, 1)', 'else performClick()',
         ui+'mouseModeFingerTapSendsRealCoordinatesAndOpensKeyboard'),
        ('coordinates', surface, '.x = (float)x, .y = (float)y', '.x = (float)x + s->cw, .y = (float)y',
         native+'sgrClickUsesCellsAndBalancedPressRelease'),
        ('release', surface, 'action <= GHOSTTY_MOUSE_ACTION_RELEASE', 'action < GHOSTTY_MOUSE_ACTION_RELEASE',
         native+'sgrClickUsesCellsAndBalancedPressRelease'),
        ('disabled-mode', surface, 'if (!tracking) return 1;', 'if (!tracking) return 0;',
         native+'disabledMouseWritesNothingAndInvalidInputDoesNotPoisonNextInput'),
        ('format', surface, 'ghostty_mouse_encoder_setopt_from_terminal(s->mouse_encoder, s->terminal);',
         'ghostty_mouse_encoder_setopt_from_terminal(s->mouse_encoder, s->terminal);\n        ghostty_mouse_encoder_setopt(s->mouse_encoder, GHOSTTY_MOUSE_ENCODER_OPT_FORMAT, &(GhosttyMouseFormat){GHOSTTY_MOUSE_FORMAT_SGR});',
         native+'x10ClickDoesNotInventAReleasePacket'),
        ('button', surface, 'ghostty_mouse_event_set_button(s->mouse, (GhosttyMouseButton)bytes[0]);',
         'ghostty_mouse_event_set_button(s->mouse, GHOSTTY_MOUSE_BUTTON_LEFT);',
         native+'rightAndMiddleButtonsModifiersAndRepeatedClicksStayOrdered'),
        ('long-press', view, 'override fun onLongPress(event: MotionEvent) {',
         'override fun onLongPress(event: MotionEvent) {\n            tapTerminal(event, 1)',
         ui+'longPressInMouseModeSelectsTextWithoutSendingAClick'),
        ('keyboard', view, 'if (touchTyping) performClick()',
         'if (false && touchTyping) performClick()',
         ui+'mouseModeFingerTapSendsRealCoordinatesAndOpensKeyboard'),
        ('validation', binding, 'require(x >= 0 && y >= 0 && modifiers in 0..1023 && button in 1..3)',
         'require(x >= -1 && y >= 0 && modifiers in 0..1023 && button in 1..3)',
         native+'disabledMouseWritesNothingAndInvalidInputDoesNotPoisonNextInput'),
        ('remote-pixels', core, 'rows ? rows : 24, (int)px_w, (int)px_h)', 'rows ? rows : 24, 0, 0)',
         ssh+'remoteResizePreservesPixelDimensionsForRealTerminalTools'),
        ('read-code', core, '"ssh_shell read: libssh2=%ld%s%s", (long)n,',
         '"ssh_shell read: libssh2=%ld%s%s", (long)LIBSSH2_ERROR_TIMEOUT,',
         ssh+'idleReadsStayConnectedAndPeerDropReportsTheActualReadFailure'),
        ('read-detail', session, 'gt_last_error());\n            failed(s, reason);',
         '"detail unavailable");\n            failed(s, reason);',
         ssh+'idleReadsStayConnectedAndPeerDropReportsTheActualReadFailure'),
        ('idle-timeout', core, 'if (n == 0 || n == LIBSSH2_ERROR_TIMEOUT || n == LIBSSH2_ERROR_EAGAIN)\n    return GT_TRANSPORT_READ_TIMEOUT;',
         'if ((n == 0 || n == LIBSSH2_ERROR_TIMEOUT || n == LIBSSH2_ERROR_EAGAIN) && false)\n    return GT_TRANSPORT_READ_TIMEOUT;',
         ssh+'idleReadsStayConnectedAndPeerDropReportsTheActualReadFailure'),
        ('eof', core, 'if (libssh2_channel_eof(sh->chan)) return GT_TRANSPORT_READ_EOF;',
         'if (libssh2_channel_eof(sh->chan)) return GT_TRANSPORT_READ_ERROR;',
         ssh+'orderlyRemoteEofIsNotReportedAsAReadFailure'),
    ]
    cases += [
        ('finger-history', view,
         'if (selecting || !inputEnabled) surface?.scroll(lines) else surface?.wheel(lines, x, y, modifiers)',
         'if (selecting || !inputEnabled) surface?.scroll(lines) else Unit',
         ui+'fingerSwipeMovesOrdinaryShellHistoryWithoutOpeningKeyboard'),
        ('tmux-touch-keyboard', view, 'if (touchTyping) performClick()',
         'if (false && touchTyping) performClick()',
         'com.hanxi.ghosterm.TmuxProductTest#tappingRealTmuxStatusSwitchesWindowsAndOpensTheKeyboard'),
        ('key-repeat', controls, 'repeated = true\n                        action()',
         'repeated = true\n                        Unit',
         'com.hanxi.ghosterm.TerminalKeyRepeatTest#holdingRepeatsAndReleaseAddsNoKeyAndStops'),
        ('key-repeat-release', controls, 'interactions.interactions.collectLatest { interaction ->',
         'interactions.interactions.collect { interaction ->',
         'com.hanxi.ghosterm.TerminalKeyRepeatTest#holdingRepeatsAndReleaseAddsNoKeyAndStops'),
    ]
    selected = os.environ.get('ANDROID_MOUSE_MUTATIONS', '').split(',')
    if selected != ['']:
        if set(selected)-{c[0] for c in cases}:
            raise RuntimeError('Unknown mouse mutation selection')
        cases = [c for c in cases if c[0] in selected]
    try:
        for label, path, old, new, method in cases:
            source = originals[path].decode()
            if source.count(old) != 1:
                raise RuntimeError('Ambiguous mouse mutation '+label)
            path.write_text(source.replace(old, new)+'\n// '+MARK+'\n')
            name = 'mouse-'+label
            build(name)
            test(name, method, False)
            test(name+'-control', 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            path.write_bytes(originals[path])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[path]:
                raise RuntimeError('Mouse restoration mismatch')
            build(name+'-restored')
            test(name+'-restored', method, True)
            print('PASS '+name+': compiled assertion red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(data)
        if any(hashlib.sha256(p.read_bytes()).digest() != hashes[p] for p in originals):
            raise RuntimeError('Mouse source restoration failed')
    print(f'ANDROID MOUSE MUTATIONS PASS: {len(cases)} groups', flush=True)


if __name__ == '__main__':
    raise SystemExit('Use ANDROID_MOUSE_MUTATE=1 with scripts/test-android-ssh.sh')
