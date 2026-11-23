#!/usr/bin/env python3
"""Device checks for Android Claude document RPC and Native launch configuration; no SDK/model."""
import argparse
import hashlib
import importlib.util
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
SOURCE = Path('android/app/src/main/java/com/hanxi/ghosterm')
CASES = [
    ('fixed-argv', 'ExtensionRelayBootstrap.kt', 'it.prefixArguments.forEach { argument -> add("--process-argument"); add(it.name); add(argument) }', '',
     'ClaudeLaunchTest#fixedArgumentsAndRemoteEnvironmentNamesAreQuotedInOrder'),
    ('remote-environment', 'ExtensionRelayBootstrap.kt', 'request.processEnvironmentNames.forEach { add("--process-environment"); add(it) }', '',
     'ClaudeLaunchTest#fixedArgumentsAndRemoteEnvironmentNamesAreQuotedInOrder'),
    ('prefix-count', 'ExtensionRelayBootstrap.kt', 'program.prefixArguments.size <= 16', 'true', 'ClaudeLaunchTest#invalidPrefixArgumentsCannotReachTheRemoteCommand'),
    ('prefix-bytes', 'ExtensionRelayBootstrap.kt', 'program.prefixArguments.sumOf { it.toByteArray().size + 1 } <= 16384', 'true', 'ClaudeLaunchTest#invalidPrefixArgumentsCannotReachTheRemoteCommand'),
    ('environment-default', 'ExtensionRelayBootstrap.kt', '&& it !in defaults', '', 'ClaudeLaunchTest#environmentNamesRequireMappingAndNeverAcceptValuesOrDefaults'),
    ('form-environment', 'SettingsContent.kt', 'ExtensionRelayBootstrap.parseProcessEnvironmentJson(processEnvironmentJson)', 'emptyList()', 'ClaudeLaunchTest#productionLaunchFormKeepsFixedArgumentsAndEnvironmentSelection'),
    ('page-identity', 'ClaudeRPCClient.kt', 'string(page, "responseID") == responseID', 'true', 'ClaudeRPCClientTest#pageIdentityOffsetsCountsAndCompletionMustMatch'),
    ('page-offset', 'ClaudeRPCClient.kt', 'count(page, "offset") == received', 'true', 'ClaudeRPCClientTest#pageIdentityOffsetsCountsAndCompletionMustMatch'),
    ('page-done', 'ClaudeRPCClient.kt', 'boolean(page, "done") == (received + content.length == units)', 'true', 'ClaudeRPCClientTest#pageIdentityOffsetsCountsAndCompletionMustMatch'),
    ('child-identity', 'ClaudeRPCClient.kt', 'string(response, "id") == "command-$next"', 'true', 'ClaudeRPCClientTest#childIdentityDuplicateFieldsAndUnknownShapesMustBeRejected'),
    ('child-fields', 'ClaudeRPCClient.kt', 'response.keys().asSequence().toSet() == expected', 'true', 'ClaudeRPCClientTest#childIdentityDuplicateFieldsAndUnknownShapesMustBeRejected'),
    ('late-reply', 'ClaudeRPCClient.kt', 'check(valid) { "Claude RPC scope closed" }', 'check(true) { "Claude RPC scope closed" }', 'ClaudeRPCClientTest#invalidationRejectsLateRepliesAndBusyCannotSendAnotherAction'),
    ('uncertain-replay', 'ClaudeRPCClient.kt', 'valid = false\n            throw failure', 'throw failure', 'ClaudeRPCClientTest#uncertainOperationClosesScopeAndNeverRetries'),
    ('duplicate-json', 'ClaudeJson.kt', 'require(seen.add(name))', 'require(true)', 'ClaudeRPCClientTest#childIdentityDuplicateFieldsAndUnknownShapesMustBeRejected'),
    ('json-depth', 'ClaudeJson.kt', 'depth <= 40', 'true', 'ClaudeRPCClientTest#strictJsonPreservesUnicodeKeysAndRejectsStructureBudgets'),
    ('json-nodes', 'ClaudeJson.kt', '++nodes <= 10000', 'true', 'ClaudeRPCClientTest#strictJsonPreservesUnicodeKeysAndRejectsStructureBudgets'),
]


CONVERSATION_CASES = [
    ('chat-confirmed-send-draft', 'ClaudeConversation.kt', 'draft = if (current.draft == text) "" else current.draft',
     'draft = ""', 'ClaudeConversationTest#confirmedSendKeepsTheDraftEditedWhileWaiting'),
    ('chat-question-not-allow', 'ClaudeConversation.kt', 'allow && prompt.isQuestion', 'false', 'ClaudeConversationTest#questionsNeedEveryAnswerAndJavaScriptKeyOrder'),
    ('chat-all-answers', 'ClaudeConversation.kt', 'answers.any { it.isEmpty() || ClaudeJson.utf8(it).size > 2048 }', 'false', 'ClaudeConversationTest#questionsNeedEveryAnswerAndJavaScriptKeyOrder'),
    ('chat-queued-reference', 'ClaudeConversation.kt', 'checkPrompt(reference)\n            val reply = call("approve"', 'val reply = call("approve"', 'ClaudeConversationTest#queuedDecisionRechecksExpiryAfterAnInFlightPoll'),
    ('chat-result-identity', 'ClaudeConversation.kt', 'id != current.activeMessageID', 'false', 'ClaudeConversationTest#foreignResultsAndWrongTurnPromptsKeepTheInputUnknown'),
    ('chat-prompt-turn', 'ClaudeConversation.kt', 'require(current.phase == Phase.RUNNING && prompt.turnID == current.activeMessageID)', 'require(current.phase == Phase.RUNNING)', 'ClaudeConversationTest#foreignResultsAndWrongTurnPromptsKeepTheInputUnknown'),
    ('chat-interrupt-expiry', 'ClaudeConversation.kt', 'current.copy(phase = Phase.INTERRUPTING, pending = emptyList())', 'current.copy(phase = Phase.INTERRUPTING)', 'ClaudeConversationTest#interruptReceiptDoesNotCompleteTheInput'),
    ('chat-interrupt-not-result', 'ClaudeConversation.kt', 'append("receipt:$sessionSequence",', 'current = current.copy(activeMessageID = null); append("receipt:$sessionSequence",', 'ClaudeConversationTest#interruptReceiptDoesNotCompleteTheInput'),
    ('chat-owner-after-reply', 'ClaudeConversation.kt', 'generation !== expected || !isCurrent()', 'generation !== expected', 'ClaudeConversationTest#ownerChangeRejectsLateSendAndKeepsTheDraft'),
    ('chat-ordered-events', 'ClaudeConversation.kt', '(sequence as Number).toLong() == sessionSequence + 1', 'true', 'ClaudeConversationTest#outOfOrderEventsFailObservationAndExpiredDecisionIsNotRetried'),
    ('chat-record-bytes', 'ClaudeConversation.kt', 'records.sumOf { it.byteCount } > 2097152', 'false', 'ClaudeConversationTest#historyCountsBytesAndKeepsEmptyPollsOutOfRawRecords'),
    ('chat-entry-bytes', 'ClaudeConversation.kt', 'entries.sumOf { it.byteCount } > 2097152', 'false', 'ClaudeConversationTest#historyCountsBytesAndKeepsEmptyPollsOutOfRawRecords'),
    ('chat-record-count', 'ClaudeConversation.kt', 'records.size > 128', 'false', 'ClaudeConversationTest#historyCountsBytesAndKeepsEmptyPollsOutOfRawRecords'),
    ('chat-entry-count', 'ClaudeConversation.kt', 'entries.size > 128', 'false', 'ClaudeConversationTest#historyCountsBytesAndKeepsEmptyPollsOutOfRawRecords'),
    ('chat-question-projection', 'ClaudeConversation.kt', 'if (isQuestion) require(questions(input.getJSONArray("questions")) == questions)', 'if (false) require(questions(input.getJSONArray("questions")) == questions)', 'ClaudeConversationTest#questionProjectionCannotChangeTheOriginalInput'),
    ('chat-startup-error', 'ClaudeConversation.kt', 'else if (next != Phase.CLOSED) "后端初始化或连接失败；请检查执行端配置和依赖。" else current.issue', 'else current.issue', 'ClaudeConversationTest#closedStartupIsVisibleWithoutAnyInputAndLocaleDoesNotChangePhases'),
    ('chat-integer-keys', 'ClaudeJson.kt', 'val ordered = keys.filter { index(it) != null }.sortedBy { index(it) } + keys.filter { index(it) == null }', 'val ordered = keys', 'ClaudeConversationTest#questionsNeedEveryAnswerAndJavaScriptKeyOrder'),
    ('chat-unicode-keys', 'ClaudeJson.kt', 'value.forEach { character ->', 'value.replace("\\u0301", "").forEach { character ->', 'ClaudeConversationTest#questionsNeedEveryAnswerAndJavaScriptKeyOrder'),
    ('chat-allow-button', 'ConversationControls.kt', 'TextButton(onClick = allow,', 'TextButton(onClick = deny,', 'ClaudeConversationUiTest#realStartSendAndToolButtonsApproveTheImmutableInputOnce'),
    ('chat-no-default-answer', 'ClaudeConversationContent.kt', 'val selected: Set<Int> = emptySet()', 'val selected: Set<Int> = setOf(0)', 'ClaudeConversationUiTest#everyQuestionStartsUnselectedAndFreeTextRequiresAnExplicitAnswer'),
    ('chat-busy-ui', 'ClaudeConversationContent.kt', 'ClaudePrompt(model, prompt, model.canRespond(prompt.reference))', 'ClaudePrompt(model, prompt, true)', 'ClaudeConversationUiTest#realStartSendAndToolButtonsApproveTheImmutableInputOnce'),
    ('chat-readonly', 'ConversationControls.kt', 'readOnly = true', 'readOnly = false', 'CodexChangeReviewUiTest#fullDiffIsSelectableReadOnlyAndLastFileIsReachable'),
]


OBSERVATION_CASES = [
    ('observe-input', 'ClaudeRPCClient.kt', 'observedInput == inputID', 'true', 'ClaudeObservationTest#foreignInputOrResponseCannotBeAdopted'),
    ('observe-response', 'ClaudeRPCClient.kt', 'lastResponseID == null || lastResponseID == responseID', 'true', 'ClaudeObservationTest#foreignInputOrResponseCannotBeAdopted'),
    ('observe-first-binding', 'ClaudeRPCClient.kt', 'lastResponseID = responseID\n            val first', 'val first', 'ClaudeObservationTest#failedReadCannotChangeTheFirstResponseIdentity'),
    ('observe-before-forget', 'ClaudeRPCClient.kt', 'completeResponse = raw.copyOf()\n            val forgotten', 'completeResponse = null\n            val forgotten', 'ClaudeObservationTest#lostForgetUsesValidatedBytesWithoutAnotherRPC'),
    ('observe-next-reset', 'ClaudeRPCClient.kt', 'lastInputID = null; lastResponseID = null; completeResponse = null', '/* old identity retained */', 'ClaudeObservationTest#failedNewOpenCannotReadPreviousIdentityOrCache'),
    ('observe-scope', 'ClaudeRPCClient.kt', 'check(scopeOpen)', 'check(true)', 'ClaudeObservationTest#invalidationRejectsObservationBeforeAndAfterAwait'),
    ('observe-child-sequence', 'ClaudeRPCClient.kt', 'integer(response, "sequence") == next', 'true', 'ClaudeObservationTest#observedPagesAndChildIdentityKeepAllChecks'),
    ('observe-immutable', 'ClaudeRPCClient.kt', 'val response get() = record?.copyOf()', 'val response get() = record', 'ClaudeObservationTest#observedByteArraysCannotMutateTheRetainedRecord'),
    ('observe-exit-value', 'ClaudeRPCClient.kt', 'exitValue.length() == 1', 'true', 'ClaudeObservationTest#malformedExitObservationCannotAuthorizeRead'),
    ('observe-owner', 'ClaudeConversation.kt', 'check(!detached && generation === expected && isCurrent())', 'check(!detached && generation === expected)', 'ClaudeObservationUiTest#detachedOrReplacedOwnerRejectsLateObservation'),
    ('observe-unknown', 'ClaudeConversation.kt', 'current.copy(observation = result)', 'current.copy(observation = result, phase = Phase.READY)', 'ClaudeObservationUiTest#observationNeverResumesOrClearsTheDraft'),
    ('observe-button', 'ClaudeConversationContent.kt', 'onClick = model::observeOutcome', 'onClick = model::send', 'ClaudeObservationUiTest#actualObserveButtonShowsCompleteSelectableReadonlyReceipt'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--android')
    target.add_argument('--headless-api', choices=['29', '36'])
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--observation', action='store_true', help='include unknown-outcome observation checks and Native controls')
    parser.add_argument('--conversation', action='store_true', help='include Native conversation state, controls and shared diff regressions')
    parser.add_argument('--plugin-lua', action='store_true', help='compile the Lua-enabled Android Host profile')
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES + CONVERSATION_CASES + OBSERVATION_CASES])
    args = parser.parse_args()
    if args.observation: args.conversation = True
    if args.case and not args.mutate: parser.error('--case requires --mutate')
    if args.case and not args.observation and any(name.startswith('observe-') for name in args.case):
        parser.error('observe cases require --observation')
    if args.case and not args.conversation and any(name.startswith('chat-') for name in args.case):
        parser.error('chat mutation cases require --conversation')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    spec = importlib.util.spec_from_file_location('owned_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
    headless = importlib.util.module_from_spec(spec); spec.loader.exec_module(headless)
    if args.headless_api:
        with headless.headless(args.headless_api) as serial: execute(args, serial)
    else: execute(args, args.android)


def execute(args, serial):
    jdk = ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'
    sdk = ROOT / 'build/android-toolchain/sdk'
    env = dict(os.environ, JAVA_HOME=str(jdk), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk))
    adb = [str(sdk / 'platform-tools/adb'), '-s', serial]
    before = {path: path.read_bytes() for path in (ROOT / SOURCE).glob('*.kt')}
    with tempfile.TemporaryDirectory(prefix='claude-android-check-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        env.update(TMPDIR=str(work), JAVA_TOOL_OPTIONS=f'-Djava.io.tmpdir={work} -Djava.awt.headless=true')
        def run(command, seconds=180):
            shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require claude-android; shift; gt_timeout "$@"'
            result = subprocess.run(['bash', '-c', shell, 'claude-android', str(ROOT), str(seconds), *map(str, command)],
                cwd=ROOT, env=env, capture_output=True, text=True)
            output = result.stdout + result.stderr
            if result.returncode: raise RuntimeError(f'Command failed: {command}\n{output}')
            return output
        print('DEVICE: ' + run([*adb, 'shell', 'getprop', 'ro.build.version.sdk']).strip() + '; ' +
            run([*adb, 'shell', 'getprop', 'ro.product.cpu.abi']).strip() + '; page ' + run([*adb, 'shell', 'getconf', 'PAGESIZE']).strip(), flush=True)

        source = ROOT
        if args.mutate:
            source = work / 'source'
            for package in ['android', 'terminal/android']:
                shutil.copytree(ROOT / package, source / package,
                    ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
            for shared in ['core', 'vendor', 'build', 'assets', 'patches', 'scripts']:
                (source / shared).symlink_to(ROOT / shared, target_is_directory=True)
        def build(label):
            lua = ['-PghostermPluginLua=true', '-PghostermLuaArchive=' + str(Path(os.environ['GHOSTERM_LUA_ARCHIVE']).resolve(strict=True))] if args.plugin_lua else ['-PghostermPluginLua=false']
            output = run([ROOT / 'android/gradlew', '-p', source / 'android', *lua,
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'], 900)
            (work / (label + '-build.log')).write_text(output)
            if 'BUILD SUCCESSFUL' not in output: raise RuntimeError('Android build did not complete')
            if label.endswith('-mutant') and not re.search(r'^> Task :app:compileDebugKotlin\s*$', output, re.M):
                raise RuntimeError('Mutation Kotlin source did not recompile:\n' + output)
            for name in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
                apk = source / 'android/app/build/outputs/apk' / name
                run([*adb, 'install', '-r', '-t', apk])
                package = 'com.hanxi.ghosterm.test' if 'androidTest/' in name else 'com.hanxi.ghosterm'
                installed = run([*adb, 'shell', 'pm', 'path', package]).strip().removeprefix('package:')
                digest = run([*adb, 'shell', 'sha256sum', installed]).split()[0]
                if digest != hashlib.sha256(apk.read_bytes()).hexdigest(): raise RuntimeError('Installed APK digest mismatch')
        def test(label, target=None, red=False):
            classes = ['ClaudeRPCClientTest', 'ClaudeLaunchTest'] + (['ClaudeConversationTest', 'ClaudeConversationUiTest', 'CodexChangeReviewUiTest'] if args.conversation else []) + (['ClaudeObservationTest', 'ClaudeObservationUiTest'] if args.observation else [])
            selected = 'com.hanxi.ghosterm.' + target if target else ','.join('com.hanxi.ghosterm.' + name for name in classes)
            output = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selected,
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
            (work / (label + '-test.log')).write_text(output)
            count = 1 if target else (49 if args.observation else 33 if args.conversation else 13)
            valid = (re.search(r'Tests run: 1,\s+Failures: 1', output) and
                any(mark in output for mark in ['java.lang.AssertionError', 'org.junit.ComparisonFailure'])) if red else f'OK ({count} test' in output
            if red and any(mark in output for mark in ['TimeoutException', 'AppNotIdleException', 'Process crashed', 'timed out']):
                valid = False
            if not valid or f'INSTRUMENTATION_STATUS: numtests={count}\n' not in output or any(mark in output for mark in
                ['initializationError', 'INSTRUMENTATION_FAILED', 'INSTRUMENTATION_STATUS_CODE: -4', 'assumption failed:', 'FATAL EXCEPTION']):
                expected = 'ordinary single assertion red' if red else 'executed green suite'
                raise RuntimeError(label + ': expected ' + expected + '\n' + output)
            return output
        build('baseline'); test('baseline'); print(f'BASELINE PASS: {49 if args.observation else 33 if args.conversation else 13} device RPC/launch/conversation checks', flush=True)
        for name, file, old, new, target in (CASES + (CONVERSATION_CASES if args.conversation else []) + (OBSERVATION_CASES if args.observation else [])) if args.mutate else []:
            if args.case and name not in args.case: continue
            path = source / SOURCE / file; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError(name + ': mutation anchor mismatch')
            try:
                path.write_bytes(original.replace(old.encode(), new.encode(), 1))
                build(name + '-mutant'); test(name + '-red', target, True)
                test(name + '-control', 'NativeSmokeTest#configurationRejectsNul')
            finally: path.write_bytes(original)
            build(name + '-restore'); test(name + '-restore', target)
            if hashlib.sha256(path.read_bytes()).digest() != hashlib.sha256(original).digest():
                raise RuntimeError('Restored source SHA256 differs')
            print('PASS ' + name + ': compiled single assertion red / same APK Core green / restored green / SHA256', flush=True)
        if args.mutate: test('final')
        run([*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
    if any(path.read_bytes() != value for path, value in before.items()): raise RuntimeError('Original production sources changed')
    print('ANDROID CLAUDE RPC PASS: no SDK/CLI/model request; explicit document fixture', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
