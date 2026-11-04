#!/usr/bin/env python3
"""Device mutations for host deletion, terminal input and foreground ownership.
Run via ANDROID_SESSION_MANAGEMENT_MUTATE=1 scripts/test-android-ssh.sh.
"""
import hashlib
import os
import signal

MARK = 'GT_ANDROID_SESSION_MANAGEMENT_MUTATION'
def execute(root, build, test):
    app = root / 'android/app/src/main/java/com/hanxi/ghosterm'
    data = root / 'android/data/src/main/java/com/hanxi/ghosterm/data/ConfigRepository.kt'
    view = root / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    paths = [app / 'HostsViewModel.kt', data, app / 'TerminalConnectionService.kt', app / 'SessionsViewModel.kt', view, app / 'TerminalScreen.kt']
    originals = {p: p.read_bytes() for p in paths}
    assert all(MARK.encode() not in value for value in originals.values())
    control = 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig'
    def replace(path, before, after, section=None):
        source = originals[path].decode()
        if section:
            start = source.index(section)
            prefix, source = source[:start], source[start:]
        else: prefix = ''
        assert source.count(before) == 1, before
        return {path: prefix + source.replace(before, after, 1) + '\n// ' + MARK + '\n'}
    host = 'com.hanxi.ghosterm.HostScreenTest#deleteHostRequiresConfirmationAndPreservesSharedAliasAndBackup'
    background = 'com.hanxi.ghosterm.TerminalBackgroundProductTest#'
    screen = 'com.hanxi.ghosterm.TerminalScreenTest#'
    promotion = ('            if (Build.VERSION.SDK_INT >= 34) startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE)\n'
                 '            else startForeground(NOTIFICATION, notification)')
    release = 'if (!enabled) { wake?.let { if (it.isHeld) it.release() }; wake = null; return }'
    cases = [
        ('host-confirm', host, lambda: replace(app / 'HostsViewModel.kt', 'removal = HostRemoval(current, base)', 'removal = HostRemoval(current, base); confirmRemove()')),
        ('host-write', host, lambda: replace(data, 'return save(previous, candidate)', 'return ConfigSaved(parsed)', 'fun removeHost(')),
        ('host-revision', 'com.hanxi.ghosterm.ConfigRepositoryTest#deleteRejectsStaleConfigurationAndBackupFailureWithoutChangingOriginal',
         lambda: replace(data, 'if (revision != previous.revision) throw ConfigConflict()', 'if (false) throw ConfigConflict()')),
        ('foreground-promotion', background + 'realServiceAndOptionalWakeLockTrackSessionsAndBackgroundIoContinues',
         lambda: replace(app / 'TerminalConnectionService.kt', promotion, promotion + '\n            stopForeground(STOP_FOREGROUND_REMOVE)')),
        ('wake-release', background + 'realServiceAndOptionalWakeLockTrackSessionsAndBackgroundIoContinues',
         lambda: replace(app / 'TerminalConnectionService.kt', release, 'if (!enabled) { wake = null; return }')),
        ('presenter-owner', background + 'clearingAPresenterKeepsTheNativeOwnerForTheNextPresenter',
         lambda: replace(app / 'SessionsViewModel.kt', 'if (!processTerminals) sessions.forEach { it.connection.close() }', 'sessions.forEach { it.connection.close() }')),
        ('cleanup-live-guard', screen + 'cleanupRemovesOnlyConfirmedStoppedIdsAndKeepsLiveAndLaterStoppedSessions',
         lambda: replace(app / 'SessionsViewModel.kt', 'if (session.state.terminal) close(id)', 'close(id)')),
        ('wheel-route', screen + 'fingerSwipeRoutesWheelToTheSameMouseSurface',
         lambda: replace(view, 'if (selecting || !inputEnabled) surface?.scroll(lines)', 'if (true) surface?.scroll(lines)')),
        ('keyboard-menu', screen + 'menuShowsKeyboardInMouseModeWithoutSendingTerminalInput',
         lambda: replace(app / 'TerminalScreen.kt', 'menu = false; keyboardRequested = true', 'menu = false; keyboardRequested = false')),
    ]
    selected = os.environ.get('ANDROID_SESSION_MANAGEMENT_CASES', '').split(',')
    if selected != ['']:
        assert set(selected) <= {name for name, _, _ in cases}
        cases = [case for case in cases if case[0] in selected]
    def interrupted(signum, _frame): raise KeyboardInterrupt('signal ' + str(signum))
    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        for name, selection, mutate in cases:
            try:
                changes = mutate()
                for path, content in changes.items(): path.write_text(content)
                label = 'session-management-' + name
                build(label)
                output = test(label, selection, False)
                assert 'AssertionError' in output or 'ComparisonFailure' in output
                assert 'Process crashed' not in output and 'INSTRUMENTATION_FAILED' not in output
                test(label + '-control', control, True)
            finally:
                for path, original in originals.items(): path.write_bytes(original)
            build(label + '-restored')
            test(label + '-restored', selection, True)
            assert all(hashlib.sha256(p.read_bytes()).digest() == hashlib.sha256(value).digest() for p, value in originals.items())
            print('PASS ' + label + ': compiled normal assertion red / same APK config green / restored green / SHA256', flush=True)
    finally:
        for path, original in originals.items(): path.write_bytes(original)
