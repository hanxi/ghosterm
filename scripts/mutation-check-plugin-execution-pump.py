#!/usr/bin/env python3
"""Compiled Native App regressions in isolated source projects.

Default: throttle the native Lua Preview receiver to one frame per
UI tick while it receives a real 256 KiB PNG over SSH.
--business-environment: omit an explicitly selected native utility environment.
--client-workspace / --client-directory / --client-instance: remove Native
publication approval / no-follow / Runtime identity checks, independently.
--local-temporary-retry: retry obsolete Core handles instead of retained local cleanup.
All require one normal state assertion; compiler, crash, sanitizer and outer
timeout failures are not accepted target reds.
"""
import argparse
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
MARK = 'GT_PLUGIN_NATIVE_EXECUTION_MUTATION'
sys.dont_write_bytecode = True


def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()


def execute(args, serial=None):
    mac = args.macos
    lua_preview = not mac and not (args.foreground_grant or args.business_environment)
    lua_environment = not mac and args.business_environment
    mac_lua_environment = mac and args.business_environment
    lua_foreground = not mac and args.foreground_grant
    component = 'ExtensionRelayBootstrap' if args.business_environment or args.foreground_grant else 'ExtensionController'
    name = Path('apple/GhostermApp/Sources/GhostermAppModel/' + component + '.swift' if mac else
                'android/app/src/main/java/com/hanxi/ghosterm/' + component + '.kt')
    original = (ROOT / name).read_bytes()
    if MARK.encode() in original:raise RuntimeError('Residual Native execution mutation')
    with tempfile.TemporaryDirectory(prefix='xpump-', dir=os.environ.get('TMPDIR', ROOT / 'tmp')) as temporary:
        work = Path(temporary); source = work / 'h'
        if mac:
            for folder in ['apple/GhostermCore', 'apple/GhostermTerminal', 'apple/GhostermApp']:
                shutil.copytree(ROOT / folder, source / folder, symlinks=True,
                    ignore=shutil.ignore_patterns('.build', '.swiftpm'))
            (source / 'core').mkdir()
            shutil.copytree(ROOT / 'core/include', source / 'core/include')
            core_build = work / 'core'
            (source / 'core/build').symlink_to(core_build, target_is_directory=True)
            for folder in ['build', 'patches', 'plugins']: (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
            (source / 'tmp').mkdir()
        else:
            for folder in ['android', 'terminal/android']:
                shutil.copytree(ROOT / folder, source / folder,
                    ignore=shutil.ignore_patterns('build', '.gradle', '.kotlin', '.cxx', 'local.properties'))
            for folder in ['core', 'vendor', 'build', 'assets', 'patches', 'scripts']:
                (source / folder).symlink_to(ROOT / folder, target_is_directory=True)
        target = source / name
        env = dict(os.environ, TMPDIR=temporary, TMP=temporary, TEMP=temporary,
                   DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
                   GHOSTERM_AUTOMATION_OFFSCREEN='1')
        if lua_preview or lua_environment or lua_foreground:
            toolchain = ROOT / 'build/android-toolchain'
            env.update(JAVA_HOME=str(toolchain / 'jdk-17.0.21+9/Contents/Home'),
                       ANDROID_HOME=str(toolchain / 'sdk'), GRADLE_USER_HOME=str(toolchain / 'gradle-home'))
        elif mac_lua_environment:
            fixture_parent = str(work.parent)
            env.update(GHOSTERM_CORE_BUILD_DIR=str(core_build), GHOSTERM_LUA_ARCHIVE=str(args.lua_archive),
                       GHOSTERM_CORE_TEST_PLUGIN_LUA='1', GHOSTERM_APP_TEST_PACKAGE=str(source / 'apple/GhostermApp'),
                       GHOSTERM_TEST_CLAUDE_ENV_PROBE='1',
                       TMPDIR=fixture_parent, TMP=fixture_parent, TEMP=fixture_parent)
        commands = []

        def run(label, command):
            shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require native-execution; shift; exec "$gt_timeout_bin" --foreground 900 "$@"'
            p = subprocess.Popen(['bash','-c',shell,'native-execution',str(ROOT),*map(str,command)],cwd=ROOT,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,start_new_session=True)
            try:stdout,stderr=p.communicate(timeout=910)
            except BaseException:
                try:os.killpg(p.pid,signal.SIGTERM)
                except ProcessLookupError:pass
                try:p.communicate(timeout=15)
                except subprocess.TimeoutExpired:
                    try:os.killpg(p.pid,signal.SIGKILL)
                    except ProcessLookupError:pass
                    p.communicate(timeout=5)
                raise
            output = stdout + stderr; (work / (label + '.log')).write_text(output)
            commands.append(dict(label=label, returncode=p.returncode))
            return p.returncode, output

        def check(label, red=False):
            if mac_lua_environment:
                code, output = run(label, [sys.executable, '-B', ROOT / 'scripts/test-plugin-claude-app.py',
                    '--gterm', args.gterm, '--package', work / 'claude', '--route', 'direct', '--selected-environment'])
                if red:
                    assert code == 1 and 'Executed 1 test, with 1 failure' in output and \
                        'Approved CLI did not receive the scoped environment' in output, output
                else:
                    assert code == 0 and 'CLAUDE LUA APP PASS: 1 owned SSH cases' in output, output
                return
            if lua_preview or lua_environment or lua_foreground:
                code, output = run(label + '-build', [source / 'android/gradlew', '-p', source / 'android',
                    '--offline', '--no-daemon', '--max-workers=2', '-PghostermPluginLua=true',
                    f'-PghostermLuaArchive={args.lua_archive}', ':app:assembleDebug', ':app:assembleDebugAndroidTest'])
                assert code == 0 and 'BUILD SUCCESSFUL' in output, output
                if red:
                    assert re.search(r'^> Task :app:compileDebugKotlin\s*$', output, re.M), output
                apks = source / 'android/app/build/outputs/apk'
                if lua_foreground:
                    adb = [ROOT / 'build/android-toolchain/sdk/platform-tools/adb', '-s', serial]
                    for apk in (apks / 'debug/app-debug.apk', apks / 'androidTest/debug/app-debug-androidTest.apk'):
                        code, output = run(label + '-install-' + apk.name, [*adb, 'install', '-r', '-t', apk])
                        assert code == 0 and 'Success' in output, output
                    code, output = run(label, [*adb, 'shell', 'am', 'instrument', '-w', '-r', '-e', 'class',
                        'com.hanxi.ghosterm.PluginForegroundInspectionTest#luaForegroundAttachmentRequiresExplicitGrant',
                        'com.hanxi.ghosterm.test/androidx.test.runner.AndroidJUnitRunner'])
                    if red:
                        assert code == 0 and re.search(r'Tests run: 1,\s+Failures: 1', output) and \
                            'java.lang.AssertionError: Lua foreground attachment requires an explicit grant' in output, output
                    else:
                        assert code == 0 and 'OK (1 test)' in output, output
                    return
                common = ['--android', serial, '--gterm', args.gterm, '--route', 'direct']
                if lua_environment:
                    driver = 'test-plugin-claude-lua-android.py'
                    fixture_args = ['--apk-root', apks, '--package', work / 'claude', '--selected-environment']
                    assertion = 'Approved CLI did not receive the scoped environment'
                    passed = 'ANDROID CLAUDE LUA APP PASS: 1 SSH routes'
                else:
                    driver = 'test-plugin-preview-android.py'
                    fixture_args = ['--apk-directory', apks]
                    if args.client_directory or args.client_instance or args.client_workspace:
                        test = ('luaClientPublicationRejectsLinkedWorkspace' if args.client_directory
                                else 'luaClientPublicationUsesApprovedWorkspaceAndRuntime')
                        fixture_args.extend(['--test', 'com.hanxi.ghosterm.PluginPreviewSettingsE2ETest#' + test])
                        assertion = ('Lua Client publication must reject a linked workspace' if args.client_directory else
                                     'Lua Client publication must reject a stale Runtime identity' if args.client_instance else
                                     'Lua Client publication requires a workspace grant')
                    else:
                        fixture_args.append('--large-attachment')
                        assertion = 'MainActivity Settings did not receive Preview resource'
                    passed = 'ANDROID OFFICIAL PREVIEW E2E PASS:'
                code, output = run(label, [sys.executable, '-B', ROOT / 'scripts' / driver, *common, *fixture_args])
                if red:
                    transcript = output.partition('INSTRUMENTATION_STATUS: class=')[2]
                    transcript = transcript.partition('\nTraceback')[0]
                    assert code == 1 and re.search(r'Tests run: 1,\s+Failures: 1', transcript) and \
                        'java.lang.AssertionError: ' + assertion in transcript and \
                        (assertion != 'MainActivity Settings did not receive Preview resource' or 'phase=FAILED, error=EXPIRED' in transcript) and not any(x in transcript for x in
                        ('Process crashed', 'AppNotIdleException', 'TimeoutException', 'INSTRUMENTATION_FAILED')), output
                else:
                    assert code == 0 and passed in output, output
                return
        def artifact():
            if not mac:
                return sha(source / 'android/app/build/outputs/apk/debug/app-debug.apk')
            objects = [path for path in (source / 'apple/GhostermApp/.build').rglob('*.o')
                       if path.name in (component + '.o', component + '.swift.o')]
            assert objects, 'Actual compiled Native component object missing'
            return {str(p.relative_to(source)): sha(p) for p in objects}

        if lua_environment or mac_lua_environment:
            code, output = run('prepare-claude', [sys.executable, '-B', ROOT / 'scripts/gterm-plugin', 'build',
                ROOT / 'plugins/official/ai-claude', '--out', work / 'claude'])
            assert code == 0, output
        check('baseline'); baseline = artifact()
        text = original.decode()
        if args.foreground_grant:
            begin = 'require(attachment.authorized && attachment.instance'
            assert text.count(begin)==1
            a=text.index(begin);b=a+len(begin)
            replacement='require(true && attachment.instance'
        elif args.client_directory:
            begin = 'rootFD = clientWorkspace.directory.withCString {\n                        open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)' if mac else \
                'OsConstants.O_RDONLY or OsConstants.O_NONBLOCK or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC'
            assert text.count(begin) == 1
            a = text.index(begin); b = a + len(begin)
            replacement = begin.replace(' | O_NOFOLLOW', '') if mac else begin.replace(' or OsConstants.O_NOFOLLOW', '')
        elif args.client_instance:
            begin = 'runtimeInstanceID == instanceID && clientSource == null'
            assert text.count(begin) == 1
            a = text.index(begin); b = a + len(begin); replacement = 'clientSource == null'
        elif args.client_workspace:
            begin = 'guard authorized, directory.hasPrefix' if mac else 'require(authorized && Charsets.UTF_8.newEncoder()'
            assert text.count(begin) == 1
            a = text.index(begin); b = a + len(begin)
            replacement = 'guard true, directory.hasPrefix' if mac else 'require(true && Charsets.UTF_8.newEncoder()'
        elif args.business_environment:
            begin = '            argv.append(contentsOf: ["--process-environment", name])' if mac else \
                '            request.processEnvironmentNames.forEach { add("--process-environment"); add(it) }'
            assert text.count(begin) == 1
            a = text.index(begin); b = a + len(begin)
            replacement = '            if name != "GT_SELECTED" { argv.append(contentsOf: ["--process-environment", name]) }' if mac else \
                '            request.processEnvironmentNames.filter { it != "GT_SELECTED" }.forEach { add("--process-environment"); add(it) }'
        elif lua_preview:
            start = text.index('    suspend fun stepRuntime(): RuntimeState')
            end = text.index('    private fun runtimeState(', start)
            begin = 'for (step in 0 until 32) {'
            assert text[start:end].count(begin) == 1
            a = text.index(begin, start, end); b = a + len(begin)
            replacement = 'for (step in 0 until 1) {'
        else:
            begin = '                // One native step consumes' if mac else '        // Native steps consume'
            end = '                return try self.runtimeState' if mac else '        val key = checkNotNull(runtimePackageKey)'
            assert text.count(begin) == 1
            a = text.index(begin); b = text.index(end, a)
            replacement = '                _ = try runtime.step()\n' if mac else '        current.step()\n'
        try:
            target.write_text(text[:a] + replacement + text[b:] + '\n// '+MARK+'\n')
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
            groups=['explicit-foreground-attachment-grant' if args.foreground_grant else 'native-publication-directory-nofollow' if args.client_directory else
                    'native-publication-runtime-instance' if args.client_instance else
                    'explicit-client-workspace-grant' if args.client_workspace else
                    'selected-business-environment' if args.business_environment else 'ui-cadence-resource-throttle'], source_sha256=sha(target),
            artifact_sha256=dict(baseline=baseline, mutant=mutant, restored=restored), commands=commands,
            scope=('macOS Native controls over owned SSH with an explicitly mapped Lua Claude process environment' if mac_lua_environment else
                   'Android Settings over owned SSH with an explicitly mapped Lua Claude process environment' if lua_environment else
                   'Android native bootstrap with Lua Preview attachment metadata' if lua_foreground else
                   'Android native Client publication over owned loopback SSH with Lua Preview' if args.client_directory or args.client_instance or args.client_workspace else
                   'Android Settings over owned loopback SSH with Lua Preview and a 256 KiB PNG') + '; single normal assertion red, same build control green, restore; no accounts/models')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print(('FOREGROUND GRANT' if args.foreground_grant else 'CLIENT DIRECTORY' if args.client_directory else 'CLIENT INSTANCE' if args.client_instance else 'CLIENT WORKSPACE' if args.client_workspace else 'BUSINESS ENV' if args.business_environment else 'PUMP') + ' MUTATION PASS: one compiled ordinary assertion red / same build control green / restore / SHA256', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument('--macos', action='store_true'); target.add_argument('--headless-api', choices=['29', '36']); target.add_argument('--android', help='explicit owned test device serial')
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--local-temporary-retry', action='store_true', help='retry obsolete Core handles after local temporary cleanup retained its owner (macOS)')
    mode.add_argument('--foreground-grant', action='store_true', help='remove explicit permission to connect the selected existing foreground instance')
    mode.add_argument('--client-directory', action='store_true', help='follow a symlink instead of opening only the selected actual directory')
    mode.add_argument('--client-instance', action='store_true', help='admit a publication action for an obsolete Runtime identity')
    mode.add_argument('--client-workspace', action='store_true', help='remove explicit Client directory approval')
    mode.add_argument('--business-environment', action='store_true', help='omit one selected business environment name instead of restoring the old pump')
    for key in ('gterm', 'lua-archive'):
        p.add_argument('--' + key, type=Path)
    p.add_argument('--receipt', type=Path); args = p.parse_args()
    if not __debug__: raise RuntimeError('Run checks with Python assertions enabled')
    if args.local_temporary_retry and not args.macos: p.error('--local-temporary-retry requires --macos')
    for key in ('gterm', 'lua_archive'):
        value = getattr(args, key)
        if value is None or not value.exists():
            p.error('--' + key.replace('_', '-') + ' requires an existing path')
        setattr(args, key, value.resolve())
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): p.error('receipt must be under project tmp')
    if args.local_temporary_retry:
        with tempfile.TemporaryDirectory(prefix='lua-retry-', dir=os.environ.get('TMPDIR', ROOT / 'tmp')) as temporary:
            work = Path(temporary)
            subprocess.run([sys.executable, '-B', ROOT / 'scripts/gterm-plugin', 'build',
                ROOT / 'plugins/official/table', '--out', work / 'table'], cwd=ROOT, check=True)
            subprocess.run([sys.executable, '-B', ROOT / 'scripts/test-plugin-table-native.py',
                '--build', args.gterm.parent, '--package', work / 'table', '--local-temporary-retry',
                '--receipt', args.receipt or work / 'receipt.json'], cwd=ROOT, check=True)
        return
    if args.macos and not args.business_environment:
        with tempfile.TemporaryDirectory(prefix='lua-pump-', dir=os.environ.get('TMPDIR', ROOT / 'tmp')) as temporary:
            output = args.receipt or Path(temporary) / 'receipt.json'
            env = dict(os.environ, GHOSTERM_LUA_ARCHIVE=str(args.lua_archive))
            mutation = ('lua-foreground-grant' if args.foreground_grant else
                        'lua-client-directory' if args.client_directory else
                        'lua-client-instance' if args.client_instance else
                        'lua-client-workspace' if args.client_workspace else 'lua-resource-pump')
            subprocess.run([sys.executable, '-B', ROOT / 'scripts/test-plugin-pane.py', '--gterm', args.gterm,
                '--route', 'direct', '--mutate', '--mutation-case', mutation, '--out', output],
                cwd=ROOT, env=env, check=True)
        return
    if args.macos: execute(args)
    elif args.android:execute(args,args.android)
    else:
        spec = importlib.util.spec_from_file_location('pump_headless', ROOT / 'scripts/test-plugin-codex-patch-android.py')
        helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
        with helper.headless(args.headless_api) as serial: execute(args, serial)


if __name__ == '__main__':
    def interrupted(number,frame):raise KeyboardInterrupt(str(number))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
