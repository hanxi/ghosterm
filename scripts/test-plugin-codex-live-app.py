#!/usr/bin/env python3
"""Opt-in real selected-account Codex through transparent offscreen Native App UI.

The isolated Swift copy links the explicitly selected CLI's Lua-enabled Core.
The main workspace Core cache, HOME, CODEX_HOME and credential files stay intact.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import uuid

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
TEST = 'CodexLiveAppTests.testRealModelFileApprovalThroughNativePage'


def require(value, code):
    if not value:
        raise RuntimeError(code)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, environment, log, seconds=600):
    with log.open('wb') as output:
        process = subprocess.Popen(list(map(str, command)), cwd=ROOT, env=environment,
                                   stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            result = process.wait(timeout=seconds)
        except BaseException:
            for sig, delay in [(signal.SIGINT, 10), (signal.SIGTERM, 5), (signal.SIGKILL, 5)]:
                try:
                    os.killpg(process.pid, sig)
                except ProcessLookupError:
                    break
                try:
                    process.wait(timeout=delay)
                    break
                except subprocess.TimeoutExpired:
                    continue
            raise
    if result:
        text = log.read_text(errors='replace')
        codes = re.findall(r'Failure\(code: "([A-Z0-9_]+)"\)', text)
        steps = [line.strip() for line in text.splitlines() if line.startswith('CODEX LIVE APP STEP:')]
        print(json.dumps(dict(nativeFailureCodes=codes, steps=steps)), file=sys.stderr)
        if log.stem == 'compile':
            print('\n'.join(line for line in text.splitlines() if 'error:' in line)[:5000],file=sys.stderr)
        raise RuntimeError('NATIVE_' + log.stem.upper().replace('-', '_') + '_FAILED')


def execute(args, work):
    build = args.build.resolve(strict=True)
    package = args.package.resolve(strict=True)
    codex = Path(shutil.which(args.codex) or args.codex).resolve(strict=True)
    gterm = build / 'gterm'
    cache = (build / 'CMakeCache.txt').read_text()
    require('GHOSTERM_WITH_PLUGIN_LUA:BOOL=ON' in cache, 'EXPLICIT_LUA_CORE_REQUIRED')
    require((build / 'core/libghosterm_core.a').is_file() and
            (build / 'core/libghosterm_lua.a').is_file(), 'MATCHED_NATIVE_CORE_REQUIRED')
    environment = {**os.environ, 'TMPDIR': str(work), 'TMP': str(work), 'TEMP': str(work),
                   'GHOSTERM_AUTOMATION_OFFSCREEN': '1', 'GHOSTERM_CORE_BUILD_DIR': str(build / 'core'),
                   'GHOSTERM_CORE_TEST_PLUGIN_LUA': '1', 'DEVELOPER_DIR': '/Applications/Xcode.app/Contents/Developer'}
    version = subprocess.run([codex, '--version'], env=environment, capture_output=True, timeout=10)
    require(version.returncode == 0 and version.stdout.strip() == b'codex-cli 0.166.0', 'PINNED_CLI_VERSION')
    auth = subprocess.run([codex, 'login', 'status'], env=environment, capture_output=True, timeout=15)
    require(auth.returncode == 0, 'SELECTED_ACCOUNT_UNAVAILABLE')
    spec = importlib.util.spec_from_file_location('codex_live_scope', ROOT/'scripts/test-plugin-codex-live.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    workspace, temporary, target_root = [work / name for name in ('workspace', 'process-tmp', 'approval-target')]
    for path in (workspace, temporary, target_root): path.mkdir(mode=0o700)
    prefix = helper.backend_arguments(workspace, work, 'file')
    # Native local launch has an aggregate 64-argv budget. Merge the five test
    # overrides in one table, then compare the CLI's complete effective feature
    # map to prove no other selected feature changed before any model call.
    selected = subprocess.run([codex,'features','list'],env=environment,capture_output=True,timeout=15)
    require(selected.returncode == 0, 'SELECTED_FEATURES_UNAVAILABLE')
    def feature_map(output):
        values = {}
        for row in output.decode('utf-8',errors='strict').splitlines():
            parts = row.split()
            if len(parts) >= 3 and parts[-1] in ('true','false'):
                require(re.fullmatch(r'[a-z][a-z0-9_.]*',parts[0]), 'FEATURE_NAME')
                values[parts[0]] = parts[-1] == 'true'
        require(values, 'EMPTY_FEATURE_CONFIGURATION')
        return values
    expected_features = feature_map(selected.stdout)
    disabled = ('apps','plugins','hooks','multi_agent','plugin_discovery_prefetch')
    for name in disabled:
        expected_features[name] = False
    table = '{'+','.join(json.dumps(name)+'=false' for name in disabled)+'}'
    prefix = [value for value in prefix if not value.startswith('--config=features.')]
    prefix.append('--config=features='+table)
    require(len(prefix) <= 11 and all(len(value.encode()) <= 4096 for value in prefix), 'NATIVE_LAUNCH_ARGUMENT_BUDGET')
    validation = subprocess.run([codex, 'app-server', '--strict-config', '--print-startup-config', *prefix],
                                env=environment, capture_output=True, timeout=15)
    require(validation.returncode == 0, 'EXPLICIT_BACKEND_CONFIGURATION')
    features = subprocess.run([codex, 'features', 'list', *prefix], env=environment, capture_output=True, timeout=15)
    require(features.returncode == 0 and feature_map(features.stdout) == expected_features and
            expected_features.get('code_mode_host') is True, 'SELECTED_FEATURE_CONFIGURATION_CHANGED')
    # Copy sources only; never clone the user's account or reconfigure root Core.
    source = args.project.resolve(strict=True) if args.project else work/'project'
    if args.project:
        require(source.is_relative_to(ROOT/'tmp') and (source/'core/build').resolve() == (build/'core').resolve(),
                'OWNED_MATCHED_ISOLATED_PROJECT_REQUIRED')
        for name in ['CodexLiveAppTests.swift','ConversationUITestSupport.swift']:
            relative = Path('apple/GhostermApp/Tests/GhostermAppTests')/name
            shutil.copyfile(ROOT/relative,source/relative)
    else:
        for folder in ['apple/GhostermCore', 'apple/GhostermTerminal', 'apple/GhostermApp']:
            shutil.copytree(ROOT/folder, source/folder, symlinks=True, ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source/'core').mkdir()
        shutil.copytree(ROOT/'core/include', source/'core/include', symlinks=True)
        (source/'core/build').symlink_to(build/'core', target_is_directory=True)
        (source/'build').symlink_to(ROOT/'build', target_is_directory=True)
        (source/'tmp').mkdir()
    fixture = dict(gterm=str(gterm), gtermSHA256=sha(gterm), codex=str(codex), codexSHA256=sha(codex),
                   package=str(package), workspace=str(workspace), temporary=str(temporary),
                   target=str(target_root/'probe.txt'), nonce='GHOSTERM_LIVE_APP_'+uuid.uuid4().hex,
                   prefixArguments=prefix, prepareOnly=args.prepare_only)
    configuration = work/'fixture.json'; configuration.write_text(json.dumps(fixture)); configuration.chmod(0o600)
    environment.update(GHOSTERM_CODEX_APP_LIVE='1', GHOSTERM_CODEX_APP_FIXTURE=str(configuration))
    selected = source/'apple/GhostermApp'
    swift_options = ['--package-path', selected, '--cache-path', work / 'swift-cache',
                     '--disable-keychain', '--disable-netrc']
    run(['swift', 'build', '--build-tests', *swift_options], environment, work/'compile.log')
    # Build has no model call. This exact, explicit test is the only model turn.
    run(['swift', 'test', '--skip-build', *swift_options, '--filter', TEST],
        environment,work/'live.log',seconds=500)
    output = (work/'live.log').read_text()
    marker = 'PASS CODEX LIVE APP PREFLIGHT:' if args.prepare_only else 'PASS CODEX LIVE APP:'
    require('Executed 1 test, with 0 failures' in output and 'skipped' not in output.lower() and
            marker in output, 'EXACT_NATIVE_CASE_REQUIRED')
    if args.prepare_only:
        print(json.dumps(dict(result='preflight-passed',modelTurns=0,scope='Native consent/thread/policy/stop only')))
        return
    target = Path(fixture['target'])
    require(target.is_file() and not target.is_symlink() and
            target.read_bytes() == (fixture['nonce']+'\n').encode(), 'ACTUAL_APPROVED_BYTES')
    print(json.dumps(dict(result='passed', scope='macOS Native App UI / local stdio / real selected-account Codex',
        controls=['consent','start-thread','send','read-complete-patch','approve-once','stop'],
        modelTurns=1, approvedFiles=1, cliSHA256=fixture['codexSHA256'], gtermSHA256=fixture['gtermSHA256'],
        packageSHA256=sha(package/'integrity.json'), receiptSHA256=sha(work/'live.log'),
        isolatedSources=True, accountLocationsUnchanged=True, windowsOffscreen=True), ensure_ascii=False))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--live', action='store_true', help='explicitly make one real model/tool request')
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--package', type=Path, default=ROOT/'build/plugins/ai-codex')
    parser.add_argument('--codex', default='codex')
    parser.add_argument('--project', type=Path, help='reuse an owned isolated Swift copy with matching Core')
    parser.add_argument('--prepare-only', action='store_true', help='start and stop through Native UI without a model turn')
    args = parser.parse_args()
    require(args.live and platform.system() == 'Darwin', 'EXPLICIT_LIVE_MACOS_ONLY')
    (ROOT/'tmp').mkdir(exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix='codex-live-app-', dir=ROOT/'tmp'))
    failed = False
    try:
        execute(args, work)
    except BaseException as error:
        failed = True
        # Fixed fixture codes only; do not echo raw model/provider diagnostics.
        code = str(error) if isinstance(error, RuntimeError) else type(error).__name__
        print('CODEX LIVE APP FAILED: '+code, file=sys.stderr)
        raise
    finally:
        # An observation timeout is not proof that a descendant exited. Retain
        # this owned tree if a process still has it open; never clear live data.
        inspection = subprocess.run(['lsof','+D',str(work)],capture_output=True,timeout=15)
        if not failed and inspection.returncode == 1 and not inspection.stdout and not inspection.stderr:
            shutil.rmtree(work)
        else:
            reason = 'for failure diagnosis' if failed else 'until process release'
            print('Owned live App artifacts retained '+reason+': '+str(work),file=sys.stderr)


if __name__ == '__main__':
    main()
