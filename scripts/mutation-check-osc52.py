#!/usr/bin/env python3
"""Manual OSC 52 guards: compiled assertion red / independent green / restored green + SHA256.

Apple and the headless Android discovery case use isolated source copies.
Android clipboard cases mutate this checkout and use the explicitly selected
test device, so avoid concurrent Android builds for those cases.
"""
import argparse
from contextlib import nullcontext
import hashlib
import importlib.util
import os
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
APPLE_DISCOVERY_LIFECYCLE = (
    'discovery-expiry', 'discovery-failure', 'discovery-reattach',
    'discovery-late-exit', 'discovery-runtime-failure',
)
DISCOVERY_CASES = ('discovery-feed', *APPLE_DISCOVERY_LIFECYCLE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['apple', 'android'], required=True)
    parser.add_argument('--case', action='append', choices=['write', 'read', 'permission', 'cancel', 'size-write', 'size-read', *DISCOVERY_CASES])
    parser.add_argument('--headless-api', choices=['29', '36'], help='owned no-window device for discovery-feed')
    args = parser.parse_args()
    selected = args.case or ()
    discovery = 'discovery-feed' in selected
    if args.platform == 'apple':
        if any(case in DISCOVERY_CASES for case in selected) and not all(case in DISCOVERY_CASES for case in selected):
            parser.error('select Apple discovery cases separately from clipboard cases')
    else:
        if any(case in APPLE_DISCOVERY_LIFECYCLE for case in selected):
            parser.error('discovery lifecycle cases require Apple')
        if discovery and (args.case != ['discovery-feed'] or not args.headless_api):
            parser.error('Android discovery-feed runs alone and requires --headless-api')
    if args.headless_api and (not discovery or args.platform != 'android'):
        parser.error('--headless-api supports only Android discovery-feed')
    device = nullcontext(None)
    if args.headless_api:
        spec = importlib.util.spec_from_file_location('osc52_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
        helper = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(helper)
        device = helper.headless(args.headless_api)
    with tempfile.TemporaryDirectory(prefix='osc52-', dir=ROOT / 'tmp') as temporary:
        with device as serial:
            verify(parser, args, Path(temporary), serial)


def verify(parser, args, work, owned_serial=None):
    env = dict(os.environ, TMPDIR=str(work), GHOSTERM_AUTOMATION_OFFSCREEN='1')

    def run(command, label, environment):
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=600)
        (work / f'{label}.log').write_text(result.stdout)
        return result

    apple_discovery = args.platform == 'apple' and any(case in DISCOVERY_CASES for case in (args.case or ()))
    if apple_discovery:
        for directory in ('GhostermApp', 'GhostermCore', 'GhostermTerminal'):
            shutil.copytree(ROOT / 'apple' / directory, work / 'apple' / directory, symlinks=True,
                            ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        for directory in ('build', 'patches'):
            (work / directory).symlink_to(ROOT / directory, target_is_directory=True)
        (work / 'core').mkdir()
        (work / 'core/include').symlink_to(ROOT / 'core/include', target_is_directory=True)
        (work / 'tmp').mkdir()
        env.update(DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer', GHOSTERM_CORE_TEST_PLUGIN_JS='1')
        cases = {
            'discovery-feed': ('SessionController.swift',
                'try bytes.withUnsafeBytes { try observer.feed($0, now:',
                'try Data().withUnsafeBytes { try observer.feed($0, now:', 'testPassiveDiscoveryKeepsOrdinaryPTYOutput',
                'top-level discovery must arrive after ordinary PTY output'),
            'discovery-expiry': ('AppModel.swift',
                'self.frontendCandidates = candidates\n                self.scheduleFrontendExpiry()',
                'self.frontendCandidates = candidates', 'testPassiveDiscoveryExpiresWhileTerminalIsIdle',
                'idle passive candidate must expire without another offer'),
            'discovery-failure': ('TerminalTab.swift',
                'public func markFailed(_ message: String) {\n        invalidateDiscovery()',
                'public func markFailed(_ message: String) {', 'testPassiveDiscoveryIsClearedWhenPaneFails',
                'failed pane must immediately remove its passive candidates'),
            'discovery-reattach': ('TerminalTab.swift',
                'self.session = session\n        invalidateDiscovery()\n        let generation',
                'self.session = session\n        let generation', 'testPassiveDiscoveryChangesOwnerWhenPaneReattaches',
                'reattaching a pane must immediately remove its old candidates'),
            'discovery-late-exit': ('TerminalTab.swift',
                'guard let self, let session, self.session === session else { return }',
                'guard let self, session != nil else { return }', 'testLateTerminalExitDoesNotInvalidateReplacementCandidates',
                'late exit from the old terminal must leave the replacement and its candidate intact'),
            'discovery-runtime-failure': ('TerminalTab.swift',
                'case .failed(let message):\n                self.invalidateDiscovery()',
                'case .failed(let message):', 'testRuntimeFailureClearsOnlyItsOwnPassiveCandidates',
                "runtime transport failure must remove only its pane's candidates"),
        }
        tests = {name: case[1:4] for name, case in cases.items()}
        source_root = Path('apple/GhostermApp/Sources/GhostermAppModel')
        def build(label):
            if label != 'baseline': return
            directory = work / 'core/build'
            for command in (['cmake', '-S', str(ROOT / 'core'), '-B', str(directory), '-DGHOSTERM_BUILD_TESTS=OFF',
                             '-DGHOSTERM_WITH_PLUGIN_JS=ON', '-DCMAKE_OSX_DEPLOYMENT_TARGET=13.0'],
                            ['cmake', '--build', str(directory), '--target', 'ghosterm_core', '-j', '8']):
                result = run(command, 'core-' + command[1].lstrip('-'), env)
                if result.returncode: raise RuntimeError(result.stdout[-12000:])
        def probe(test, label, red=False, assertion_message=None):
            name = 'HostListTests.testReadingDoesNotMutateConfig' if test == 'control' else 'DefaultSessionTests.' + test
            result = run(['xcrun', 'swift', 'test', '--package-path', work / 'apple/GhostermApp', '--filter', name], label, env)
            text = result.stdout
            success = result.returncode == 0 and 'Executed 1 test, with 0 failures' in text and 'skipped' not in text.lower()
            assertion = (result.returncode == 1 and 'Build complete!' in text and
                         'Executed 1 test, with 1 failure (0 unexpected)' in text and
                         assertion_message in text)
            if not (assertion if red else success): raise RuntimeError(f'{label}:\n{text[-12000:]}')
    elif args.platform == 'apple':
        package = work / 'apple/GhostermTerminal'
        shutil.copytree(ROOT / 'apple/GhostermTerminal', package, symlinks=True,
                        ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (work / 'build').symlink_to(ROOT / 'build', target_is_directory=True)
        source = package / 'Sources/GhostermTerminal/TerminalSurface.swift'
        prefix = ['swift', 'test', '--build-system', 'native', '--package-path', str(package), '--filter']
        tests = {
            'write': ('clipboard.setString(text, forType: .string)', '_ = text',
                      'testOSC52WriteAndReadUseSystemClipboardAndEngineReply'),
            'read': ('guard let self else { return false }',
                     'guard let self, self.clipboardGeneration < 0 else { return false }',
                     'testOSC52WriteAndReadUseSystemClipboardAndEngineReply'),
            'size-write': ('self.box.clipboardWrite = { [weak self] text, confirm in',
                           'self.box.clipboardWrite = { [weak self] text, confirm in\n            if text.utf8.count > 1024 * 1024 { return }',
                           'testWriteAndReadAboveOneMiBUseEngineAndClipboard'),
            'size-read': ('let text = clipboard.string(forType: .string) ?? ""',
                          'let text = clipboard.string(forType: .string) ?? ""\n            guard text.utf8.count <= 1024 * 1024 else { return false }',
                          'testWriteAndReadAboveOneMiBUseEngineAndClipboard'),
            'permission': ('pending(allowed)', 'pending(true)', 'testExplicitDenialNeverDisclosesText'),
            'cancel': ('if !visible { cancelClipboard() }', 'if !visible { /* mutation: omit cancel */ }',
                       'testDeniedAndHiddenRequestsNeverDiscloseClipboard'),
        }
        def build(label): pass
        def probe(test, label, red=False):
            name = ('TerminalSurfaceTests.testOutputReachesGhosttyGrid' if test == 'control'
                    else 'TerminalOSC52Tests.' + test)
            result = run(prefix + [name], label, env)
            text = result.stdout
            ok = (result.returncode == 1 and 'Build complete!' in text and
                  'XCTAssert' in text and 'Executed 1 test' in text and
                  'error: fatalError' not in text) if red else (
                      result.returncode == 0 and 'Executed 1 test, with 0 failures' in text)
            if not ok: raise RuntimeError(f'{label}:\n{text[-12000:]}')
    else:
        serial = owned_serial or env.get('ANDROID_SERIAL')
        if not serial: parser.error('ANDROID_SERIAL must designate the authorized test device')
        adb = [str(ROOT / 'build/android-toolchain/sdk/platform-tools/adb'), '-s', serial]
        project = ROOT
        if owned_serial:
            project = work / 'source'
            for directory in ('android', 'terminal/android'):
                shutil.copytree(ROOT / directory, project / directory,
                    ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
            for directory in ('core', 'vendor', 'build', 'assets', 'packages', 'scripts', 'patches', 'runtime-payload'):
                (project / directory).symlink_to(ROOT / directory, target_is_directory=True)
            (project / 'tmp').mkdir()
        relative = 'android/bridge/src/main/cpp/session_jni.c'
        source = project / relative
        tests = {
            'write': ('if (!read && *length) {', 'if (!read && *length) { /* mutation */\n        *length = 0;',
                      'fragmentedUnicodeWriteAndEmptyWrite'),
            'read': ('s->clipboard_read = read;', 's->clipboard_read = false;', 'approvedReadUsesEngineBase64Reply'),
            'size-write': ('s->clipboard_bytes = malloc(read || !*length ? 1 : *length);',
                           'if (!read && *length > 1024 * 1024) { pthread_mutex_unlock(&s->mutex); return false; }\n    s->clipboard_bytes = malloc(read || !*length ? 1 : *length);',
                           'writeAndReadAboveOneMiBAreNotCappedByHost'),
            'size-read': ('bool ok = allowed;', 'bool ok = allowed && length <= INPUT_LIMIT;',
                          'writeAndReadAboveOneMiBAreNotCappedByHost'),
            'permission': ('bool ok = allowed;', 'bool ok = !allowed;',
                           'deniedReadReturnsEmptyReply'),
            'cancel': ('atomic_store(&s->visible, visible);',
                       'visible = true; atomic_store(&s->visible, visible);',
                       'hiddenReadRejectsLateApproval'),
        }
        if owned_serial:
            tests = {
                'discovery-feed': ('gt_ext_discovery_feed(s->discovery,\n                                                output,\n                                                (size_t)length,',
                    'gt_ext_discovery_feed(s->discovery,\n                                                NULL,\n                                                0,',
                    'discoveryIgnoresNestedOscAndPreservesLocalTerminalOutput')
            }
        apk_profile = None
        def build(label):
            nonlocal apk_profile
            command = ['bash', 'scripts/build-android.sh']
            if owned_serial:
                sdk = ROOT / 'build/android-toolchain'
                env.update(ANDROID_HOME=str(sdk / 'sdk'), GRADLE_USER_HOME=str(sdk / 'gradle-home'),
                    JAVA_HOME=str(sdk / 'jdk-17.0.21+9/Contents/Home'), JAVA_TOOL_OPTIONS=f'-Djava.io.tmpdir={work}')
                command = [str(project / 'android/gradlew'), '-p', str(project / 'android'),
                    '--offline', '--no-daemon', '--max-workers=2', '-PghostermPluginJs=true']
            result = run([*command, ':app:assembleDebug', ':app:assembleDebugAndroidTest'], label+'-build', env)
            if result.returncode:
                raise RuntimeError(label + ': compile failed; not an assertion red\n' + result.stdout[-12000:])
            for kind, path in [('app','debug/app-debug.apk'), ('test','androidTest/debug/app-debug-androidTest.apk')]:
                apk = project / 'android/app/build/outputs/apk' / path
                result = run(adb + ['install', '-r', '-t', str(apk)], label+'-install-'+kind, env)
                if result.returncode or 'Success' not in result.stdout:
                    raise RuntimeError(label + ': install failed\n' + result.stdout[-12000:])
                if kind == 'app' and owned_serial:
                    digest = hashlib.sha256(apk.read_bytes()).hexdigest()
                    if label == 'baseline':
                        apk_profile = digest
                    if label.endswith('-mutant') and digest == apk_profile:
                        raise RuntimeError('discovery wire mutant did not change the production APK')
                if owned_serial:
                    package_id = 'com.hanxi.ghosterm' + ('.test' if kind == 'test' else '')
                    path_result = run(adb + ['shell', 'pm', 'path', package_id], label+'-path-'+kind, env)
                    installed = path_result.stdout.strip()
                    if path_result.returncode or not installed.startswith('package:/data/app/') or '\n' in installed:
                        raise RuntimeError('unexpected installed APK path')
                    result = run(adb + ['shell', 'sha256sum', installed.removeprefix('package:')], label+'-sha-'+kind, env)
                    if result.returncode or not result.stdout.startswith(hashlib.sha256(apk.read_bytes()).hexdigest() + ' '):
                        raise RuntimeError('installed APK bytes differ')
        def probe(test, label, red=False):
            name = ('NativeSmokeTest#corePreservesUtf8AndLineEndings' if test == 'control' else 'TerminalOSC52Test#'+test)
            result = run(adb + ['shell','am','instrument','-w','-r','-e','class','com.hanxi.ghosterm.'+name,
                               'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'], label, env)
            text = result.stdout
            ok = (result.returncode == 0 and 'Tests run: 1,  Failures: 1' in text and
                  any(kind in text for kind in ('java.lang.AssertionError', 'org.junit.ComparisonFailure', 'org.junit.internal.ArrayComparisonFailure')) and
                  'INSTRUMENTATION_STATUS_CODE: -2' in text and 'Process crashed' not in text) if red else (
                      result.returncode == 0 and 'OK (1 test)' in text)
            if not ok: raise RuntimeError(f'{label}:\n{text[-12000:]}')
            if red and owned_serial and '顶层发现候选未到达，普通终端标记已绘制' not in text:
                raise RuntimeError(f'{label}: wrong discovery assertion\n{text[-12000:]}')
    def interrupted(signum, frame): raise RuntimeError(f'interrupted: {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
    build('baseline')
    for name, (old, new, target) in tests.items():
        if args.case and name not in args.case:
            continue
        if apple_discovery:
            filename, *_, assertion_message = cases[name]
            relative = source_root / filename
            source = work / relative
        original = source.read_bytes()
        digest = hashlib.sha256(original).hexdigest()
        text = original.decode()
        if text.count(old) != 1:
            raise RuntimeError(f'{name}: mutation anchor count {text.count(old)}')
        probe(target, name+'-baseline')
        try:
            source.write_text(text.replace(old, new))
            build(name+'-mutant')
            if apple_discovery:
                probe(target, name+'-red', True, assertion_message=assertion_message)
            else:
                probe(target, name+'-red', True)
            probe('control', name+'-control')
        finally:
            source.write_bytes(original)
            assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
        if apple_discovery or owned_serial:
            assert (ROOT / relative).read_bytes() == original
        build(name+'-restored')
        probe(target, name+'-restored')
        print(f'PASS {args.platform}/{name}: assertion red / control green / restored green / SHA256 {digest}', flush=True)


if __name__ == '__main__': main()
