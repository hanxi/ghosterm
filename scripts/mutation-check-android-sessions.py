#!/usr/bin/env python3
"""Compile session-switcher mutations on a device; require one normal assertion red and independent Core green."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import signal
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_ANDROID_SESSIONS_MUTATION'
spec = importlib.util.spec_from_file_location('native_helper', ROOT / 'scripts/mutation-check-android-native.py')
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', action='append', dest='selected')
    args = parser.parse_args()
    serial = os.environ.get('ANDROID_SERIAL')
    if not serial:
        raise RuntimeError('Set ANDROID_SERIAL to the intended test device')
    adb = ['adb', '-s', serial]
    helper.require(helper.run([*adb, 'get-state']))
    base = ROOT / 'android/app/src/main/java/com/hanxi/ghosterm'
    screen, panel, model, bar = (base / name for name in (
        'TerminalScreen.kt', 'TerminalSessionPanel.kt', 'SessionsViewModel.kt', 'CompactTerminalBar.kt'))
    view = ROOT / 'terminal/android/src/main/java/com/hanxi/ghosterm/terminal/TerminalView.kt'
    originals = {path: path.read_bytes() for path in (screen, panel, model, bar, view)}
    if any(MARK.encode() in data for data in originals.values()):
        print('Residual session-switcher mutation', file=sys.stderr)
        raise SystemExit(2)
    prefix = 'com.hanxi.ghosterm.TerminalSessionPanelTest#'
    entry = prefix + 'fixedEntryReplacesMenuAndRemainsReachableWithoutAuxiliaryKeys'
    previous = prefix + 'previousSelectionKeepsOwnersOrderAndStableNumbers'
    keyboard = prefix + 'switchAndDismissRestoreOpenImeAndInputGoesOnlyToSelectedPty'
    stopped = prefix + 'closedImeStaysClosedAndStoppedPreviousDoesNotReappear'
    grouping = prefix + 'groupedSearchAndFooterKeepCurrentAndAllCreationActionsVisible'
    close = prefix + 'rowCloseConfirmationKeepsTheCapturedUuidAcrossSelectionChanges'
    opening = prefix + 'openingAndCancellingPanelKeepsImeVisibleOnEveryFrame'
    search_focus = prefix + 'searchingKeepsImeVisibleAndReturnsInputFocusToSameTerminal'
    cases = [
        ('entry-route', entry, bar, 'IconButton(onClick = onSessions,', 'IconButton(onClick = {},'),
        ('hidden-aux-entry', 'com.hanxi.ghosterm.TerminalLayoutTest#portraitFocusKeepsMenuFixedAndKeyboardInMenu',
         bar, 'IconButton(onClick = onSessions,', 'if (policy.auxiliaryKeys) IconButton(onClick = onSessions,'),
        ('duplicate-menu', entry, screen, 'val actions: @Composable () -> Unit = {',
         'val actions: @Composable () -> Unit = { Text("切换会话", Modifier.testTag("terminal-menu-sessions"))'),
        ('previous-selection', previous, model, 'terminalStore.previous = active?.id', 'terminalStore.previous = null'),
        ('stable-numbers', previous, model,
         '(sessions.filter { it.remoteAlias == session.remoteAlias }\n            .maxOfOrNull { it.switcherNumber } ?: 0) + 1', '1'),
        ('keyboard-restore', search_focus, screen, 'if (restore && keyboardBeforePanel)', 'if (false)'),
        ('keyboard-hidden', stopped, screen, 'if (restore && keyboardBeforePanel)', 'if (restore)'),
        ('keyboard-snapshot', search_focus, screen,
         'keyboardBeforePanel = terminal?.let {\n            androidx.core.view.ViewCompat.getRootWindowInsets(it)\n                ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime())\n        } ?: imeVisible',
         'keyboardBeforePanel = false'),
        ('stopped-previous', stopped, model, 'it.id != selected && !it.state.terminal', 'it.id != selected'),
        ('stopped-collapsed', stopped, panel,
         'mutableStateOf(sessions.any { it.id == selectedId && it.state.terminal })', 'mutableStateOf(true)'),
        ('grouping', grouping, panel, '.groupBy { it.remoteAlias }', '.groupBy { null }'),
        ('search', grouping, panel, 'text.contains(query.trim(), ignoreCase = true)', 'text.contains("", ignoreCase = true)'),
        ('current-highlight', grouping, panel, 'selected = current', 'selected = false'),
        ('footer-many', grouping, panel,
         'Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp)',
         'if (sessions.size < 7) Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp, vertical = 10.dp)'),
        ('row-selection', keyboard, panel, '.clickable { onSelect(item.id) }', '.clickable { Unit }'),
        ('close-target', close, screen, 'model.close(target.id)', 'model.close(session.id)'),
        ('close-cancel', close, screen,
         'dismissButton = { TextButton(onClick = { closeTarget = null })',
         'dismissButton = { TextButton(onClick = { closeTarget?.let { model.close(it.id) }; closeTarget = null })'),
        ('new-ssh', grouping, screen,
         'dismissSessions(false); terminal?.hideKeyboard(); model.selectNewRemote()',
         'dismissSessions(false); terminal?.hideKeyboard(); Unit'),
    ]
    cases += [
        ('adaptive-height', entry, panel, '.heightIn(max = preferredHeight).fillMaxHeight(.92f)', '.fillMaxHeight(.92f)'),
        ('keyboard-platform-state', stopped, screen,
         'keyboardBeforePanel = terminal?.let {\n            androidx.core.view.ViewCompat.getRootWindowInsets(it)\n                ?.isVisible(androidx.core.view.WindowInsetsCompat.Type.ime())\n        } ?: imeVisible',
         'keyboardBeforePanel = true'),
    ]
    screen_source = originals[screen].decode()
    retained = screen_source.replace('                AndroidView(factory', '            key(session.id) {\n                AndroidView(factory')
    retained = retained.replace('modifier = Modifier.fillMaxSize().testTag("terminal-surface"))\n            if (compact)',
        'modifier = Modifier.fillMaxSize().testTag("terminal-surface"))\n            }\n            if (compact)')
    panel_source = originals[panel].decode()
    separate_window = panel_source.replace('    BoxWithConstraints(Modifier.fillMaxSize().testTag("terminal-session-overlay")',
        '    androidx.compose.ui.window.Dialog(onDismissRequest = onDismiss) {\n    BoxWithConstraints(Modifier.fillMaxSize().testTag("terminal-session-overlay")')
    separate_window = separate_window.replace('\n}\n\n@Composable\nprivate fun TerminalSessionRow', '\n    }\n}\n\n@Composable\nprivate fun TerminalSessionRow')
    cases += [
        ('panel-open-hide', opening, screen, 'keyboardRequested = null\n        list = true',
         'keyboardRequested = null\n        terminal?.hideKeyboard()\n        list = true'),
        ('panel-window-focus', opening, panel, panel_source, separate_window),
        ('panel-ime-insets', opening, screen, '.union(WindowInsets.ime)', ''),
        ('panel-compact-scroll', 'com.hanxi.ghosterm.TerminalLayoutTest#largeFontCompactMenuRoutesFilesAndSessionsWithoutReplacingOwner',
         panel, 'if (compactPanel) item(key = "compact-footer")', 'if (false) item(key = "compact-footer")'),
        ('view-retained', 'com.hanxi.ghosterm.TerminalScreenTest#ctrlDoesNotCrossPagesSessionsOrNormalExit', screen, screen_source, retained),
        ('surface-context-cancel', 'com.hanxi.ghosterm.TerminalScreenTest#ctrlDoesNotCrossPagesSessionsOrNormalExit', view,
         'resetTextActions()\n            connection?.cancel(); connection = null',
         'resetTextActions()\n            connection = null'),
        ('panel-direct-focus', search_focus, screen, 'if (restore) terminal?.requestFocus()', 'if (restore) Unit'),
        ('panel-late-dismiss', opening, screen,
         'val dismissSessions: (Boolean) -> Unit = { restore ->\n        if (list) {',
         'val dismissSessions: (Boolean) -> Unit = { restore ->\n        if (true) {'),
        ('surface-selection-reset', 'com.hanxi.ghosterm.TerminalScreenTest#ctrlDoesNotCrossPagesSessionsOrNormalExit', view,
         'releaseHeldKeys()\n            resetTextActions()',
         'releaseHeldKeys()'),
        ('pre-ime-back', keyboard, view, 'keyCode == KeyEvent.KEYCODE_BACK && handler != null', 'keyCode == KeyEvent.KEYCODE_UNKNOWN && handler != null'),
        ('search-pre-ime-back', search_focus, panel, 'if (event.key == Key.Back)', 'if (false)'),
    ]
    platform_only = {'keyboard-platform-state', 'pre-ime-back', 'search-pre-ime-back'}
    selected = args.selected or [case[0] for case in cases if case[0] not in platform_only]
    if platform_only.intersection(selected):
        device = helper.run([*adb, 'shell', 'getprop', 'ro.build.version.sdk'])
        helper.require(device)
        if device.stdout.strip() != '29':
            raise RuntimeError('Selected legacy platform/Back guards require API 29')
    if len(selected) != len(set(selected)) or set(selected) - {case[0] for case in cases}:
        raise RuntimeError('Unknown or duplicate session mutation')
    cases = [case for case in cases if case[0] in selected]
    for name, _, path, old, _ in cases:
        if originals[path].decode().count(old) != 1:
            raise RuntimeError('Missing or ambiguous source anchor: ' + name)
    work = ROOT / 'tmp/android-session-mutations' / str(time.time_ns())
    work.mkdir(parents=True)
    for path, data in originals.items():
        (work / path.name).write_bytes(data)
    print('SESSION MUTATION RECEIPTS', work, flush=True)

    def logged(label, command, seconds=180):
        result = helper.run(command, seconds)
        output = result.stdout + result.stderr
        (work / (label + '.log')).write_text(output)
        helper.require(result)
        return output

    def build(label):
        logged(label + '-build', ['bash', ROOT / 'scripts/build-android.sh'], 900)
        for apk in ('apk/debug/app-debug.apk', 'apk/androidTest/debug/app-debug-androidTest.apk'):
            logged(label + '-install-' + Path(apk).name, [*adb, 'install', '-r', '-t', ROOT / 'android/app/build/outputs' / apk])

    def test(label, selection, green):
        output = logged(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selection,
            'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
        good = 'OK (1 test)' in output and 'FAILURES' not in output if green else (
            bool(re.search(r'Tests run: 1,\s+Failures: 1', output)) and
            any(kind in output for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure')) and
            'Process crashed' not in output)
        if 'INSTRUMENTATION_STATUS: numtests=1' not in output or not good:
            raise RuntimeError(output)

    def restore():
        for path, data in originals.items():
            if MARK.encode() in path.read_bytes():
                path.write_bytes(data)
            if hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(data).digest():
                raise RuntimeError('Session source restoration mismatch: ' + path.name)

    results = []
    try:
        build('baseline')
        for index, target in enumerate(dict.fromkeys(case[1] for case in cases)):
            test('baseline-' + str(index), target, True)
        for name, target, path, old, new in cases:
            path.write_text(originals[path].decode().replace(old, new) + '\n// ' + MARK + '\n')
            build(name)
            test(name + '-target', target, False)
            test(name + '-control', 'com.hanxi.ghosterm.ConfigRepositoryTest#firstSaveAndReloadDeriveHostsFromConfig', True)
            restore()
            build(name + '-restored')
            test(name + '-restored', target, True)
            results.append(dict(case=name, compiled_assertion_red=True, same_apk_core_green=True, restored_green=True))
            (work / 'accepted-results.json').write_text(json.dumps(results, indent=2) + '\n')
            print('PASS ' + name + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
    finally:
        restore()
        helper.require(helper.run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm']))
    print(f'ANDROID SESSION MUTATIONS PASS: {len(results)} groups; receipts {work}', flush=True)


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f'signal {signum}')
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    main()
