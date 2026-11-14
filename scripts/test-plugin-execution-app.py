#!/usr/bin/env python3
"""Native Executor runtime selection/approval checks, using isolated mutations and owned headless AVDs.

No account, model, remote user host, dependency installation or foreground window.
The shell test uses an owned argv probe; these tests do not claim actual Node/Exec SSH activation.
"""
import argparse
from contextlib import nullcontext
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
SWIFT = Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionRelayBootstrap.swift')
KOTLIN = Path('android/app/src/main/java/com/hanxi/ghosterm/ExtensionRelayBootstrap.kt')
SWIFT_CASES = [
    ('approval', SWIFT, 'guard execution.authorized else', 'guard true else', 'testExecRequiresSeparateExecutionApproval'),
    ('exec-selection', SWIFT, '["--runtime", "exec", "--allow-process-execution"]', '["--runtime", "portable-js", "--allow-process-execution"]', 'testSelectedRuntimeEmitsPinnedDependenciesAndEnvironmentWithoutBusinessGrants'),
    ('sdk-identity', SWIFT, '"--node-sdk", node.sdkDirectory, node.sdkCatalogSHA256', '"--node-sdk", node.sdkDirectory, node.sha256', 'testSelectedRuntimeEmitsPinnedDependenciesAndEnvironmentWithoutBusinessGrants'),
    ('tmpdir', SWIFT, 'let environment = request.execution.map { "TMPDIR=" + quote($0.temporaryDirectory) + " " } ?? ""', 'let environment = ""', 'testActualPOSIXCommandPreservesQuotedDependencyPathsAndTemporaryDirectory'),
    ('version', SWIFT, 'node.version.utf8.first.map({ (48...57).contains($0) }) == true', 'true', 'testInvalidRuntimeDependenciesFailBeforeSSH'),
    ('command-budget', SWIFT, 'guard command.utf8.count <= 1024 else', 'guard true else', 'testInvalidRuntimeDependenciesFailBeforeSSH'),
    ('edit-approval', Path('apple/GhostermApp/Sources/GhostermApp/ExecutorRuntimeFields.swift'), 'var temporaryDirectory = "" { didSet { authorized = false } }', 'var temporaryDirectory = ""', 'testExecutionDraftRevokesApprovalOnEveryDependencyEditAndRuntimeChange'),
]
LOCAL_SWIFT_CASES = [
    ('local-approval', Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionLocalLaunch.swift'), 'self.authorized = authorized', 'self.authorized = true', 'LocalExtensionTests.testLocalWorkerRejectsMissingApprovalBeforeExecution'),
    ('local-digest', Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionLocalLaunch.swift'), 'digest == executableSHA256', 'true', 'LocalExtensionTests.testDigestAndEnvironmentRejectBeforeExecution'),
    ('local-cwd', Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionLocalLaunch.swift'), 'let directory = try openDirectory(workingDirectory)', 'let directory = try openDirectory(temporaryDirectory)', 'LocalExtensionTests.testLocalCloseAwaitsReapAndKeepsFinalObservation'),
    ('local-loader', Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'), '$0 != "NODE_OPTIONS"', 'true', 'LocalExtensionTests.testLoaderEnvironmentIsRejectedWhenValueExists'),
    ('local-arguments', SWIFT, 'argv.append(contentsOf: request.arguments)', 'argv.append(contentsOf: request.arguments.reversed())', 'LocalExtensionTests.testLocalArgumentsKeepRawBytesWithoutSSHLimit'),
    ('local-count', SWIFT, 'argv.count <= 64', 'true', 'LocalExtensionTests.testLocalArgumentCountIsNotSilentlyTruncated'),
    ('local-final', Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'), 'guard released else { throw local ? ControllerError.localProcessDraining : ControllerError.channelUnavailable }\n                self.channel = nil', 'guard released else { throw local ? ControllerError.localProcessDraining : ControllerError.channelUnavailable }\n                self.channel = nil; self.lastLocalProcess = nil', 'LocalExtensionTests.testLocalCloseAwaitsReapAndKeepsFinalObservation'),
    ('local-retry', Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift'), 'guard ProcessInfo.processInfo.systemUptime < deadline else { throw ControllerError.localProcessDraining }', 'guard false else { throw ControllerError.localProcessDraining }', 'LocalExtensionTests.testTermIgnoreUsesFiniteCloseRetries'),
    ('local-form', Path('apple/GhostermApp/Sources/GhostermApp/ExtensionsPane.swift'), 'var localDigest = "" { didSet { localAllowed = false; clientAuthorized = false; attachmentAllowed = false } }', 'var localDigest = "" { didSet { clientAuthorized = false; attachmentAllowed = false } }', 'LocalExtensionTests.testLocalFormEditsRevokeCapturedApproval'),
]
ANDROID_CASES = [
    ('approval', KOTLIN, 'require(execution.authorized)', 'require(true)', 'nodeAndExecRequireSeparateApprovalAndNeverGrantBusinessProcesses'),
    ('exec-selection', KOTLIN, 'add("exec"); add("--allow-process-execution")', 'add("portable-js"); add("--allow-process-execution")', 'nodeAndExecRequireSeparateApprovalAndNeverGrantBusinessProcesses'),
    ('sdk-identity', KOTLIN, 'add("--node-sdk"); add(sdkDirectory); add(sdkCatalogSHA256)', 'add("--node-sdk"); add(sdkDirectory); add(sha256)', 'nodeAndExecRequireSeparateApprovalAndNeverGrantBusinessProcesses'),
    ('tmpdir', KOTLIN, 'val environment = request.execution?.let { "TMPDIR=" + quote(it.temporaryDirectory) + " " }.orEmpty()', 'val environment = ""', 'quotedDependenciesAndTMPDIRRemainLiteral'),
    ('version', KOTLIN, 'version.matches(Regex("[0-9][0-9A-Za-z.+-]*"))', 'true', 'invalidDependenciesAndTemporaryDirectoriesAreRejected'),
    ('command-budget', KOTLIN, 'require(it.toByteArray().size <= 1024)', 'require(true)', 'invalidDependenciesAndTemporaryDirectoriesAreRejected'),
    ('form-execution', Path('android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'), 'parseProcessEnvironmentJson(processEnvironmentJson), execution)', 'parseProcessEnvironmentJson(processEnvironmentJson), null)', 'productionFormRequiresApprovalAndCapturesExecWithoutNodeDependencies'),
    ('selection-identity', Path('android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'), 'remember(entry.review.id, entry.review.digest, command.path,\n        selectedHost, executable, executorDirectory, executorDigest,', 'remember(selectedHost, executable, executorDirectory, executorDigest,', 'productionFormRevokesApprovalOnDependencyEditsAndRuntimeSwitch'),
    ('edit-approval', Path('android/app/src/main/java/com/hanxi/ghosterm/SettingsContent.kt'), 'var executionAllowed by remember(entry.review.id, entry.review.digest, command.path,\n        selectedHost, executable, executorDirectory, executorDigest,\n        workspaceRootDirectory, argumentsJson, methodsJson, hostMethodsJson, processProgramsJson, processEnvironmentJson,\n        runtimeChoice, executionTmpdir, nodeExecutable, nodeVersion, nodeSha256, nodeSDKDirectory, nodeSDKDigest)', 'var executionAllowed by remember', 'productionFormRevokesApprovalOnDependencyEditsAndRuntimeSwitch'),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args, serial=None):
    inputs = {str(p.relative_to(ROOT)): sha(p) for directory in ['apple/GhostermApp/Sources', 'apple/GhostermApp/Tests',
        'apple/GhostermCore/Sources', 'apple/GhostermTerminal/Sources', 'android/app/src', 'core/include']
        for p in (ROOT / directory).rglob('*') if p.is_file()}
    inputs['scripts/fixtures/plugin-local-idle.c'] = sha(ROOT / 'scripts/fixtures/plugin-local-idle.c')
    with tempfile.TemporaryDirectory(prefix='execution-app-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); logs = work / 'logs'; logs.mkdir(); commands = []
        source = ROOT
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
            GHOSTERM_AUTOMATION_OFFSCREEN='1', JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
            ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'), GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))
        env.pop('GHOSTERM_CORE_TEST_PLUGIN_JS', None)
        if args.mutate:
            source = work / 'source'; source.mkdir(); (source / 'tmp').mkdir()
            if args.macos:
                for folder in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal']:
                    shutil.copytree(ROOT / folder, source / folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
                (source / 'core').mkdir(); shutil.copytree(ROOT / 'core/include', source / 'core/include', symlinks=True)
                (source / 'core/build').symlink_to(ROOT / 'core/build', target_is_directory=True)
                for name in ['build', 'patches']:
                    (source / name).symlink_to(ROOT / name, target_is_directory=True)
            else:
                for folder in ['android', 'terminal/android']:
                    shutil.copytree(ROOT / folder, source / folder,
                        ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
                for name in ['core', 'vendor', 'build', 'assets', 'packages', 'scripts']:
                    (source / name).symlink_to(ROOT / name, target_is_directory=True)
        def run(label, command, *, red=False, timeout=900):
            result = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, text=True, timeout=timeout)
            output = result.stdout + result.stderr; (logs / (label + '.log')).write_text(output)
            commands.append(dict(label=label, returncode=result.returncode))
            if result.returncode and not red: raise RuntimeError(label + '\n' + output)
            return result.returncode, output
        if args.macos and args.local:
            probe = work / 'local-idle'
            run('idle-probe', ['xcrun', '--sdk', 'macosx', 'clang', '-std=c11', '-Wall', '-Wextra', '-Werror', ROOT / 'scripts/fixtures/plugin-local-idle.c', '-o', probe])
            env['GHOSTERM_LOCAL_IDLE_CHILD'] = str(probe)
        adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial] if serial else []
        if adb:
            metadata = {name: run(name, [*adb, 'shell', *command])[1].strip() for name, command in [
                ('api', ['getprop', 'ro.build.version.sdk']), ('abi', ['getprop', 'ro.product.cpu.abi']), ('pagesize', ['getconf', 'PAGESIZE'])]}
            metadata['serial'] = serial
            print('DEVICE ' + json.dumps(metadata), flush=True)
        def build(label):
            if args.macos: return  # swift test below builds each exact source mutation.
            _, output = run(label + '-build', [ROOT / 'android/gradlew', '-p', source / 'android', '-PghostermPluginJs=true',
                ':app:assembleDebug', ':app:assembleDebugAndroidTest'])
            if 'BUILD SUCCESSFUL' not in output: raise RuntimeError('Android build did not complete')
            if label.endswith('-mutant') and not re.search(r'^> Task :app:compileDebugKotlin\s*$', output, re.M):
                raise RuntimeError('Mutated Kotlin was not actually compiled')
            for apk in ['debug/app-debug.apk', 'androidTest/debug/app-debug-androidTest.apk']:
                path = source / 'android/app/build/outputs/apk' / apk
                run(label + '-install-' + path.name, [*adb, 'install', '-r', '-t', path])
                package = 'com.hanxi.ghosterm.test' if 'androidTest/' in apk else 'com.hanxi.ghosterm'
                installed = run(label + '-installed-' + path.name, [*adb, 'shell', 'pm', 'path', package])[1].strip().removeprefix('package:')
                digest = run(label + '-sha-' + path.name, [*adb, 'shell', 'sha256sum', installed])[1].split()[0]
                if digest != sha(path): raise RuntimeError('Installed APK differs')
        def test(label, target=None, red=False, control=False, compiled_file=None):
            if args.macos:
                selected = 'ExtensionControllerTests.testClientHostProfileOnlyAdvertisesIntegratedCapabilities' if control else (target if target and '.' in target else 'ExecutorExecutionTests.' + target if target else 'LocalExtensionTests|ExecutorExecutionTests' if args.local else 'ExecutorExecutionTests')
                objects = source / 'apple/GhostermApp/.build'
                before_objects = {str(p): sha(p) for p in objects.rglob(compiled_file.stem + '.o')} if compiled_file else {}
                code, output = run(label, ['swift', 'test', '--package-path', source / 'apple/GhostermApp', '--filter', selected], red=red, timeout=300)
                count = 1 if target or control else 16 if args.local else 7
                assertion = 'XCTAssert' in output or re.search(r': error: .* : failed - [^\n]+', output)
                valid = (code == 1 and 'Executed 1 test, with 1 failure (0 unexpected)' in output and assertion) if red else (
                    code == 0 and f'Executed {count} test' in output and 'with 0 failures' in output)
                if red and not any(sha(p) != before_objects.get(str(p)) for p in objects.rglob(compiled_file.stem + '.o')):
                    raise RuntimeError('Mutated Swift object did not change: ' + str(compiled_file) + '\n' + output)
            else:
                selected = 'com.hanxi.ghosterm.NativeSmokeTest#configurationRejectsNul' if control else 'com.hanxi.ghosterm.ExecutorExecutionTest' + ('#' + target if target else '')
                code, output = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', selected,
                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                count = 1 if target or control else 6
                valid = (re.search(r'Tests run: 1,\s+Failures: 1', output) and any(x in output for x in
                    ['java.lang.AssertionError', 'org.junit.ComparisonFailure'])) if red else f'OK ({count} test' in output
                valid = valid and f'INSTRUMENTATION_STATUS: numtests={count}\n' in output
            if not valid or any(x in output for x in ['unexpected signal', 'Process crashed', 'Precondition failed',
                'emit-module command failed', 'TimeoutException', 'AppNotIdleException', 'INSTRUMENTATION_FAILED',
                'INSTRUMENTATION_STATUS_CODE: -4', 'assumption failed:', 'FATAL EXCEPTION', 'skipped']):
                raise RuntimeError(label + ': expected ' + ('normal single assertion red' if red else 'complete green') + '\n' + output)
        build('baseline'); test('baseline'); print('BASELINE PASS', flush=True)
        groups = []
        for name, file, old, new, target in ((SWIFT_CASES + LOCAL_SWIFT_CASES if args.local else SWIFT_CASES) if args.macos else ANDROID_CASES) if args.mutate else []:
            if args.case and name not in args.case: continue
            path = source / file; original = path.read_bytes()
            if original.count(old.encode()) != 1: raise RuntimeError(name + ': ambiguous mutation anchor')
            try:
                path.write_bytes(original.replace(old.encode(), new.encode(), 1))
                build(name + '-mutant'); test(name + '-red', target, red=True, compiled_file=file)
                test(name + '-control', control=True)
            finally: path.write_bytes(original)
            build(name + '-restore'); test(name + '-restored', target)
            if sha(path) != hashlib.sha256(original).hexdigest(): raise RuntimeError('Restoration SHA mismatch')
            groups.append(name); print(name + ': compiled normal assertion red / independent configuration green / restore green / SHA256', flush=True)
        if args.mutate: test('final')
        if adb: run('stop', [*adb, 'shell', 'am', 'force-stop', 'com.hanxi.ghosterm'])
        if any(sha(ROOT / name) != value for name, value in inputs.items()): raise RuntimeError('Input source changed')
        record = dict(accepted=True, platform='macos' if args.macos else 'android', groups=groups,
            tests=(16 if args.local else 7) if args.macos else 6, inputs_sha256=inputs, commands=commands,
            device=metadata if adb else None,
            scope=('Native launch/approval configuration and actual POSIX argv/environment probe' if args.macos else
                   'Android Native form/approval and generated command validation') + '; no actual Node/Exec SSH activation')
        if args.receipt:
            receipt = args.receipt.resolve()
            if not receipt.is_relative_to(ROOT / 'tmp'): raise RuntimeError('Receipt must be in project tmp')
            receipt.write_text(json.dumps(record, indent=2) + '\n')
        print('EXECUTION APP PASS: ' + str(len(groups)) + ' groups; ' + str(len(inputs)) + ' input hashes; ' + str(len(commands)) + ' commands', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--macos', action='store_true'); target.add_argument('--android'); target.add_argument('--headless-api', choices=['29', '36'])
    parser.add_argument('--mutate', action='store_true'); parser.add_argument('--local', action='store_true', help='also exercise local Native launch and metadata guards (macOS)'); parser.add_argument('--case', action='append')
    parser.add_argument('--receipt', type=Path)
    args = parser.parse_args()
    if args.local and not args.macos: parser.error('--local requires --macos')
    if args.case and (not args.mutate or set(args.case) - {c[0] for c in ((SWIFT_CASES + LOCAL_SWIFT_CASES if args.local else SWIFT_CASES) if args.macos else ANDROID_CASES)}):
        parser.error('--case requires --mutate and known cases')
    (ROOT / 'tmp').mkdir(exist_ok=True)
    manager = nullcontext(args.android)
    if args.headless_api:
        spec = importlib.util.spec_from_file_location('execution_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        manager = module.headless(args.headless_api)
    with manager as serial: execute(args, serial)


if __name__ == '__main__':
    def interrupted(number, frame): raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT, interrupted); signal.signal(signal.SIGTERM, interrupted)
    main()
