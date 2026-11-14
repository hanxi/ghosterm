#!/usr/bin/env python3
"""Compiled Native App regressions in isolated source projects.

Default: restore one-frame-per-UI-tick throttling of a real 256 KiB resource.
--business-environment: omit an explicitly selected native utility environment.
--client-workspace / --client-directory / --client-instance: remove Native
publication approval / no-follow / Runtime identity checks, independently.
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
    component = 'ExtensionRelayBootstrap' if args.business_environment or args.foreground_grant else 'ExtensionController'
    name = Path('apple/GhostermApp/Sources/GhostermAppModel/' + component + '.swift' if mac else
                'android/app/src/main/java/com/hanxi/ghosterm/' + component + '.kt')
    original = (ROOT / name).read_bytes()
    if MARK.encode() in original:raise RuntimeError('Residual Native execution mutation')
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
            command = [sys.executable, ROOT / 'scripts/test-plugin-execution-ssh-app.py',
                *(['--macos'] if mac else ['--android', serial]), '--project', source,
                '--build', args.build, '--node', args.node, '--sdk', args.sdk,
                *(['--attach'] if args.foreground_grant else []),
                '--case', 'node:direct:no-approval' if args.foreground_grant else 'node:direct:client-directory-link' if args.client_directory else
                'node:direct:client-resource' if args.client_instance else
                'node:direct:client-no-approval' if args.client_workspace else
                'node:direct:process-environment' if args.business_environment else 'node:direct:resource']
            code, output = run(label, command)
            if red:
                if mac:
                    start = output.find("Test Suite 'Selected tests'")
                    transcript = output[start:]
                    valid = (start >= 0 and re.search(r'Executed 1 test, with 1 failure', transcript)
                             and ('XCTAssertNotNil failed' in transcript if (args.foreground_grant or args.client_workspace or args.client_directory) else
                                  'XCTAssertTrue failed' in transcript if args.client_instance else
                                  'XCTAssertEqual failed:' in transcript)
                             and (args.foreground_grant or args.client_workspace or args.client_directory or args.client_instance or args.business_environment or 'RPCState.unknown' in transcript))
                else:
                    start = output.find('INSTRUMENTATION_STATUS: class=')
                    transcript = output[start:]
                    valid = (start >= 0 and re.search(r'Tests run: 1,\s+Failures: 1', transcript)
                             and ('org.junit.Assert.assertNotNull' in transcript if (args.foreground_grant or args.client_workspace or args.client_directory) else
                                  'Stale Native action must not publish into another Runtime' in transcript if args.client_instance else
                                  'java.lang.AssertionError: expected:<' in transcript if args.business_environment else
                                  'java.lang.AssertionError: expected:<SUCCEEDED> but was:<UNKNOWN>' in transcript))
                assert code == 1 and valid and not any(x in transcript for x in
                    ['Process crashed', 'AppNotIdleException', 'TimeoutException', 'Compilation error',
                     'INSTRUMENTATION_FAILED', 'AddressSanitizer', 'runtime error:']), output
            else:
                assert code == 0 and 'EXECUTION SSH APP PASS: 1 actual cases' in output, output

        def artifact():
            if not mac:
                return sha(source / 'android/app/build/outputs/apk/debug/app-debug.apk')
            objects = list((source / 'apple/GhostermApp/.build').rglob(component + '.o'))
            assert objects, 'Actual compiled Native component object missing'
            return {str(p.relative_to(source)): sha(p) for p in objects}

        check('baseline'); baseline = artifact()
        text = original.decode()
        if args.foreground_grant:
            begin = 'guard attachment.authorized, attachment.instance' if mac else 'require(attachment.authorized && attachment.instance'
            assert text.count(begin)==1
            a=text.index(begin);b=a+len(begin)
            replacement='guard true, attachment.instance' if mac else 'require(true && attachment.instance'
        elif args.client_directory:
            begin = 'rootFD = clientWorkspace.directory.withCString {\n                        open($0, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)' if mac else \
                'OsConstants.O_RDONLY or OsConstants.O_NONBLOCK or OsConstants.O_NOFOLLOW or OsConstants.O_CLOEXEC'
            assert text.count(begin) == 1
            a = text.index(begin); b = a + len(begin)
            replacement = begin.replace(' | O_NOFOLLOW', '') if mac else begin.replace(' or OsConstants.O_NOFOLLOW', '')
        elif args.client_instance:
            begin = '                      self.runtimeInstanceID == instanceID,\n' if mac else 'runtimeInstanceID == instanceID && '
            assert text.count(begin) == 1
            a = text.index(begin); b = a + len(begin); replacement = ''
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
            scope='Owned Native App/loopback SSH/Node actual resource or mapped business process; single normal assertion red, same build control green, restore; no accounts/models')
        if args.receipt: args.receipt.write_text(json.dumps(receipt, indent=2) + '\n')
        print(('FOREGROUND GRANT' if args.foreground_grant else 'CLIENT DIRECTORY' if args.client_directory else 'CLIENT INSTANCE' if args.client_instance else 'CLIENT WORKSPACE' if args.client_workspace else 'BUSINESS ENV' if args.business_environment else 'PUMP') + ' MUTATION PASS: one compiled ordinary assertion red / same build control green / restore / SHA256', flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    target = p.add_mutually_exclusive_group(required=True)
    target.add_argument('--macos', action='store_true'); target.add_argument('--headless-api', choices=['29', '36']); target.add_argument('--android', help='explicit owned test device serial')
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--foreground-grant', action='store_true', help='remove explicit permission to connect the selected existing foreground instance')
    mode.add_argument('--client-directory', action='store_true', help='follow a symlink instead of opening only the selected actual directory')
    mode.add_argument('--client-instance', action='store_true', help='admit a publication action for an obsolete Runtime identity')
    mode.add_argument('--client-workspace', action='store_true', help='remove explicit Client directory approval')
    mode.add_argument('--business-environment', action='store_true', help='omit one selected business environment name instead of restoring the old pump')
    for key in ['build', 'node', 'sdk']: p.add_argument('--' + key, type=Path, required=True)
    p.add_argument('--receipt', type=Path); args = p.parse_args()
    if not __debug__: raise RuntimeError('Run checks with Python assertions enabled')
    if args.receipt and not args.receipt.resolve().is_relative_to(ROOT / 'tmp'): p.error('receipt must be under project tmp')
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
