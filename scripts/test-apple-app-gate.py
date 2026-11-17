#!/usr/bin/env python3
"""Check the real gate's exit propagation with isolated shell dependency stubs.

No Swift build, SSH server, UI or plugin is executed. The production gate is
copied into a disposable repository fixture under the project's tmp directory.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--gate', type=Path, default=ROOT / 'scripts/apple-app-gate.sh')
parser.add_argument('--shell', type=Path, default=Path(shutil.which('bash') or '/bin/bash'))
options, test_args = parser.parse_known_args()
SOURCE = options.gate.resolve()


class GateExitTests(unittest.TestCase):
    def setUp(self):
        (ROOT / 'tmp').mkdir(exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='apple-gate-exit-', dir=ROOT / 'tmp')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        scripts = self.root / 'scripts'
        scripts.mkdir()
        shutil.copyfile(SOURCE, scripts / 'apple-app-gate.sh')
        (self.root / 'build/GhosttyKit.xcframework/macos-arm64').mkdir(parents=True)
        (self.root / 'apple/GhostermApp').mkdir(parents=True)
        binaries = self.root / 'bin'
        binaries.mkdir()
        # Deliberately omit sshd/ssh-keygen: exercising the gate's control flow
        # must not discover or start a host service, even when one is installed.
        for name in ('bash', 'dirname', 'mkdir', 'rm', 'sed', 'grep', 'head', 'tail'):
            target = str(options.shell.resolve()) if name == 'bash' else shutil.which(name)
            self.assertIsNotNone(target, name)
            (binaries / name).symlink_to(target)
        self.stub(scripts / 'check-abi-apple.sh', 'abi', 'GATE_ABI_RC')
        self.stub(scripts / 'build-core.sh', 'core', 'GATE_CORE_RC')
        self.stub(scripts / 'test-plugin-html-webkit.sh', 'html', 'GATE_HTML_RC')
        self.stub(binaries / 'swift', 'swift', 'GATE_SWIFT_RC')
        self.trace = self.root / 'trace'
        self.swift_arguments = self.root / 'swift-arguments'
        self.env = {key: value for key, value in os.environ.items()
                    if not key.startswith(('GHOSTERM_', 'GATE_'))}
        self.env.update(PATH=str(binaries), TMPDIR=str(self.root / 'tmp'),
                        GATE_TEST_TRACE=str(self.trace), GATE_TEST_ARGV=str(self.swift_arguments))

    @staticmethod
    def stub(path, event, status):
        arguments = 'printf "%s\\0" "$@" > "$GATE_TEST_ARGV"\n' if event == 'swift' else ''
        path.write_text('#!/bin/bash\n'
                        f'printf "%s\\n" "{event}" >> "$GATE_TEST_TRACE"\n'
                        + arguments +
                        f'exit "${{{status}:-0}}"\n')
        path.chmod(0o755)

    def run_gate(self, **statuses):
        result = subprocess.run([str(self.root / 'bin/bash'), str(self.root / 'scripts/apple-app-gate.sh')],
                                cwd=self.root, env={**self.env, **statuses},
                                text=True, capture_output=True, timeout=10)
        trace = self.trace.read_text().splitlines() if self.trace.exists() else []
        return result, trace

    def assert_failure(self, result, code):
        self.assertEqual(result.returncode, code, result.stdout + result.stderr)
        self.assertNotIn('APPLE APP GATE PASS', result.stdout + result.stderr)

    def test_success_runs_all_checks(self):
        result, trace = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(trace, ['abi', 'core', 'swift', 'html'])
        self.assertIn('APPLE APP GATE PASS', result.stdout)

    def test_abi_failure_stops_before_build(self):
        result, trace = self.run_gate(GATE_ABI_RC='41')
        self.assert_failure(result, 41)
        self.assertEqual(trace, ['abi'])

    def test_filter_is_passed_as_one_argument(self):
        selected = 'ConfigStoreTests|HostListTests with spaces'
        self.env['GHOSTERM_APP_TEST_FILTER'] = selected
        result, trace = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(trace, ['abi', 'core', 'swift', 'html'])
        self.assertEqual(self.swift_arguments.read_bytes().split(b'\0'),
                         [b'test', b'--filter', selected.encode(), b''])

    def test_core_failure_stops_before_swift(self):
        result, trace = self.run_gate(GATE_CORE_RC='42')
        self.assert_failure(result, 1)
        self.assertEqual(trace, ['abi', 'core'])

    def test_swift_failure_stops_before_html(self):
        result, trace = self.run_gate(GATE_SWIFT_RC='43')
        self.assert_failure(result, 1)
        self.assertEqual(trace, ['abi', 'core', 'swift'])

    def test_html_failure_prevents_pass(self):
        result, trace = self.run_gate(GATE_HTML_RC='44')
        self.assert_failure(result, 44)
        self.assertEqual(trace, ['abi', 'core', 'swift', 'html'])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *test_args])
