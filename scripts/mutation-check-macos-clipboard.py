#!/usr/bin/env python3
"""Compiled clipboard/menu mutations on a private native checkout below project tmp/.

Uses disposable config, fixture clipboard strings and transparent background
windows. The menu tests supply only the OS key/main-window query; real NSMenu,
nil targets and responder search remain in use. No SSH/user host data is used.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
TERMINAL = 'apple/GhostermTerminal/'
APP = 'apple/GhostermApp/'
SURFACE = TERMINAL + 'Sources/GhostermTerminal/TerminalSurface.swift'
VIEW = TERMINAL + 'Sources/GhostermTerminal/TerminalSurfaceView.swift'
SHIM = TERMINAL + 'Sources/CGhostty/shim.c'
MENU = APP + 'Sources/GhostermApp/AppMenu.swift'
CASES = [
    ('shim-action', SHIM, 'static const char action[] = "paste_from_clipboard";', 'static const char action[] = "ignore";', 'testSafeAndBracketedPastesUseEngineEncodingWithoutPrompt'),
    ('paste-entry', VIEW, '        surface.pasteFromClipboard()', '        if let text = NSPasteboard.general.string(forType: .string) { surface.sendText(text) }', 'testUnsafeMenuPasteWaitsForApprovalAndUsesReviewedSnapshot'),
    ('pane-cancel', VIEW, 'if !paneFocused { surface.cancelClipboardRequests() }', '// mutation: keep the old pane permission', 'testChangingActivePaneRevokesItsPendingPaste'),
    ('approve-result', SURFACE, 'if allowed {', 'if true {', 'testDeniedUnsafePasteProducesNoInput'),
    ('reviewed-bytes', SURFACE, 'text.withCString { gt_ghostty_surface_clipboard_complete(raw, request, $0, text.utf8.count, true) }', 'let value = NSPasteboard.general.string(forType: .string) ?? ""\n                    value.withCString { gt_ghostty_surface_clipboard_complete(raw, request, $0, value.utf8.count, true) }', 'testUnsafeMenuPasteWaitsForApprovalAndUsesReviewedSnapshot'),
    ('pending-once', SURFACE, 'guard clipboardDecision == nil else { completion(false); return }', '// mutation: replace an outstanding permission', 'testSecondPendingRequestCannotReplaceReviewedPaste'),
    ('generation', SURFACE, 'self.clipboardGeneration == generation, ', '', 'testOldApprovalCannotApproveANewerRequest'),
    ('hidden-cancel', SURFACE, 'if !visible { cancelClipboard() }', 'if false { cancelClipboard() }', 'testHiddenSurfaceRevokesItsPendingPaste'),
    ('cancel-denies', SURFACE, 'clipboardAlert = nil\n        decision?(false)', 'clipboardAlert = nil\n        decision?(true)', 'testChangingActivePaneRevokesItsPendingPaste'),
    ('paste-intent', SURFACE, 'operation: read ? .read : .paste', 'operation: read ? .read : .write', 'testUnsafeMenuPasteWaitsForApprovalAndUsesReviewedSnapshot'),
    ('write-intent', SURFACE, 'self.confirmClipboard(text: text, operation: .write)', 'self.confirmClipboard(text: text, operation: .paste)', 'testClipboardWriteAskKeepsWriteIntentAndDenialPreservesClipboard'),
    ('preview-budget', SURFACE, 'text.utf8.prefix(4096)', 'text.utf8.prefix(65536)', 'testNativePastePreviewUsesAByteBudgetForCombiningText'),
    ('sheet-intent', SURFACE, 'alert.messageText = "确认粘贴到当前终端？"', 'alert.messageText = "允许终端程序写入剪贴板？"', 'testNativePasteSheetShowsPasteIntentAndApprovesCapturedText'),
    ('copy-on-select', SURFACE, 'else { write() }', 'else { }', 'testCopyOnSelectWritesOnlyTheActualSelection'),
    ('main-menu-action', MENU, 'action: #selector(NSText.paste(_:)),', 'action: #selector(NSText.copy(_:)),', 'testProcessMenuUsesInjectedWindowResponderWithoutRetargetingItems'),
    ('main-menu-key', MENU, 'keyEquivalent: "v")', 'keyEquivalent: "")', 'testProcessMenuUsesInjectedWindowResponderWithoutRetargetingItems'),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', required=True, type=Path)
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    checkout = args.checkout.resolve(strict=True)
    if ROOT / 'tmp' not in checkout.parents or checkout == ROOT:
        parser.error('refusing to mutate outside an independent checkout below project tmp/')
    actual = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', '--show-toplevel'], text=True).strip()
    if Path(actual).resolve() != checkout:
        parser.error('a private checkout is required')
    work = Path(tempfile.mkdtemp(prefix='clipboard-mutations-', dir=checkout / 'tmp'))
    originals = {path: (checkout / path).read_bytes() for path in {case[1] for case in CASES}}
    inputs = {path: (checkout / path).read_bytes() for path in [
        TERMINAL + 'Tests/GhostermTerminalTests/TerminalPasteTests.swift',
        TERMINAL + 'Tests/GhostermTerminalTests/TerminalOSC52Tests.swift',
        APP + 'Tests/GhostermAppTests/ClipboardMenuRoutingTests.swift',
        TERMINAL + 'Sources/CGhostty/include/ghostty_host.h',
    ]}
    protected = {path: (ROOT / path).read_bytes() for path in originals}
    environment = {**os.environ, 'TMPDIR': str(checkout / 'tmp'), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1'}
    records = []

    def run(label, package, selection, red=False):
        command = ['swift', 'test', '--package-path', str(checkout / package), '--skip-update', '--filter', selection]
        if package == APP:
            command += ['--cache-path', str(checkout / 'tmp/spm-cache')]
        logfile = work / (label + '.log')
        with logfile.open('w') as output:
            result = subprocess.run(command, cwd=checkout, env=environment, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=180)
        output = logfile.read_text()
        if red:
            accepted = (result.returncode == 1 and 'Build complete!' in output and 'XCTAssert' in output
                        and re.search(r'Executed 1 test, with 1 failures? \(0 unexpected\)', output)
                        and not re.search(r'unexpected signal|Program crashed|AddressSanitizer|UndefinedBehaviorSanitizer|error: emit-module|error: fatalError', output))
        else:
            accepted = (result.returncode == 0 and re.search(r'Executed [1-9][0-9]* tests?, with 0 failures', output)
                        and not re.search(r'Executed 0 tests|Test Case .* skipped', output))
        if not accepted:
            raise RuntimeError(f'{label}: expected {"compiled single assertion red" if red else "green"}: {logfile}')
        records.append({'case': label, 'returncode': result.returncode, 'log': str(logfile.relative_to(checkout))})

    terminal_selection = 'TerminalPasteTests|TerminalClipboardTests|TerminalOSC52Tests'
    app_selection = 'ClipboardMenuRoutingTests|AppMenuCoverageTests|HostListTests'
    try:
        run('terminal-baseline', TERMINAL, terminal_selection)
        run('app-baseline', APP, app_selection)
        for name, filename, before, after, target in CASES:
            if args.case and name not in args.case:
                continue
            original = originals[filename].decode()
            if original.count(before) != 1:
                raise RuntimeError(f'{name}: expected exactly one mutation anchor')
            (checkout / filename).write_text(original.replace(before, after))
            menu = filename == MENU
            package, test_class = (APP, 'ClipboardMenuRoutingTests') if menu else (TERMINAL, 'TerminalPasteTests')
            control = 'HostListTests.testReadingDoesNotMutateConfig' if menu else 'TerminalSurfaceTests.testOutputReachesGhosttyGrid'
            run(name + '-red', package, test_class + '.' + target, True)
            run(name + '-control', package, control)
            (checkout / filename).write_bytes(originals[filename])
            run(name + '-restored', package, test_class + '.' + target)
            assert (checkout / filename).read_bytes() == originals[filename]
            for path, data in inputs.items():
                assert (checkout / path).read_bytes() == data
            print(f'PASS {name}: single assertion red / same-bundle independent control green / restored recompile / SHA256', flush=True)
        run('terminal-final', TERMINAL, terminal_selection)
        run('app-final', APP, app_selection)
    finally:
        for path, data in originals.items():
            (checkout / path).write_bytes(data)
        (work / 'receipt.json').write_text(json.dumps({
            'sources_sha256': {path: sha(data) for path, data in originals.items()},
            'inputs_sha256': {path: sha(data) for path, data in inputs.items()},
            'records': records,
            'scope': 'Private native checkout, fixture clipboard, offscreen windows; injected key/main-window lookup for process menu only',
        }, indent=2) + '\n')
        for path, data in inputs.items():
            assert (checkout / path).read_bytes() == data
        for path, data in protected.items():
            assert (ROOT / path).read_bytes() == data
    print('PASS macOS clipboard mutation scan: ' + str(work / 'receipt.json'), flush=True)


if __name__ == '__main__':
    main()
