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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--android')
    target.add_argument('--headless-api', choices=['29', '36'])
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--plugin-js', action='store_true', help='also compile the optional JS-enabled Android Host profile')
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    if args.case and not args.mutate: parser.error('--case requires --mutate')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    spec = importlib.util.spec_from_file_location('owned_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
    headless = importlib.util.module_from_spec(spec); spec.loader.exec_module(headless)
    if args.headless_api:
        with headless.headless(args.headless_api) as serial: execute(args, serial)
    else: execute(args, args.android)


def execute(args, serial):
    jdk = ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'
    sdk = ROOT / 'build/android-toolchain/sdk'
    env = dict(os.environ, JAVA_HOME=str(jdk), ANDROID_HOME=str(sdk), ANDROID_SDK_ROOT=str(sdk),
        TMPDIR=str(ROOT / 'tmp'), JAVA_TOOL_OPTIONS='-Djava.io.tmpdir=' + str(ROOT / 'tmp'))
    adb = [str(sdk / 'platform-tools/adb'), '-s', serial]
    before = {path: path.read_bytes() for path in (ROOT / SOURCE).glob('*.kt')}
    with tempfile.TemporaryDirectory(prefix='claude-android-check-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
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
            for shared in ['core', 'vendor', 'build', 'assets', 'packages', 'scripts']:
                (source / shared).symlink_to(ROOT / shared, target_is_directory=True)
        def build(label):
            output = run([ROOT / 'android/gradlew', '-p', source / 'android', '-PghostermPluginJs=' + str(args.plugin_js).lower(),
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
            selected = 'com.hanxi.ghosterm.' + target if target else ','.join('com.hanxi.ghosterm.' + name for name in ['ClaudeRPCClientTest', 'ClaudeLaunchTest'])
            output = run([*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selected,
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
            (work / (label + '-test.log')).write_text(output)
            count = 1 if target else 13
            valid = (re.search(r'Tests run: 1,\s+Failures: 1', output) and
                any(mark in output for mark in ['java.lang.AssertionError', 'org.junit.ComparisonFailure'])) if red else f'OK ({count} test' in output
            if red and any(mark in output for mark in ['TimeoutException', 'AppNotIdleException', 'Process crashed', 'timed out']):
                valid = False
            if not valid or f'INSTRUMENTATION_STATUS: numtests={count}\n' not in output or any(mark in output for mark in
                ['initializationError', 'INSTRUMENTATION_FAILED', 'INSTRUMENTATION_STATUS_CODE: -4', 'assumption failed:', 'FATAL EXCEPTION']):
                expected = 'ordinary single assertion red' if red else 'executed green suite'
                raise RuntimeError(label + ': expected ' + expected + '\n' + output)
            return output
        build('baseline'); test('baseline'); print('BASELINE PASS: 13 device RPC/launch/form checks', flush=True)
        for name, file, old, new, target in CASES if args.mutate else []:
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
