#!/usr/bin/env python3
"""Restore one-frame-per-UI-tick throttling in an isolated Native App.

The real 256 KiB SSH publication must fail one normal state assertion; compiler,
crash, sanitizer and outer timeout failures are not an accepted target red.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args, serial=None):
    mac = args.macos
    name = Path('apple/GhostermApp/Sources/GhostermAppModel/ExtensionController.swift' if mac else
                'android/app/src/main/java/com/hanxi/ghosterm/ExtensionController.kt')
    original = (ROOT / name).read_bytes()
    with tempfile.TemporaryDirectory(prefix='xpump-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary); source = work / 'h'
        if mac:
            for folder in ['apple/GhostermCore', 'apple/GhostermTerminal', 'apple/GhostermApp']:
                shutil.copytree(ROOT / folder, source / folder, symlinks=True,
                    ignore=shutil.ignore_patterns('.build', '.swiftpm'))
            (source / 'core').mkdir()
            shutil.copytree(ROOT / 'core/include', source / 'core/include')
            (source / 'core/build').symlink_to(args.build.resolve() / 'core', target_is_directory=True)
            for folder in ['build', 'patches']: (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
            (source / 'tmp').mkdir()
        else:
            for folder in ['android', 'terminal/android']:
                shutil.copytree(ROOT / folder, source / folder,
                    ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
            for folder in ['core', 'vendor', 'build', 'assets', 'packages', 'scripts']:
                (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        target = source / name
        env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'),
                   DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
                   GHOSTERM_AUTOMATION_OFFSCREEN='1', GHOSTERM_CORE_TEST_PLUGIN_JS='1')
        commands = []

        def run(label, command):
            p = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True,
                               text=True, timeout=900)
            output = p.stdout + p.stderr; (work / (label + '.log')).write_text(output)
            commands.append(dict(label=label, returncode=p.returncode))
            return p.returncode, output

        def check(label, red=False):
            command = [sys.executable, ROOT / 'scripts/test-plugin-execution-ssh-app.py',
                *(['--macos'] if mac else ['--android', serial]), '--project', source,
                '--build', args.build, '--node', args.node, '--sdk', args.sdk,
                '--case', 'node:direct:resource']
            code, output = run(label, command)
            if red:
                if mac:
                    start = output.find("Test Suite 'Selected tests'")
                    transcript = output[start:]
                    valid = (start >= 0 and re.search(r'Executed 1 test, with 1 failure', transcript)
                             and 'XCTAssertEqual failed:' in transcript and 'RPCState.unknown' in transcript)
                else:
                    start = output.find('INSTRUMENTATION_STATUS: class=')
                    transcript = output[start:]
                    valid = (start >= 0 and re.search(r'Tests run: 1,\s+Failures: 1', transcript)
                             and 'java.lang.AssertionError: expected:<SUCCEEDED> but was:<UNKNOWN>' in transcript)
                assert code == 1 and valid and not any(x in transcript for x in
                    ['Process crashed', 'AppNotIdleException', 'TimeoutException', 'Compilation error',
                     'INSTRUMENTATION_FAILED', 'AddressSanitizer', 'runtime error:']), output
            else:
                assert code == 0 and 'EXECUTION SSH APP PASS: 1 actual cases' in output, output

        def artifact():
            if not mac:
                return sha(source / 'android/app/build/outputs/apk/debug/app-debug.apk')
            objects = list((source / 'apple/GhostermApp/.build').rglob('ExtensionController.o'))
            assert objects, 'Actual compiled controller object missing'
            return {str(p.relative_to(source)): sha(p) for p in objects}

        check('baseline'); baseline = artifact()
        text = original.decode()
        begin = '                // One native step consumes' if mac else '        // Native steps consume'
        end = '                return try self.runtimeState' if mac else '        val key = checkNotNull(runtimePackageKey)'
        assert text.count(begin) == 1
        a = text.index(begin); b = text.index(end, a)
        replacement = '                _ = try runtime.step()\n' if mac else '        current.step()\n'
        try:
            target.write_text(text[:a] + replacement + text[b:])
            check('mutated', True); mutant = artifact()
            assert baseline != mutant, 'Mutant did not change the actual compiled object/APK'
            if mac:
                code, output = run('control', ['swift', 'test', '--skip-build', '--package-path',
                    source / 'apple/GhostermApp', '--filter',
                    'ExtensionControllerTests.testClientHostProfileOnlyAdvertisesIntegratedCapabilities'])
                assert code == 0 and 'Executed 1 test, with 0 failures' in output and 'skipped' not in output.lower(), output
            else:
                code, output = run('control', [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial,
                    'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                    'com.hanxi.ghosterm.NativeSmokeTest#configurationRejectsNul',
                    'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                assert code == 0 and 'OK (1 test)' in output and 'numtests=1\n' in output, output
        finally:
            target.write_bytes(original)
        check('restored'); restored = artifact()
        assert target.read_bytes() == original and (ROOT / name).read_bytes() == original
        receipt = dict(accepted=True, platform='macos' if mac else 'android',
            groups=['ui-cadence-resource-throttle'], source_sha256=sha(target),
            artifact_sha256=dict(baseline=baseline, mutant=mutant, restored=restored), commands=commands,
            scope='Owned Native App/loopback SSH/Node 256 KiB actual resource; single normal assertion red, same build control green, restore; no accounts/models')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print('PUMP MUTATION PASS: one compiled ordinary assertion red / same build control green / restore / SHA256', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument('--macos', action='store_true'); target.add_argument('--headless-api', choices=['29', '36'])
    for key in ['build', 'node', 'sdk']: p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--receipt', type=Path); args = p.parse_args()
    if not __debug__: raise RuntimeError('Run checks with Python assertions enabled')
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): p.error('receipt must be under project tmp')
    if args.macos: execute(args)
    else:
        spec = importlib.util.spec_from_file_location('pump_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
        helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
        with helper.headless(args.headless_api) as serial: execute(args, serial)


if __name__ == '__main__': main()
