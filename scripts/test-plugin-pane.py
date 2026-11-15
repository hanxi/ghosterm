#!/usr/bin/env python3
"""Exercise macOS plugin controls over owned SSH, optionally in mutated copies.

The directory chooser is injected; this does not verify the system file panel.
All windows remain offscreen and all temporary copies are under project tmp/.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PANE = 'apple/GhostermApp/Sources/GhostermApp/ExtensionsPane.swift'
TEST = 'PluginPaneFlowTests.testPreviewThroughPackageApprovalLaunchConsentAndStopControls'
CONTROL = 'HostListTests.testReadingDoesNotMutateConfig'


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gterm', type=Path, required=True)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--out', type=Path, default=ROOT / 'tmp/plugin-pane-receipt.json')
    args = parser.parse_args()
    gterm = args.gterm.resolve(strict=True)
    out = args.out.resolve()
    assert ROOT / 'tmp' in out.parents, 'receipt must be under project tmp/'
    assert gterm.is_file() and os.access(gterm, os.X_OK), 'explicit executable gterm required'
    cases = [
        ('digest', 'Text("摘要：\\(entry.review.digest)")', 'Text("摘要：unverified")',
         'review must display the exact immutable package digest'),
        ('approval', 'decide(entry: entry, approve: true)', 'decide(entry: entry, approve: false)',
         'approval did not reveal launch form'),
        ('launch', 'Button("连接并启动 Client 命令") { launch(entry) }', 'Button("连接并启动 Client 命令") { }',
         'launch control did not reach consent'),
        ('consent', 'await model.decideExtensionConsent(allow: true)', 'await model.decideExtensionConsent(allow: false)',
         'Preview did not receive exact owned SSH resource'),
        ('stop', 'await model.stopExtensionRun()', 'await Task.yield()',
         'stop control did not release run'),
    ]
    original = (ROOT / PANE).read_bytes()
    for label, old, _, _ in cases:
        assert original.count(old.encode()) == 1, label
    tracked = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
    names = {p for p in tracked if p.startswith(('core/', 'apple/', 'packages/plugin-sdk/', 'plugins/official/preview/'))
             and (ROOT / p).is_file()}
    names.update([PANE, 'scripts/test-plugin-pane.py', 'scripts/apple-app-gate.sh',
                  'scripts/check-abi-apple.sh', 'scripts/build-core.sh',
                  'scripts/test-plugin-html-webkit.sh',
                  'apple/GhostermApp/Tests/GhostermAppTests/PluginPaneFlowTests.swift'])
    inputs = {name: sha(ROOT / name) for name in sorted(names)}
    executable_sha = sha(gterm)
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'), GHOSTERM_AUTOMATION_OFFSCREEN='1',
               DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',
               GHOSTERM_CORE_TEST_PLUGIN_JS='1', GHOSTERM_TEST_GTERM=str(gterm))
    # The gate, never an inherited endpoint, creates the only authorized SSH service.
    for key in list(env):
        if key.startswith('GHOSTERM_SSH_'):
            del env[key]
    env.pop('GHOSTERM_TEST_CODEX_JUMP', None)
    commands, groups = [], []
    with tempfile.TemporaryDirectory(prefix='pane-ui-', dir=ROOT / 'tmp') as temporary:
        work = Path(temporary)
        source = work / 'source'
        for directory in ['apple/GhostermApp', 'apple/GhostermCore', 'apple/GhostermTerminal']:
            shutil.copytree(ROOT / directory, source / directory, symlinks=True,
                            ignore=shutil.ignore_patterns('.build', '.swiftpm'))
        (source / 'core').mkdir()
        for name in ['include', 'build']:
            (source / 'core' / name).symlink_to(ROOT / 'core' / name, target_is_directory=True)
        for name in ['build', 'patches', 'plugins']:
            (source / name).symlink_to(ROOT / name, target_is_directory=True)
        (source / 'tmp').mkdir()
        env['GHOSTERM_APP_TEST_PACKAGE'] = str(source / 'apple/GhostermApp')

        def run(label, selected=TEST, expected_failure=None):
            child_env = dict(env, GHOSTERM_APP_TEST_FILTER=selected)
            child = subprocess.Popen(['bash', 'scripts/apple-app-gate.sh'], cwd=ROOT, env=child_env,
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, start_new_session=True)
            try:
                output, _ = child.communicate(timeout=360)
            finally:
                if child.poll() is None:
                    os.killpg(child.pid, signal.SIGTERM)
                    try:
                        child.communicate(timeout=8)
                    except subprocess.TimeoutExpired:
                        os.killpg(child.pid, signal.SIGKILL)
                        child.communicate()
            log = (ROOT / 'tmp/apple-app-test.log').read_text()
            (work / (label + '.log')).write_bytes(output + log.encode())
            if expected_failure:
                failures = re.findall(r'^.*: error: .* : (?:XCTAssert\w+ failed|failed -)[^\n]*', log, re.M)
                assert child.returncode == 1 and len(failures) == 1 and expected_failure in failures[0], (label, log[-12000:])
                assert 'Executed 1 test, with 1 failure (0 unexpected)' in log, (label, log[-12000:])
                assert not any(s in log for s in ['unexpected signal', 'AddressSanitizer', 'fatalError']), label
            else:
                assert child.returncode == 0 and 'Executed 1 test, with 0 failures' in log, (label, output.decode()[-4000:], log[-12000:])
                assert 'skipped' not in log.lower(), label
            commands.append(dict(label=label, status=child.returncode,
                                 gateSHA256=hashlib.sha256(output).hexdigest(),
                                 xctestSHA256=hashlib.sha256(log.encode()).hexdigest()))
            print('PASS ' + label, flush=True)

        run('baseline')
        linked = {name: sha(ROOT / name) for name in [
            'core/build/libghosterm_core.a', 'core/build/libghosterm_quickjs.a',
            'build/host-native/lib/libssh2.a',
        ]}
        path = source / PANE
        for label, old, new, expected in cases if args.mutate else []:
            objects = {p: sha(p) for p in (source / 'apple/GhostermApp/.build').rglob('*') if p.name in ('ExtensionsPane.swift.o', 'ExtensionsPane.o')}
            assert objects, 'compiled production pane object required'
            path.write_bytes(original.replace(old.encode(), new.encode()))
            try:
                run(label + '-assertion', expected_failure=expected)
                assert any(sha(p) != digest for p, digest in objects.items()), 'mutation must be compiled'
                run(label + '-control', CONTROL)
            finally:
                path.write_bytes(original)
            run(label + '-restored')
            assert sha(path) == inputs[PANE], 'restoration must match exact source SHA256'
            groups.append(label)
        binary = next((source / 'apple/GhostermApp/.build').rglob('GhostermAppTests.xctest/Contents/MacOS/GhostermAppTests'))
        binary_sha = sha(binary)
    assert all(sha(ROOT / name) == digest for name, digest in inputs.items()), 'source changed during verification'
    assert all(sha(ROOT / name) == digest for name, digest in linked.items()), 'linked archive changed during verification'
    assert sha(gterm) == executable_sha, 'gterm changed during verification'
    assert (ROOT / PANE).read_bytes() == original, 'original checkout changed'
    receipt = dict(schemaVersion=1, scope='macOS Native controls; injected directory chooser; owned direct SSH; Markdown',
                   inputs=inputs, linkedArchives=linked, gtermSHA256=executable_sha, xctestSHA256=binary_sha,
                   commands=commands, mutations=groups)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(receipt, ensure_ascii=False, indent=2) + '\n')
    print(f'PLUGIN PANE PASS: 1 flow, {len(groups)} assertion/control/restore groups; receipt {out}', flush=True)


if __name__ == '__main__':
    main()
