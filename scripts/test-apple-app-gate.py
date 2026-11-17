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

# Test imports must not create temporary bytecode beside tracked sources.
sys.dont_write_bytecode = True


ROOT = Path(__file__).resolve().parent.parent
parser = argparse.ArgumentParser(add_help=False)
parser.add_argument('--gate', type=Path, default=ROOT / 'scripts/apple-app-gate.sh')
parser.add_argument('--fixture-scripts', type=Path, default=ROOT / 'scripts')
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
        for name in ['apple-gate.sh', 'apple-gate-profile.sh', 'check-required-xctests.py']:
            shutil.copyfile(options.fixture_scripts / name, scripts / name)
        (self.root / 'build/GhosttyKit.xcframework/macos-arm64').mkdir(parents=True)
        (self.root / 'apple/GhostermApp').mkdir(parents=True)
        (self.root / 'apple/GhostermCore').mkdir(parents=True)
        binaries = self.root / 'bin'
        binaries.mkdir()
        # Deliberately omit sshd/ssh-keygen: exercising the gate's control flow
        # must not discover or start a host service, even when one is installed.
        for name in ('bash', 'dirname', 'mkdir', 'rm', 'sed', 'grep', 'head', 'tail', 'python3', 'cp', 'mktemp', 'id', 'seq', 'sleep', 'env'):
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
        completion = '''printf "%s\\n" "Test Case 'StubTests.testControl' passed (0.001 seconds)." "Executed 1 test, with 0 failures (0 unexpected)"
''' if event == 'swift' else ''
        path.write_text('#!/bin/bash\n'
                        f'printf "%s\\n" "{event}" >> "$GATE_TEST_TRACE"\n'
                        + arguments + completion +
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

    def test_optional_no_ssh_runs_all_checks_and_reports_partial(self):
        result, trace = self.run_gate()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(trace, ['abi', 'core', 'swift', 'html'])
        self.assertIn('APPLE APP GATE PARTIAL', result.stdout)
        self.assertNotIn('APPLE APP GATE PASS', result.stdout)

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


    def test_required_missing_swift_is_failure(self):
        (self.root / 'bin/swift').unlink()
        result, trace = self.run_gate(GHOSTERM_APPLE_GATE_MODE='required')
        self.assert_failure(result, 2)
        self.assertEqual(trace, [])

    def test_optional_missing_swift_is_partial(self):
        (self.root / 'bin/swift').unlink()
        result, trace = self.run_gate()
        self.assertEqual(result.returncode, 0)
        self.assertIn('APPLE APP GATE PARTIAL', result.stdout)
        self.assertNotIn('APPLE APP GATE PASS', result.stdout)
        self.assertEqual(trace, [])

    def test_required_missing_ssh_is_failure(self):
        result, trace = self.run_gate(GHOSTERM_APPLE_GATE_MODE='required')
        self.assert_failure(result, 2)
        self.assertEqual(trace, ['abi', 'core'])

    def test_invalid_mode_is_rejected(self):
        result, trace = self.run_gate(GHOSTERM_APPLE_GATE_MODE='requred')
        self.assert_failure(result, 2)
        self.assertEqual(trace, [])

    def test_empty_swift_execution_cannot_pass(self):
        swift = self.root / 'bin/swift'
        swift.write_text('#!/bin/bash\nprintf "%s\\n" "Executed 0 tests, with 0 failures (0 unexpected)"\n')
        result, _ = self.run_gate()
        self.assert_failure(result, 1)

    def owned_ssh_stub(self, starts=True):
        binaries = self.root / 'bin'
        keygen = binaries / 'ssh-keygen'
        keygen.write_text('#!/bin/bash\nwhile [ "$#" -gt 0 ]; do if [ "$1" = -f ]; then shift; : > "$1"; : > "$1.pub"; fi; shift; done\n')
        keygen.chmod(0o755)
        sshd = binaries / 'sshd'
        sshd.write_text('#!' + str(Path(shutil.which('python3')).resolve()) + '\n' +
            'import socket,sys\n' +
            'from pathlib import Path\n' +
            'with Path(__file__).with_name("sshd-started").open("w") as f: f.write("owned fixture")\n' +
            ("raise SystemExit(1)\n" if not starts else
             'p=Path(sys.argv[sys.argv.index("-f")+1]);port=int(next(x.split()[1] for x in p.read_text().splitlines() if x.startswith("Port ")))\n'
             's=socket.socket();s.bind(("127.0.0.1",port));s.listen()\n'
             'while True:\n c,_=s.accept();c.close()\n'))
        sshd.chmod(0o755)

    def test_required_sshd_start_failure_is_failure(self):
        self.owned_ssh_stub(starts=False)
        result, _ = self.run_gate(GHOSTERM_APPLE_GATE_MODE='required')
        self.assert_failure(result, 2)
        self.assertTrue((self.root / 'bin/sshd-started').exists())

    def required_log(self, status='passed'):
        swift = self.root / 'bin/swift'
        lines = ["Test Case '-[GhostermAppTests.SessionSmokeTests testLocalShellReachesGrid]' passed (0.01 seconds).",
                 "Test Case '-[GhostermAppTests.SessionSmokeTests testSSHShellReachesGrid]' " + status + " (0.01 seconds).",
                 "Test Case '-[GhostermAppTests.SFTPBrowserTests testDualPaneBrowseAndDownload]' passed (0.01 seconds).",
                 'Executed 3 tests, with 0 failures (0 unexpected)']
        swift.write_text('#!/bin/bash\nprintf "%s\\n" "swift" >> "$GATE_TEST_TRACE"\n' +
                         ''.join("printf '%s\\n' " + __import__('shlex').quote(line) + '\n' for line in lines))
        swift.chmod(0o755)

    def test_required_receipts_pass(self):
        self.owned_ssh_stub()
        self.required_log()
        result, trace = self.run_gate(GHOSTERM_APPLE_GATE_MODE='required')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('APPLE APP GATE PASS (mode=required)', result.stdout)
        self.assertEqual(trace, ['abi', 'core', 'swift', 'html'])

    def test_required_skipped_ssh_test_is_failure(self):
        self.owned_ssh_stub()
        self.required_log(status='skipped')
        result, trace = self.run_gate(GHOSTERM_APPLE_GATE_MODE='required')
        self.assert_failure(result, 1)
        self.assertEqual(trace, ['abi', 'core', 'swift'])

    def test_required_missing_ssh_receipt_is_failure(self):
        self.owned_ssh_stub()
        result, trace = self.run_gate(GHOSTERM_APPLE_GATE_MODE='required')
        self.assert_failure(result, 1)
        self.assertEqual(trace, ['abi', 'core', 'swift'])

    def test_core_required_forbids_no_e2e(self):
        result = subprocess.run([str(options.shell), str(self.root / 'scripts/apple-gate.sh'), '--no-e2e'],
                                env={**self.env, 'GHOSTERM_APPLE_GATE_MODE': 'required'}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertNotIn('APPLE GATE PASS', result.stdout)

    def test_core_required_checks_all_twelve_ssh_receipts(self):
        self.owned_ssh_stub()
        names = ['SSHEndToEnd', 'SSHControlledEndToEnd', 'SSHAbortStopsOwnedSFTP',
                 'SFTPEndToEnd', 'SFTPDirectoryCreationEndToEnd', 'SFTPRemovalEndToEnd',
                 'SFTPPublishEndToEnd', 'SFTPReceiveEndToEnd', 'SFTPExclusiveCreateEndToEnd',
                 'SFTPPreviewEndToEnd', 'SFTPBrowseEndToEnd', 'SFTPControlledEndToEnd']
        swift = self.root / 'bin/swift'
        lines = ["Test Case '-[GhostermCoreTests.GhostermCoreTests test" + name + "]' passed (0.01 seconds)." for name in names]
        lines.append('Executed 12 tests, with 0 failures (0 unexpected)')
        swift.write_text('#!/bin/bash\n' + ''.join("printf '%s\\n' " + __import__('shlex').quote(line) + '\n' for line in lines))
        swift.chmod(0o755)
        result = subprocess.run([str(options.shell), str(self.root / 'scripts/apple-gate.sh')],
                                env={**self.env, 'GHOSTERM_APPLE_GATE_MODE': 'required'}, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('APPLE GATE PASS (mode=required)', result.stdout)

    def test_checker_rejects_duplicate_and_wrong_class_receipts(self):
        import importlib.util
        spec = importlib.util.spec_from_file_location('required_xctests', ROOT / 'scripts/check-required-xctests.py')
        module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        passed = "Test Case '-[M.Class method]' passed (0.01 seconds).\nExecuted 1 test, with 0 failures (0 unexpected)\n"
        module.verify(passed, ['Class.method'])
        with self.assertRaises(ValueError): module.verify(passed + passed, ['Class.method'])
        with self.assertRaises(ValueError): module.verify(passed, ['OtherClass.method'])
        with self.assertRaises(ValueError): module.verify(passed.replace('0 failures', '1 failure'), ['Class.method'])


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0], *test_args])
