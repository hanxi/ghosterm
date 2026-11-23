#!/usr/bin/env python3
"""Verify Android stopping an active Lua Runtime with an owned SSH/CLI fixture."""
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
FILE = Path('android/app/src/main/java/com/hanxi/ghosterm/ExtensionPackagesViewModel.kt')
OLD = 'activeRuntime?.info?.phase !in setOf(ExtensionRuntime.Phase.CREATED,\n                ExtensionRuntime.Phase.STARTING, ExtensionRuntime.Phase.ACTIVE)'


def execute(args, serial):
    original = (ROOT / FILE).read_bytes()
    assert original.count(OLD.encode()) == 1
    with tempfile.TemporaryDirectory(prefix='xstop-', dir=os.environ['TMPDIR']) as temporary:
        work = Path(temporary); source = work / 'h'
        for folder in ['android', 'terminal/android']:
            shutil.copytree(ROOT / folder, source / folder,
                ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
        for folder in ['core', 'vendor', 'build', 'assets', 'patches', 'scripts']:
            (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        target = source / FILE
        env = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary,
                   JAVA_HOME=str(ROOT / 'build/android-toolchain/jdk-17.0.21+9/Contents/Home'),
                   ANDROID_HOME=str(ROOT / 'build/android-toolchain/sdk'),
                   GRADLE_USER_HOME=str(ROOT / 'build/android-toolchain/gradle-home'))
        output_root = source / 'android/app/build/outputs/apk'
        def run(label, command):
            p = subprocess.run(list(map(str, command)), cwd=ROOT, env=env, capture_output=True, text=True, timeout=900)
            output = p.stdout + p.stderr; (work / (label + '.log')).write_text(output)
            return p.returncode, output
        def check(label, red=False):
            compiled, log = run(label + '-build', [source / 'android/gradlew', '-p', source / 'android',
                '--offline', '--no-daemon', '--max-workers=2', '-PghostermPluginLua=true',
                f'-PghostermLuaArchive={args.lua_archive}', ':app:assembleDebug', ':app:assembleDebugAndroidTest'])
            assert compiled == 0 and 'BUILD SUCCESSFUL' in log, log
            if red:
                assert re.search(r'^> Task :app:compileDebugKotlin\s*$', log, re.M), log
            command = [sys.executable, '-B', ROOT / 'scripts/test-plugin-claude-lua-android.py',
                '--android', serial, '--gterm', args.gterm, '--package', args.package,
                '--apk-root', output_root, '--route', 'direct']
            code, output = run(label, command)
            if red:
                # The child runner's Python traceback can quote its own rejection
                # strings; classify the actual instrumentation transcript only.
                transcript = output[output.find('INSTRUMENTATION_STATUS: class='):]
                assert code == 1 and re.search(r'Tests run: 1,\s+Failures: 1', transcript) and \
                    'java.lang.AssertionError: Runtime stop missing' in transcript and not any(x in transcript for x in
                    ['Process crashed', 'AppNotIdleException', 'TimeoutException', 'Compilation error', 'INSTRUMENTATION_FAILED']), output
            else: assert code == 0 and 'ANDROID CLAUDE LUA APP PASS: 1 SSH routes' in output, output
        check('baseline')
        apk = source / 'android/app/build/outputs/apk/debug/app-debug.apk'
        baseline_apk = hashlib.sha256(apk.read_bytes()).hexdigest()
        try:
            target.write_bytes(original.replace(OLD.encode(), OLD.replace(', ExtensionRuntime.Phase.ACTIVE', '').encode(), 1))
            check('mutated', True)
            mutated_apk = hashlib.sha256(apk.read_bytes()).hexdigest()
            assert mutated_apk != baseline_apk, 'Mutated APK did not change'
            code, output = run('control', [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial,
                'shell', 'am', 'instrument', '-w', '-r', '-e', 'class', 'com.hanxi.ghosterm.NativeSmokeTest#configurationRejectsNul',
                'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
            assert code == 0 and 'OK (1 test)' in output and 'numtests=1\n' in output, output
        finally: target.write_bytes(original)
        check('restored')
        assert target.read_bytes() == original and (ROOT / FILE).read_bytes() == original
        receipt = dict(accepted=True, groups=['active-runtime-stop'], source_sha256=hashlib.sha256(original).hexdigest(),
            apk_sha256=dict(baseline=baseline_apk, mutated=mutated_apk, restored=hashlib.sha256(apk.read_bytes()).hexdigest()),
            scope='Explicit owned API/arm64/4 KiB no-window device and actual loopback SSH/Lua with a no-model CLI fixture; Native control same APK green; no model')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print('STOP MUTATION PASS: actual Kotlin compile / single ordinary state assertion red / same APK Core green / restore / SHA256', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--headless-api', choices=['29', '36'], required=True)
    for key in ['gterm', 'package', 'lua-archive']: p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--receipt', type=Path)
    args = p.parse_args()
    args.lua_archive = args.lua_archive.resolve(strict=True)
    if not __debug__: raise RuntimeError('Run checks with Python assertions enabled')
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): p.error('receipt must be under project tmp')
    spec = importlib.util.spec_from_file_location('stop_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    with helper.headless(args.headless_api, Path(os.environ['TMPDIR'])) as serial: execute(args, serial)


if __name__ == '__main__': main()
