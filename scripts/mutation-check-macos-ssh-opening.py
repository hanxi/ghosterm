#!/usr/bin/env python3
"""Compile ordinary SSH/SFTP admission mutations in a supplied isolated checkout.

Only disposable files and an owned loopback sshd are used. Every target must be
one normal XCTest assertion failure after successful compilation, followed by an
independent configuration control and restored recompilation with matching SHA256.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import pwd
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
MODEL = 'apple/GhostermApp/Sources/GhostermAppModel/'
TEST = 'apple/GhostermApp/Tests/GhostermAppTests/SSHConnectionOpeningTests.swift'
CASES = [
    ('budget', 'SSHConnectionOpening.swift', 'let remaining = deadline - now()', 'let remaining = 60.0', 'testRemainingBudgetNeverRestartsAfterAuthentication'),
    ('cancel-priority', 'SSHConnectionOpening.swift', 'guard !cancellation.isCancelled else', 'guard true else', 'testCancellationWinsOverTimeout'),
    ('finite-budget', 'SSHConnectionOpening.swift', 'remaining.isFinite, remaining > 0', 'remaining > 0', 'testExpiredAndInvalidBudgetCannotOpenAChannel'),
    ('sftp-cap', 'SSHConnectionOpening.swift', 'min(Double(maximum), max(1, remaining * 1000))', 'min(60_000, max(1, remaining * 1000))', 'testSFTPCapDoesNotExceedRemainingLifetime'),
    ('shell-binding', 'SSHConnectionOpening.swift', 'try Transport.openSSHShell($0, timeoutMilliseconds: $1, cancellation: $2)', '_ = $2; return try Transport.openSSHShell($0, timeoutMilliseconds: $1)', 'testControlledOpenersRejectPreCancelledToken'),
    ('sftp-binding', 'SSHConnectionOpening.swift', 'try $0.sftp(timeoutMilliseconds: $1, cancellation: $2)', '_ = $2; return try $0.sftp(timeoutMilliseconds: $1)', 'testControlledOpenersRejectPreCancelledToken'),
    ('auth-deadline', 'SSHAuthenticationFlow.swift', 'deadline = opening?.deadline ??', 'deadline = (nil as TimeInterval?) ??', 'testExpiredAuthenticationDoesNotGetAnotherMinute'),
    ('auth-preflight', 'SSHAuthenticationFlow.swift', 'guard checkAdmission() else { return }\n        let timer', '// mutation: skip preflight\n        let timer', 'testCancelledAuthenticationDoesNotResolve'),
    ('auth-late-dns', 'SSHAuthenticationFlow.swift', '!self.finished, self.checkAdmission() else { return }; self.resolver', '!self.finished else { return }; self.resolver', 'testLateAuthenticationDNSCannotReplaceCancellation'),
    ('route-deadline', 'SSHRouteAuthenticationFlow.swift', 'deadline = opening?.deadline ??', 'deadline = (nil as TimeInterval?) ??', 'testExpiredRouteDoesNotGetAnotherMinute'),
    ('route-preflight', 'SSHRouteAuthenticationFlow.swift', 'guard checkAdmission() else { return }\n        let timer', '// mutation: skip preflight\n        let timer', 'testCancelledRouteDoesNotResolve'),
    ('route-late-dns', 'SSHRouteAuthenticationFlow.swift', '!self.finished, self.checkAdmission() else { return }; self.resolver', '!self.finished else { return }; self.resolver', 'testLateRouteDNSCannotReplaceCancellation'),
    ('terminal-budget', 'AppModel.swift', 'let remaining = try opening.remainingMilliseconds()\n                                let transport', 'let remaining: UInt32 = 60_000\n                                let transport', 'testTerminalProductUsesRemainingBudgetAndSharedToken'),
    ('sftp-budget', 'AppModel.swift', 'let remaining = try opening.remainingMilliseconds(maximum: 5000)', 'let remaining: UInt32 = 5000', 'testSFTPProductUsesRemainingBudgetAndSharedToken'),
    ('busy-owner-cancel', 'AppModel.swift', 'cancelledConnects.add(id)\n        connectionOpenings[id]?.cancel()', 'cancelledConnects.add(id)\n        // mutation: cancellation queued behind IO', 'testCancellationInterruptsARealStalledSFTPSubsystem'),
    ('task-cancel', 'AppModel.swift', '}, onCancel: { opening.cancel() })\n            await finishConnectionOpening', '}, onCancel: {})\n            await finishConnectionOpening', 'testSFTPTaskCancellationReachesBusyOpeningOwner'),
    ('terminal-late-success', 'AppModel.swift', 'if cancelled || opening.cancellation.isCancelled { pane.markFailed', 'if cancelled { pane.markFailed', 'testTerminalLateSuccessCannotAttachAfterCancellation'),
    ('sftp-late-success', 'AppModel.swift', 'if opening.cancellation.isCancelled {\n                                sftp.close()', 'if false {\n                                sftp.close()', 'testSFTPLateSuccessIsClosedBeforeCancellationReturns'),
    ('opening-generation', 'AppModel.swift', 'if connectionOpenings[id] === opening { connectionOpenings.removeValue(forKey: id) }', 'connectionOpenings.removeValue(forKey: id)', 'testOldOpeningCompletionCannotRemoveReplacement'),
    ('auth-generation', 'AppModel.swift', 'guard authenticationGenerations[id] == generation else { return }', '// mutation: old completion removes replacement', 'testOldAuthenticationCompletionCannotRemoveReplacement'),
]


def digest(value):
    return hashlib.sha256(value).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkout', required=True, type=Path)
    parser.add_argument('--case', action='append', choices=[case[0] for case in CASES])
    args = parser.parse_args()
    checkout = args.checkout.resolve(strict=True)
    if ROOT / 'tmp' not in checkout.parents or checkout == ROOT:
        parser.error('refusing to mutate outside a private checkout below project tmp/')
    actual = subprocess.check_output(['git', '-C', str(checkout), 'rev-parse', '--show-toplevel'], text=True).strip()
    if Path(actual).resolve() != checkout:
        parser.error('an independent checkout is required')
    work = Path(tempfile.mkdtemp(prefix='ssh-opening-mutations-', dir=checkout / 'tmp'))
    sources = {name: checkout / MODEL / name for name in {case[1] for case in CASES}}
    originals = {name: path.read_bytes() for name, path in sources.items()}
    tests = (checkout / TEST).read_bytes()
    records = []
    environment = {**os.environ, 'TMPDIR': str(checkout / 'tmp'), 'GHOSTERM_AUTOMATION_OFFSCREEN': '1'}
    # Protect the main checkout's connection methods while allowing the other
    # session to edit unrelated AppModel plugin methods concurrently.
    def main_snapshot():
        result = {}
        for name in sources:
            path = ROOT / MODEL / name
            if not path.exists():
                result[name] = None
                continue
            value = path.read_bytes()
            if name == 'AppModel.swift':
                text = value.decode()
                value = (text.split('private func startSSH(', 1)[1].split('private func startLocal(', 1)[0]
                         + text.split('func authenticate(_ target:', 1)[1].split('func finishChooserAuthentication(', 1)[0]).encode()
            result[name] = value
        return result
    protected = main_snapshot()
    command = ['swift', 'test', '--package-path', str(checkout / 'apple/GhostermApp'),
               '--cache-path', str(checkout / 'tmp/spm-cache'), '--skip-update', '--filter']

    def run(label, selection, red=False):
        logfile = work / (label + '.log')
        with logfile.open('w') as output:
            result = subprocess.run([*command, selection], cwd=checkout, env=environment,
                                    stdout=output, stderr=subprocess.STDOUT, timeout=180)
        output = logfile.read_text()
        if red:
            accepted = (result.returncode == 1 and 'Build complete!' in output
                        and re.search(r'Executed 1 test, with 1 failures? \(0 unexpected\)', output)
                        and 'XCTAssert' in output
                        and not re.search(r'Program crashed|unexpected signal|AddressSanitizer|UndefinedBehaviorSanitizer|error: emit-module|error: fatalError', output))
        else:
            accepted = (result.returncode == 0 and re.search(r'Executed [1-9][0-9]* tests?, with 0 failures', output)
                        and not re.search(r'Executed 0 tests|Test Case .* skipped', output))
        if not accepted:
            raise RuntimeError(f'{label}: expected {"compiled single normal assertion red" if red else "green"}: {logfile}')
        records.append({'case': label, 'returncode': result.returncode, 'log': str(logfile.relative_to(checkout))})

    server = None
    stalled_server = None
    try:
        for name in ('hostkey', 'clientkey'):
            subprocess.run(['/usr/bin/ssh-keygen', '-t', 'ed25519', '-f', str(work / name), '-N', '', '-q'],
                           stdin=subprocess.DEVNULL, check=True)
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); port = listener.getsockname()[1]
        config = work / 'sshd_config'
        config.write_text(f'''Port {port}
ListenAddress 127.0.0.1
HostKey {work}/hostkey
PidFile {work}/sshd.pid
AuthorizedKeysFile {work}/clientkey.pub
StrictModes no
PubkeyAuthentication yes
PasswordAuthentication no
KbdInteractiveAuthentication no
UsePAM no
PerSourcePenalties no
PermitUserRC no
LogLevel ERROR
Subsystem sftp /usr/libexec/sftp-server -d "{work}"
''')
        with (work / 'sshd.log').open('w') as output:
            server = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(config)], stdin=subprocess.DEVNULL,
                                      stdout=output, stderr=subprocess.STDOUT)
        for _ in range(50):
            if server.poll() is not None:
                raise RuntimeError('owned loopback sshd failed to start')
            try:
                with socket.create_connection(('127.0.0.1', port), timeout=.2):
                    break
            except OSError:
                time.sleep(.1)
        else:
            raise RuntimeError('owned loopback sshd did not become ready')
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0)); stall_port = listener.getsockname()[1]
        stall_config = work / 'stalled_config'
        stall_config.write_text(config.read_text().replace(f'Port {port}\n', f'Port {stall_port}\n')
            .replace(f'PidFile {work}/sshd.pid', f'PidFile {work}/stalled.pid')
            .replace(f'/usr/libexec/sftp-server -d "{work}"', '/bin/sleep 30'))
        with (work / 'stalled.log').open('w') as output:
            stalled_server = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(stall_config)],
                stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT)
        for _ in range(50):
            if stalled_server.poll() is not None:
                raise RuntimeError('owned stalled-subsystem sshd failed to start')
            try:
                with socket.create_connection(('127.0.0.1', stall_port), timeout=.2):
                    break
            except OSError:
                time.sleep(.1)
        else:
            raise RuntimeError('owned stalled-subsystem sshd did not become ready')
        environment['GHOSTERM_SSH_SFTP_STALL_PORT'] = str(stall_port)
        environment.update(GHOSTERM_SSH_HOST='127.0.0.1', GHOSTERM_SSH_PORT=str(port),
                           GHOSTERM_SSH_USER=pwd.getpwuid(os.getuid()).pw_name, GHOSTERM_SSH_KEY=str(work / 'clientkey'))
        environment.pop('GHOSTERM_TEST_KEYCHAIN_OWNED_REFERENCE', None)
        baseline = 'SSHConnectionOpeningTests|HostListTests'
        run('baseline', baseline)
        for name, filename, before, after, target in CASES:
            if args.case and name not in args.case:
                continue
            original = originals[filename].decode()
            if original.count(before) != 1:
                raise RuntimeError(f'{name}: mutation anchor must match exactly once')
            sources[filename].write_text(original.replace(before, after))
            run(name + '-red', 'SSHConnectionOpeningTests.' + target, red=True)
            run(name + '-control', 'HostListTests.testReadingDoesNotMutateConfig')
            sources[filename].write_bytes(originals[filename])
            run(name + '-restored', 'SSHConnectionOpeningTests.' + target)
            if sources[filename].read_bytes() != originals[filename] or (checkout / TEST).read_bytes() != tests:
                raise RuntimeError('source/test restore mismatch')
            print(f'PASS {name}: single assertion red, same-build control green, restored recompile green, SHA256', flush=True)
        run('final', baseline)
    finally:
        for name, path in sources.items():
            path.write_bytes(originals[name])
        for owned_server in (stalled_server, server):
            if owned_server is not None:
                owned_server.terminate()
                try:
                    owned_server.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    owned_server.kill(); owned_server.wait()
        (work / 'receipt.json').write_text(json.dumps({
            'sources_sha256': {name: digest(value) for name, value in originals.items()},
            'tests_sha256': digest(tests), 'records': records,
            'scope': 'Private native checkout, disposable config, owned loopback sshd; ordinary admission only',
        }, indent=2) + '\n')
        if (checkout / TEST).read_bytes() != tests or main_snapshot() != protected:
            raise RuntimeError('test inputs or main connection sources changed during scan')
    print('PASS macOS SSH opening mutation scan: ' + str(work / 'receipt.json'), flush=True)


if __name__ == '__main__':
    main()
