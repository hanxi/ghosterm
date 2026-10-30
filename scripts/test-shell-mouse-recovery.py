#!/usr/bin/env python3
"""Real Bash/Zsh prompt and native tmux regression for automatic mouse recovery.

--mutate proves each shell hook matters (assertion red, live-TUI control green,
restore green and SHA256). --codex additionally drives the installed Codex CLI
composer without submitting prompts. Synthetic wake is NOT real system sleep.
No user startup file, tmux server, or existing Codex session is changed.
"""
import argparse
from contextlib import contextmanager
import json
import pwd
import socket
import hashlib
import os
from pathlib import Path
import pty
import re
import runpy
import select
import shlex
import shutil
import signal
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_MOUSE_RECOVERY_MUTATION_MARK'
RESET = b'\x1b[?1003l'


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def timed(args, limit=180, **kwargs):
    command = ['bash', '-c', 'source "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require; shift; gt_timeout "$@"', '_', str(ROOT), str(limit)] + [str(x) for x in args]
    return subprocess.run(command, **kwargs)


class Shell:
    def __init__(self, executable):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            args = [executable] + (['-f', '-i'] if 'zsh' in executable else ['--noprofile', '--norc', '-i'])
            os.execve(executable, args, dict(os.environ, TERM='xterm-256color'))
        self.drain(0.2)
        self.command("PS1='GHOSTERM_PROMPT> '")

    def drain(self, seconds):
        end = time.monotonic() + seconds
        data = b''
        while time.monotonic() < end:
            if select.select([self.fd], [], [], 0.02)[0]:
                try:
                    data += os.read(self.fd, 65536)
                except OSError:
                    break
        return data

    def command(self, value):
        os.write(self.fd, value.encode() + b'\n')
        end = time.monotonic() + 5
        data = b''
        while time.monotonic() < end:
            data += self.drain(0.05)
            if b'GHOSTERM_PROMPT> ' in data:
                return data + self.drain(0.05)
        raise RuntimeError('interactive shell prompt missing: ' + repr(data[-1000:]))

    def close(self):
        os.close(self.fd)
        try:
            os.kill(self.pid, signal.SIGHUP)
        except ProcessLookupError:
            pass
        os.waitpid(self.pid, 0)


def shell_checks(work, shell, executable, script=None):
    script = script or ROOT / 'scripts/shell-integration' / ('mouse-recovery.' + shell)
    process = timed([executable, '-f' if shell == 'zsh' else '--noprofile', '-c', 'source "$1"; printf NONINTERACTIVE', 'probe', script], capture_output=True)
    require(process.returncode == 0 and process.stdout == b'NONINTERACTIVE', 'noninteractive shell must be unchanged')
    test = Shell(executable)
    status = work / (shell + '-status')
    hooks = work / (shell + '-hooks')
    try:
        original = "printf '%s' \"$?\" > " + shlex.quote(str(status))
        if shell == 'bash':
            test.command('PROMPT_COMMAND=' + shlex.quote(original + ' # trailing comment'))
        else:
            test.command('precmd() { ' + original + '; }; extra_hook() { printf ok > ' + shlex.quote(str(hooks)) + '; }; precmd_functions=(extra_hook)')
        source = 'source ' + shlex.quote(str(script))
        test.command(source)
        output = test.command(source)
        require(output.count(RESET) == 1, shell + ': repeated source must run one reset, not zero or two')
        if shell == 'zsh':
            hooks.unlink(missing_ok=True)
        output = test.command('(exit 37)')
        require(RESET in output, shell + ': failed commands must still recover at prompt')
        require(status.read_text() == '37', shell + ': existing prompt hook must receive command exit status')
        if shell == 'zsh':
            require(hooks.exists() and hooks.read_text() == 'ok', 'zsh existing hook must still run')
        print('PASS', shell, 'interactive/idempotent/status/existing hooks and noninteractive control', flush=True)
    finally:
        test.close()


def installer_checks(work, install=None):
    install = install or runpy.run_path(str(ROOT / 'scripts/install-shell-mouse-recovery.py'))['install']
    rc = work / 'home/.zshrc'
    rc.parent.mkdir()
    before = b'# keep bytes\xff\nexport EXISTING=1\n'
    rc.write_bytes(before)
    directory = work / 'home/integration'
    install('zsh', rc, directory, False)
    require(rc.read_bytes() == before and not directory.exists(), 'preview wrote files')
    install('zsh', rc, directory, True)
    backups = list(rc.parent.glob('.zshrc.ghosterm-mouse-*.bak'))
    require(len(backups) == 1 and backups[0].read_bytes() == before, 'byte-for-byte startup backup missing')
    after = rc.read_bytes()
    require(after.startswith(before), 'existing startup bytes changed')
    install('zsh', rc, directory, True)
    require(rc.read_bytes() == after and len(list(rc.parent.glob('*.bak'))) == 1, 'install is not idempotent')
    require((directory / 'mouse-recovery.zsh').read_bytes() == (ROOT / 'scripts/shell-integration/mouse-recovery.zsh').read_bytes(), 'installed hook differs from tested hook')
    print('PASS installer preview/backup/preserved bytes/idempotence', flush=True)


def native(work, shell, recover, script, codex=False, ssh=None):
    env = dict(os.environ, TMPDIR=str(ROOT / 'tmp'),
               GHOSTERM_MOUSE_TEST_ROOT=str(work), GHOSTERM_MOUSE_TEST_SHELL=shell,
               GHOSTERM_MOUSE_PROMPT_RECOVERY=str(int(recover)), GHOSTERM_MOUSE_RECOVERY_SCRIPT=str(script),
               GHOSTERM_MOUSE_TEST_CODEX=str(int(codex)), GHOSTERM_MOUSE_CODEX_BINARY=shutil.which("codex") or "codex")
    if ssh:
        env['GHOSTERM_MOUSE_SSH_PREFIX'] = json.dumps(ssh)
    name = ('ssh-' if ssh else '') + shell + ('-codex' if codex else '') + ('-fixed' if recover else '-baseline')
    log = work / (name + '.log')
    try:
        with log.open('w') as output:
            result = timed([work / 'native/.build/debug/MouseProbe'], env=env, cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
        return result.returncode, log.read_text()
    finally:
        subprocess.run([env.get('TMUX_BINARY', '/opt/homebrew/bin/tmux'), '-S', str(work / 'native.sock'), 'kill-server'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)




def extra_mutations(work, originals):
    cases = [
        ('bash', 'idempotence', '    case "${PROMPT_COMMAND[*]-}" in *\'__ghosterm_mouse_status=\'*) return 0;; esac', '    :', 'repeated source must run one reset'),
        ('bash', 'status', '    return "$previous_status"', '    return 0', 'existing prompt hook must receive command exit status'),
        ('zsh', 'idempotence', 'if (( ! ${precmd_functions[(Ie)__ghosterm_mouse_prompt]} )); then', 'if true; then', 'repeated source must run one reset'),
        ('zsh', 'existing-hooks', '    precmd_functions+=(__ghosterm_mouse_prompt)', '    precmd_functions=(__ghosterm_mouse_prompt)', 'zsh existing hook must still run'),
    ]
    for shell, label, old, new, expected in cases:
        path = ROOT / 'scripts/shell-integration' / ('mouse-recovery.' + shell)
        data = originals[path].decode()
        require(data.count(old) == 1, 'mutation anchor drift/' + label)
        mutated = work / ('mutant-' + shell + '-' + label)
        mutated.write_text(data.replace(old, new) + '\n# ' + MARK + '\n')
        try:
            try:
                shell_checks(work, shell, '/bin/' + shell, mutated)
            except RuntimeError as error:
                require(expected in str(error), 'wrong mutation failure/' + label + ': ' + str(error))
            else:
                raise RuntimeError('mutation did not turn red/' + label)
            # The same broken hook still leaves real noninteractive execution intact.
            executable = '/bin/' + shell
            control = timed([executable, '-f' if shell == 'zsh' else '--noprofile', '-c', 'source "$1"; printf CONTROL', 'probe', mutated], capture_output=True)
            require(control.returncode == 0 and control.stdout == b'CONTROL', 'same-mutant noninteractive control failed/' + label)
        finally:
            mutated.write_bytes(originals[path])
            require(hashlib.sha256(mutated.read_bytes()).digest() == hashlib.sha256(path.read_bytes()).digest() == hashlib.sha256(originals[path]).digest(), 'SHA256 rollback failed/' + label)
        shell_checks(work, shell, '/bin/' + shell, mutated)
        print('PASS mutation', shell, label, 'target red / noninteractive control green / restored green / SHA256', flush=True)
    installer_path = ROOT / 'scripts/install-shell-mouse-recovery.py'
    original = installer_path.read_bytes()
    for label, old, new, expected in (
        ('backup', 'if after != before and rc.exists():', 'if False:', 'byte-for-byte startup backup missing'),
        ('preserve', 'after = before + block', 'after = block', 'existing startup bytes changed'),
    ):
        require(original.decode().count(old) == 1, 'installer mutation anchor drift/' + label)
        namespace = {'__file__': str(installer_path), '__name__': 'mouse_installer_test'}
        exec(compile(original.decode().replace(old, new) + '\n# ' + MARK, str(installer_path), 'exec'), namespace)
        directory = work / ('installer-mutant-' + label); directory.mkdir()
        try:
            installer_checks(directory, namespace['install'])
        except RuntimeError as error:
            require(expected in str(error), 'wrong installer mutation failure: ' + str(error))
        else:
            raise RuntimeError('installer mutation did not turn red/' + label)
        require((directory / 'home/integration/mouse-recovery.zsh').read_bytes() == (ROOT / 'scripts/shell-integration/mouse-recovery.zsh').read_bytes(), 'same-mutant hook installation control failed')
        restored = work / ('installer-restored-' + label);restored.mkdir()
        installer_checks(restored)
        require(hashlib.sha256(installer_path.read_bytes()).digest() == hashlib.sha256(original).digest(), 'installer SHA256 changed')
        print('PASS installer mutation', label, 'target red / payload green / restored green / SHA256', flush=True)


@contextmanager
def loopback_ssh(work):
    fixture = work / 'sshd'
    fixture.mkdir()
    for name in ('hostkey', 'clientkey'):
        result = timed(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', fixture / name], capture_output=True)
        require(result.returncode == 0, 'fixture key generation failed')
    (fixture / 'authorized_keys').write_bytes((fixture / 'clientkey.pub').read_bytes())
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1', 0))
        port = reservation.getsockname()[1]
    config = fixture / 'sshd_config'
    config.write_text(f"Port {port}\nListenAddress 127.0.0.1\nHostKey {fixture / 'hostkey'}\nPidFile {fixture / 'pid'}\nAuthorizedKeysFile {fixture / 'authorized_keys'}\nPubkeyAuthentication yes\nPasswordAuthentication no\nKbdInteractiveAuthentication no\nUsePAM no\nStrictModes no\nPerSourcePenalties no\nLogLevel ERROR\n")
    with (fixture / 'server.log').open('w') as log:
        server = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(config)], stdout=log, stderr=log, start_new_session=True)
        try:
            ready = False
            for _ in range(50):
                if server.poll() is not None:
                    break
                with socket.socket() as check:
                    check.settimeout(0.1)
                    if check.connect_ex(('127.0.0.1', port)) == 0:
                        ready = True
                        break
                time.sleep(0.02)
            require(ready, 'isolated sshd did not start: ' + (fixture / 'server.log').read_text())
            yield ['/usr/bin/ssh', '-F', '/dev/null', '-tt', '-i', str(fixture / 'clientkey'), '-p', str(port), '-o', 'IdentitiesOnly=yes', '-o', 'IdentityAgent=none', '-o', 'BatchMode=yes', '-o', 'StrictHostKeyChecking=accept-new', '-o', 'UserKnownHostsFile=' + str(fixture / 'known_hosts'), '-o', 'GlobalKnownHostsFile=/dev/null', pwd.getpwuid(os.getuid()).pw_name + '@127.0.0.1']
        finally:
            server.terminate()
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill(); server.wait()
            # Fixture private keys are never retained in evidence or snapshots.
            for name in ('hostkey', 'clientkey'):
                (fixture / name).unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mutate', action='store_true')
    parser.add_argument('--codex', action='store_true')
    parser.add_argument('--ssh', action='store_true', help='Also test manual system ssh through an isolated loopback sshd')
    args = parser.parse_args()
    temporary_root = ROOT / 'tmp'
    temporary_root.mkdir(exist_ok=True)
    hooks = [ROOT / 'scripts/shell-integration' / ('mouse-recovery.' + shell) for shell in ('bash', 'zsh')]
    originals = {p: p.read_bytes() for p in hooks}
    require(all(MARK.encode() not in data for data in originals.values()), 'residual mutation marker')
    work = Path(tempfile.mkdtemp(prefix='mouse-', dir=temporary_root))
    os.environ['TMPDIR'] = str(temporary_root)
    print('EVIDENCE', work, flush=True)
    for shell in ('bash', 'zsh'):
        shell_checks(work, shell, '/bin/' + shell)
    if Path('/opt/homebrew/bin/bash').exists():
        shell_checks(work, 'bash', '/opt/homebrew/bin/bash')
    installer_checks(work)
    if args.mutate:
        extra_mutations(work, originals)
    sources = work / 'native/Sources/MouseProbe'
    sources.mkdir(parents=True)
    shutil.copyfile(ROOT / 'scripts/probes/MouseRecovery.swift', sources / 'main.swift')
    shutil.copyfile(ROOT / 'apple/GhostermApp/Sources/GhostermAppModel/SessionController.swift', sources / 'SessionController.swift')
    import json
    deps = ', '.join('.package(path: ' + json.dumps(str(ROOT / 'apple' / name)) + ')' for name in ('GhostermTerminal', 'GhostermCore'))
    (work / 'native/Package.swift').write_text('// swift-tools-version:5.9\nimport PackageDescription\nlet package = Package(name: "MouseProbe", platforms: [.macOS(.v13)], dependencies: [' + deps + '], targets: [.executableTarget(name: "MouseProbe", dependencies: [.product(name: "GhostermTerminal", package: "GhostermTerminal"), .product(name: "GhostermCore", package: "GhostermCore")])])\n')
    (work / 'tui.py').write_text('''import os,sys,signal,termios,tty
saved=termios.tcgetattr(0)
def cleanup(signum,frame):
    os.write(1,b'\\x1b[?1000l\\x1b[?1002l\\x1b[?1003l\\x1b[?1006l')
    termios.tcsetattr(0,termios.TCSANOW,saved)
    sys.exit(0)
signal.signal(signal.SIGTERM,cleanup)
tty.setraw(0)
with open(sys.argv[1],'w') as f:f.write(str(os.getpid()))
os.write(1,b'\\x1b[?1003h\\x1b[?1006hTUI_READY:' + str(os.getpid()).encode() + b'\\r\\n')
with open(sys.argv[1]+'.input','wb',buffering=0) as captured:
    while True:captured.write(os.read(0,4096))
''')
    with (work / 'build.log').open('w') as output:
        build = timed(['xcrun', 'swift', 'build', '--package-path', work / 'native'], stdout=output, stderr=subprocess.STDOUT)
    require(build.returncode == 0, (work / 'build.log').read_text()[-6000:])
    for shell, path in zip(('bash', 'zsh'), hooks):
        for recover in (False, True):
            rc, output = native(work, shell, recover, path)
            require(rc == 0 and 'DONE:' in output, output[-6000:])
            print('PASS native', shell, 'automatic recovery' if recover else 'reproduced baseline', flush=True)
        if args.mutate:
            mutated = work / path.name
            data = originals[path].decode()
            anchor = "        builtin printf '\\033[?9l\\033[?1000l\\033[?1002l\\033[?1003l\\033[?1005l\\033[?1006l\\033[?1015l\\033[?1016l'"
            require(data.count(anchor) == 1, 'mutation anchor drift')
            mutated.write_text(data.replace(anchor, '        : # ' + MARK))
            try:
                rc, output = native(work, shell, True, mutated)
                require(rc == 1 and 'FAIL FIX automatic cleanup after crash' in output, 'target assertion did not turn red: ' + output[-6000:])
                require('PASS healthy mouse reporting still works after synthetic wake' in output, 'same-mutant live-TUI control missing: ' + output[-6000:])
            finally:
                mutated.write_bytes(originals[path])
                require(hashlib.sha256(mutated.read_bytes()).digest() == hashlib.sha256(originals[path]).digest() == hashlib.sha256(path.read_bytes()).digest(), 'SHA256 restore mismatch')
            rc, output = native(work, shell, True, mutated)
            require(rc == 0 and 'DONE:' in output, output[-6000:])
            print('PASS mutation', shell, 'target red / same-mutant live TUI green / restore green / SHA256', flush=True)
    if args.codex:
        require(shutil.which('codex') is not None, '--codex needs installed Codex CLI')
        for recover in (False, True):
            rc, output = native(work, 'bash', recover, hooks[0], True)
            require(rc == 0 and 'PASS Codex composer is ready' in output and 'PASS Codex keyboard still works after reattach' in output, output[-6000:])
            print('PASS real Codex composer synthetic wake/reattach; recovery=', recover, flush=True)
    require(all(path.read_bytes() == original for path, original in originals.items()), 'source files changed during isolated test')
    if args.ssh:
        with loopback_ssh(work) as ssh:
            for shell, path in zip(('bash', 'zsh'), hooks):
                for recover in (False, True):
                    rc, output = native(work, shell, recover, path, ssh=ssh)
                    require(rc == 0 and 'DONE:' in output, output[-6000:])
                    print('PASS manual system SSH + tmux', shell, 'recovery=', recover, flush=True)
            if args.codex:
                for recover in (False, True):
                    rc, output = native(work, 'bash', recover, hooks[0], True, ssh)
                    require(rc == 0 and 'PASS Codex composer is ready' in output and 'PASS Codex keyboard still works after reattach' in output, output[-6000:])
                    print('PASS real Codex over manual system SSH; recovery=', recover, flush=True)
    print('PASS shell mouse recovery; actual system sleep is not covered', flush=True)


if __name__ == '__main__':
    def interrupted(number, frame):
        raise InterruptedError(number)
    for number in (signal.SIGINT, signal.SIGTERM):
        signal.signal(number, interrupted)
    main()
