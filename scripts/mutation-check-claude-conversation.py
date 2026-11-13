#!/usr/bin/env python3
"""Isolated Claude Native conversation and shared-control protection mutations."""
import hashlib
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MODEL = Path('apple/GhostermApp/Sources/GhostermAppModel')
CHAT = 'ClaudeConversation.swift'
JSON = 'ClaudeJSON.swift'
CONTROLS = '../GhostermApp/ConversationControls.swift'
MARK = 'GT_CLAUDE_CONVERSATION_MUTATION'
CONTROL = 'ClaudeRPCClientTests.testRelayRejectsInvalidPrefixAndEnvironmentBeforeLaunch'
CASES = [
    ('reference-bytes', CHAT, 'a.requestID.utf8.elementsEqual(b.requestID.utf8)', 'a.requestID == b.requestID',
     'ClaudeConversationTests.testReferencesUseExactBytesAndQuestionsNeverAcceptGenericAllow'),
    ('question-not-allow', CHAT, '!allow || !prompt.isQuestion', 'true',
     'ClaudeConversationTests.testReferencesUseExactBytesAndQuestionsNeverAcceptGenericAllow'),
    ('queued-live-reference', CHAT, 'try self.checkPrompt(reference)\n            let reply = try await self.call("approve"',
     '/* mutation: omit live reference check */\n            let reply = try await self.call("approve"',
     'ClaudeConversationTests.testQueuedApprovalRechecksAfterAnInFlightPollExpiresIt'),
    ('result-correlation', CHAT, 'activeMessageID?.utf8.elementsEqual(id.utf8) == true,\n                  let error',
     'true,\n                  let error', 'ClaudeConversationTests.testForeignResultKeepsInputUnknownAndWrongTurnCannotOfferApproval'),
    ('prompt-turn', CHAT, 'guard phase == .running, activeMessageID?.utf8.elementsEqual(prompt.turnID.utf8) == true else',
     'guard phase == .running else', 'ClaudeConversationTests.testForeignResultKeepsInputUnknownAndWrongTurnCannotOfferApproval'),
    ('interrupt-expiry', CHAT, 'self.phase = .interrupting; self.pending = []', 'self.phase = .interrupting',
     'ClaudeConversationTests.testInterruptReceiptDoesNotCompleteInputAndOldPromptCannotRun'),
    ('interrupt-not-result', CHAT, 'append(id: "receipt:\\(sessionSequence)", title:',
     'activeMessageID = nil; append(id: "receipt:\\(sessionSequence)", title:',
     'ClaudeConversationTests.testInterruptReceiptDoesNotCompleteInputAndOldPromptCannotRun'),
    ('owner-after-reply', CHAT, 'guard valid, expected == generation, isCurrent() else',
     'guard valid, expected == generation else', 'ClaudeConversationTests.testOwnerChangeDuringSendRejectsTheLateReply'),
    ('ordered-events', CHAT, 'event["sequence"]?.integer == sessionSequence + 1', 'true',
     'ClaudeConversationTests.testSequenceGapsAndBoundedHistoryRemainVisible'),
    ('record-bytes', CHAT, 'records.reduce(0, { $0 + $1.json.utf8.count }) > 2_097_152', 'false',
     'ClaudeConversationTests.testSequenceGapsAndBoundedHistoryRemainVisible'),
    ('entry-bytes', CHAT, 'entries.reduce(0, { $0 + $1.text.utf8.count }) > 2_097_152', 'false',
     'ClaudeConversationTests.testSequenceGapsAndBoundedHistoryRemainVisible'),
    ('unicode-answers', JSON, 'default: result.unicodeScalars.append(scalar)',
     'default: if scalar.value != 0x301 { result.unicodeScalars.append(scalar) }',
     'ClaudeConversationTests.testQuestionsNeedEveryExplicitAnswerAndPreserveUnicodeAndNumericKeys'),
    ('integer-question-keys', JSON, 'let ordered = numeric + members.filter { index($0.name) == nil }', 'let ordered = members',
     'ClaudeConversationTests.testQuestionsNeedEveryExplicitAnswerAndPreserveUnicodeAndNumericKeys'),
    ('rendered-allow-route', CONTROLS, 'Button(allowLabel, action: allow)', 'Button(allowLabel, action: deny)',
     'ClaudeConversationViewTests.testRenderedStartSendAndToolApprovalUseTheProductionControls'),
    ('readonly-unicode-update', CONTROLS, '!view.string.utf8.elementsEqual(text.utf8)', 'view.string != text',
     'CodexChangeReviewTests.testNativeTextUpdatePreservesExactUnicodeBytes'),
    ('startup-error-visible', CHAT, 'else if next != .closed { issue = "后端初始化或连接失败；请检查执行端配置和依赖。" }',
     'else if next != .closed { issue = nil }', 'ClaudeConversationTests.testStartupFailureIsVisibleWithoutAnActiveInput'),
]

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    cache = (ROOT / 'core/build/CMakeCache.txt').read_text()
    if 'GHOSTERM_WITH_PLUGIN_JS:BOOL=OFF' not in cache:
        raise SystemExit('Restore the default JS=OFF Core before these Native fixture mutations; real JS-enabled SSH cases run separately')
    originals = {file: (ROOT / MODEL / file).read_bytes() for file in sorted(set(case[1] for case in CASES))}
    for name, file, old, _, _ in CASES:
        if originals[file].count(old.encode()) != 1 or MARK.encode() in originals[file]:
            raise RuntimeError(name + ': mutation preflight mismatch')
    with tempfile.TemporaryDirectory(prefix='claude-conversation-mutations-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'source'; scratch = work / 'scratch'
        for package in ['GhostermCore', 'GhostermTerminal', 'GhostermApp']:
            shutil.copytree(ROOT / 'apple' / package, source / 'apple' / package,
                            symlinks=True, ignore=shutil.ignore_patterns('.build', '__pycache__'))
        # Only this new suite and the owned AppModel fixture are needed here.
        tests = source / 'apple/GhostermApp/Tests/GhostermAppTests'
        for entry in tests.iterdir():
            if entry.name not in ['ClaudeRPCClientTests.swift', 'ClaudeConversationTests.swift', 'ClaudeConversationViewTests.swift', 'CodexChangeReviewTests.swift', 'ConversationUITestSupport.swift', 'TestWindows.swift', 'AppModelFixture.swift']:
                if entry.is_dir(): shutil.rmtree(entry)
                else: entry.unlink()
        (source / 'build').symlink_to(ROOT / 'build', target_is_directory=True)
        (source / 'core').mkdir(); (source / 'core/build').symlink_to(ROOT / 'core/build', target_is_directory=True)
        shutil.copytree(ROOT / 'core/include', source / 'core/include')
        env = dict(os.environ, TMPDIR=str(work), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
        def test(target=None, red=False):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-swift; shift; gt_timeout 240 "$@"'
            command = ['swift', 'test', '--verbose', '--package-path', source / 'apple/GhostermApp', '--scratch-path', scratch,
                       '--filter', target or 'ClaudeConversationTests|ClaudeConversationViewTests|ClaudeRPCClientTests']
            result = subprocess.run(['bash', '-c', shell, 'claude-swift', str(ROOT), *map(str, command)],
                                    env=env, cwd=ROOT, text=True, capture_output=True, timeout=250)
            output = result.stdout + result.stderr
            if red:
                accepted = result.returncode == 1 and re.search(r'Executed 1 test, with 1 failure(?:s)? \(0 unexpected\)', output)
                accepted = accepted and ('XCTAssert' in output or 'failed - Claude' in output)
                accepted = accepted and not any(token in output for token in ['emit-module command failed', 'Fatal error:', 'unexpected signal'])
            else:
                accepted = result.returncode == 0 and f'Executed {1 if target else 24} test' in output and 'skipped' not in output.lower()
            if not accepted: raise RuntimeError(('Expected compiled XCTest assertion red' if red else 'Expected green suite') + '\n' + output)
            return output
        test(); print('BASELINE PASS: 24 Native conversation/view/document tests', flush=True)
        for name, file, old, new, target in CASES:
            path = source / MODEL / file
            try:
                path.write_bytes(originals[file].replace(old.encode(), new.encode(), 1) + ('\n// ' + MARK + '\n').encode())
                output = test(target, red=True)
                compiled = re.search(r'-primary-file\s+[\"\']?' + re.escape(str(path.resolve())), output)
                compiled = compiled or re.search(r'^(?:Compile|Compiling) ' + re.escape(Path(file).name) + r'$', output, re.M)
                if not compiled:
                    raise RuntimeError(name + ': source was not recompiled:\n' + output)
                test(CONTROL)
            finally: path.write_bytes(originals[file])
            test(target)
            if sha(path) != hashlib.sha256(originals[file]).hexdigest(): raise RuntimeError('restore digest mismatch')
            print(f'PASS {name}: compiled assertion red / independent XCTest green / restored green / SHA256', flush=True)
        test()
    if any((ROOT / MODEL / file).read_bytes() != original for file, original in originals.items()):
        raise RuntimeError('working production source changed')
    print(f'CLAUDE CONVERSATION PASS: {len(CASES)} isolated mutation groups; working sources unchanged')


def stop(number, frame): raise KeyboardInterrupt(str(number))


if __name__ == '__main__':
    signal.signal(signal.SIGINT, stop); signal.signal(signal.SIGTERM, stop)
    main()
