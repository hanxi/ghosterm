#!/usr/bin/env python3
"""Manual OSC 52 guards: compiled assertion red / independent green / restored green + SHA256.

Apple uses an isolated source copy. Android mutates this checkout and uses the
explicitly selected test device, so avoid concurrent Android builds.
"""
import argparse
import hashlib
import os
from pathlib import Path
import signal
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--platform', choices=['apple', 'android'], required=True)
    parser.add_argument('--case', choices=['write', 'read', 'permission', 'cancel', 'size-write', 'size-read'])
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='osc52-', dir=ROOT / 'tmp') as temporary:
        verify(parser, args, Path(temporary))


def verify(parser, args, work):
    env = dict(os.environ, TMPDIR=str(work), GHOSTERM_AUTOMATION_OFFSCREEN='1')

    def run(command, label, environment):
        result = subprocess.run(command, cwd=ROOT, env=environment, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=600)
        (work / f'{label}.log').write_text(result.stdout)
        return result

    if args.platform == 'apple':
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
        serial = env.get('ANDROID_SERIAL')
        if not serial: parser.error('ANDROID_SERIAL must designate the authorized test device')
        adb = [str(ROOT / 'build/android-toolchain/sdk/platform-tools/adb'), '-s', serial]
        source = ROOT / 'android/bridge/src/main/cpp/session_jni.c'
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
        def build(label):
            result = run(['bash', 'scripts/build-android.sh', ':app:assembleDebug', ':app:assembleDebugAndroidTest'], label+'-build', env)
            if result.returncode:
                raise RuntimeError(label + ': compile failed; not an assertion red\n' + result.stdout[-12000:])
            for kind, path in [('app','debug/app-debug.apk'), ('test','androidTest/debug/app-debug-androidTest.apk')]:
                result = run(adb + ['install', '-r', '-t', str(ROOT/'android/app/build/outputs/apk'/path)], label+'-install-'+kind, env)
                if result.returncode or 'Success' not in result.stdout:
                    raise RuntimeError(label + ': install failed\n' + result.stdout[-12000:])
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
    original = source.read_bytes()
    digest = hashlib.sha256(original).hexdigest()
    def interrupted(signum, frame): raise RuntimeError(f'interrupted: {signum}')
    for sig in (signal.SIGINT, signal.SIGTERM): signal.signal(sig, interrupted)
    try:
        build('baseline')
        for name, (old, new, target) in tests.items():
            if args.case and name != args.case: continue
            text = original.decode()
            if text.count(old) != 1: raise RuntimeError(f'{name}: mutation anchor count {text.count(old)}')
            probe(target, name+'-baseline')
            source.write_text(text.replace(old, new))
            build(name+'-mutant')
            probe(target, name+'-red', True)
            probe('control', name+'-control')
            source.write_bytes(original)
            assert hashlib.sha256(source.read_bytes()).hexdigest() == digest
            build(name+'-restored')
            probe(target, name+'-restored')
            print(f'PASS {args.platform}/{name}: assertion red / control green / restored green / SHA256 {digest}', flush=True)
    finally:
        source.write_bytes(original)
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest


if __name__ == '__main__': main()
