#!/usr/bin/env python3
"""Real pinned libssh2 -> owned OpenSSH exec, plus Paramiko protocol faults.
No private files/argv/logs: ephemeral unlocked keys reach C only through stdin.
"""
import argparse
import base64
from contextlib import ExitStack, contextmanager
import hashlib
import importlib.metadata
import importlib.util
import io
import json
import logging
import os
from pathlib import Path
import pwd
import select
import shlex
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True


def require(ok, message):
    if not ok:
        raise AssertionError(message)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def memory_identity():
    # Same OpenSSH Ed25519 fixture encoding as test-plugin-ssh.py. No API that
    # formats a failed subprocess may print generated private stdout.
    p = subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-outform', 'DER'], capture_output=True, timeout=10)
    require(p.returncode == 0 and len(p.stdout) == 48 and p.stdout[:16] == bytes.fromhex('302e020100300506032b657004220420'), 'in-memory fixture key generation failed')
    private = p.stdout
    p = subprocess.run(['openssl', 'pkey', '-inform', 'DER', '-pubout', '-outform', 'DER'], input=private, capture_output=True, timeout=10)
    require(p.returncode == 0 and len(p.stdout) == 44 and p.stdout[:12] == bytes.fromhex('302a300506032b6570032100'), 'fixture public derivation failed')
    public = p.stdout[12:]
    field = lambda b: struct.pack('>I', len(b)) + b
    blob = field(b'ssh-ed25519') + field(public)
    check = os.urandom(4)
    clear = check + check + field(b'ssh-ed25519') + field(public) + field(private[-32:] + public) + field(b'bridge-stream-fixture')
    clear += bytes(range(1, 9 - len(clear) % 8))
    packet = b'openssh-key-v1\0' + field(b'none') + field(b'none') + field(b'') + struct.pack('>I', 1) + field(blob) + field(clear)
    encoded = base64.b64encode(packet)
    key = b'-----BEGIN OPENSSH PRIVATE KEY-----\n' + b'\n'.join(encoded[i:i+70] for i in range(0, len(encoded), 70)) + b'\n-----END OPENSSH PRIVATE KEY-----\n'
    return key, b'ssh-ed25519 ' + base64.b64encode(blob) + b' bridge-stream-fixture\n'


@contextmanager
def real_server(work, public, helper):
    with ExitStack() as stack:
        agent = stack.enter_context(helper.owned_agent(work, 'host-agent'))
        host = helper.add_memory_key(agent, 'bridge-stream-host')
        host_file = work / 'host.pub'
        host_file.write_bytes(host)
        authorized = work / 'authorized_keys'
        authorized.write_bytes(public)
        empty = work / 'empty'
        empty.mkdir()
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            port = listener.getsockname()[1]
        config = work / 'sshd.conf'
        config.write_text(
            f'Port {port}\nListenAddress 127.0.0.1\nHostKey {host_file}\nHostKeyAgent {agent}\n'
            f'PidFile {work / "sshd.pid"}\nAuthorizedKeysFile {authorized}\n'
            'PubkeyAuthentication yes\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n'
            'UsePAM no\nStrictModes no\nLogLevel ERROR\nPermitUserRC no\n'
            'AllowAgentForwarding no\nAllowTcpForwarding no\nAllowStreamLocalForwarding no\n'
            f'PrintMotd no\nPrintLastLog no\nSetEnv HOME={empty} ZDOTDIR={empty} TMPDIR={work}\n')
        # Older macOS OpenSSH has no penalty directive. It is fixture tuning,
        # never a prerequisite for the channel/identity behavior under test.
        support = subprocess.run(['/usr/sbin/sshd','-T','-f',str(config),'-o','PerSourcePenalties=no'],
                                 capture_output=True, timeout=5)
        if support.returncode == 0:
            with config.open('a') as stream:
                stream.write('PerSourcePenalties no\n')
        log = stack.enter_context((work / 'sshd.log').open('wb'))
        daemon = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(config)], stdout=log, stderr=log)
        stack.callback(helper.stop, daemon)
        end = time.monotonic() + 5
        while True:
            require(daemon.poll() is None, 'owned sshd startup failed')
            with socket.socket() as probe:
                probe.settimeout(.05)
                if probe.connect_ex(('127.0.0.1', port)) == 0:
                    break
            require(time.monotonic() < end, 'owned sshd startup timeout')
            time.sleep(.02)
        yield port, host


@contextmanager
def fault_server(client_public, barriers=None):
    import paramiko
    host_private, host_public = memory_identity()
    key = paramiko.Ed25519Key.from_private_key(io.StringIO(host_private.decode()))
    client_blob = client_public.split()[1].decode()
    logger = logging.getLogger('paramiko.transport')
    old_disabled = logger.disabled
    logger.disabled = True
    closing = threading.Event()
    lock = threading.Lock()
    events = {}
    transports = []
    workers = []
    barriers = {} if barriers is None else barriers
    class Peer(paramiko.ServerInterface):
        def __init__(self):
            self.name = ''
            self.command = threading.Event()
        def event(self, name):
            with lock:
                events.setdefault(self.name, []).append(name)
        def get_allowed_auths(self, username):
            return 'publickey'
        def check_auth_publickey(self, username, supplied):
            self.name = username
            return paramiko.AUTH_SUCCESSFUL if supplied.get_base64() == client_blob else paramiko.AUTH_FAILED
        def check_channel_request(self, kind, chanid):
            self.event('channel')
            if self.name == 'channel-deny':
                return paramiko.OPEN_FAILED_RESOURCE_SHORTAGE
            return paramiko.OPEN_SUCCEEDED if kind == 'session' else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
        def check_channel_pty_request(self, *args):
            self.event('pty')
            return False
        def check_channel_shell_request(self, *args):
            self.event('shell')
            return False
        def check_channel_exec_request(self, channel, command):
            self.event('exec')
            if self.name == 'deny':
                return False
            if self.name in ('admission-cancel', 'admission-deadline'):
                closing.wait(10)
                return False
            self.command.set()
            return True
    def serve(connection):
        transport = paramiko.Transport(connection)
        with lock:
            transports.append(transport)
        peer = Peer()
        try:
            transport.add_server_key(key)
            transport.start_server(server=peer)
            channel = transport.accept(5)
            if channel is None:
                return
            end = time.monotonic() + 5
            while not peer.command.wait(.02):
                if not transport.is_active() or closing.is_set() or time.monotonic() > end:
                    return
            if peer.name.startswith('legacy-'):
                channel.sendall(b'stdout-tail')
                channel.sendall_stderr(b'stderr-tail')
                gated = peer.name in ('legacy-live','legacy-late','legacy-reset')
                if peer.name == 'legacy-late':
                    channel.shutdown_write()
                if gated:
                    while not barriers[peer.name].wait(.02):
                        if closing.is_set() or not transport.is_active():
                            return
                    require(not channel.closed, 'exit-status query sent a premature CLOSE')
                if peer.name == 'legacy-reset':
                    connection.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                    connection.close()
                    transport.close()
                    return
                if peer.name in ('legacy-zero','legacy-live'):
                    if peer.name == 'legacy-live':
                        channel.sendall(b'LATE')
                    channel.send_exit_status(0)
                elif peer.name in ('legacy-23','legacy-late','legacy-duplicate'):
                    channel.send_exit_status(23)
                    if peer.name=='legacy-duplicate':
                        channel.send_exit_status(0)
                elif peer.name=='legacy-range':
                    channel.send_exit_status(0xffffffff)
                elif peer.name in ('legacy-signal','legacy-malformed'):
                    from paramiko.message import Message
                    message=Message();message.add_byte(b'\x62');message.add_int(channel.remote_chanid)
                    message.add_string('exit-signal' if peer.name=='legacy-signal' else 'exit-status');message.add_boolean(False)
                    if peer.name=='legacy-signal':
                        message.add_string('TERM');message.add_boolean(False);message.add_string('fixture');message.add_string('')
                    transport._send_user_message(message) # deliberate incomplete status fixture
                channel.shutdown_write()
                # Keep TCP open; SSH EOF and an optional status are independent.
                while transport.is_active() and not closing.wait(.02):
                    pass
                return
            channel.sendall(b'\x00\x00\x00\x05ready')
            data = bytearray()
            channel.settimeout(5)
            while len(data) < 4:
                chunk = channel.recv(4-len(data))
                if not chunk:
                    return
                data.extend(chunk)
            length = struct.unpack('>I', data)[0]
            require(0 < length <= 65536, 'invalid client frame in fault peer')
            while len(data) < length+4:
                chunk = channel.recv(min(251, length+4-len(data)))
                if not chunk:
                    return
                data.extend(chunk)
            if peer.name == 'reset':
                require(bytes(data[4:]) == b'reset-ack', 'reset requires client receipt barrier')
                # The client consumed READY and acknowledged it; RST must not
                # race delivery of the very evidence this test depends on.
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack('ii', 1, 0))
                connection.close()
                transport.close()
                return
            for offset in range(0, len(data), 173):
                channel.sendall(data[offset:offset+173])
            # Intentionally omit send_exit_status: getter 0 must stay unknown.
            channel.shutdown_write()
            while transport.is_active() and not closing.wait(.02):
                pass
        except (EOFError, OSError, paramiko.SSHException):
            pass
        finally:
            transport.close()
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', 0))
    listener.listen()
    listener.settimeout(.1)
    def accept():
        while not closing.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            worker = threading.Thread(target=serve, args=(connection,))
            workers.append(worker)
            worker.start()
    acceptor = threading.Thread(target=accept)
    acceptor.start()
    try:
        yield listener.getsockname()[1], host_public, events
    finally:
        closing.set()
        acceptor.join(2)
        require(not acceptor.is_alive(), 'fault acceptor did not stop')
        listener.close()
        for transport in transports:
            transport.close()
        for worker in workers:
            worker.join(6)
            require(not worker.is_alive(), 'fault worker did not stop')
        logger.disabled = old_disabled


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', type=Path, default=ROOT/'core/build/test_bridge_ssh')
    parser.add_argument('--case', action='append')
    parser.add_argument('--legacy', action='store_true')
    parser.add_argument('--expect-assertion', action='store_true', help=argparse.SUPPRESS)
    args = parser.parse_args()
    # Existing pinned test-only toolchain; never install/fetch during a gate.
    for line in (ROOT/'scripts/fixtures/ssh-channel-requirements.txt').read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        name, version = line.split('==')
        require(importlib.metadata.version(name) == version, 'SSH fault fixture toolchain version mismatch: '+name)
    helper = load('bridge_ssh_probe_helpers', ROOT/'scripts/test-plugin-ssh.py')
    binary = args.binary.resolve()
    require(binary.is_file(), 'build test_bridge_ssh first')
    work = Path(tempfile.mkdtemp(prefix='plugin-ssh-stream-', dir=ROOT/'tmp'))
    private, public = memory_identity()
    reports = []
    env = {**os.environ, 'TMPDIR':str(work), 'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1', 'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    env.pop('TMUX', None)
    env.pop('TMUX_PANE', None)
    cases = ('echo','diagnostic','flood','exit','truncated','pollution','cancel','allocation','before-admission','isolation',
             'deny','admission-cancel','admission-deadline','missing-exit','reset','channel-deny')
    legacy_cases=('legacy-zero','legacy-23','legacy-missing','legacy-signal','legacy-malformed','legacy-live','legacy-late','legacy-reset','legacy-duplicate','legacy-range')
    selected = args.case or list(legacy_cases if args.legacy else cases)
    require(all(case in cases+legacy_cases for case in selected), 'unknown bridge SSH case')
    require(not args.expect_assertion or len(selected) == 1, 'one mutation target required')
    barriers={mode:threading.Event() for mode in legacy_cases}
    def run(mode, port, user, host, worker_mode):
        known = work / ('known-'+mode)
        known.write_bytes(f'[bridge-target.invalid]:{port} '.encode()+host)
        command = shlex.join([str(binary),'--executor',worker_mode])
        shell = '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-ssh-stream; shift; gt_timeout 15 "$@"'
        process = subprocess.Popen(['bash','-c',shell,'plugin-ssh-stream',str(ROOT),str(binary),mode,str(port),user,str(known),command],
                                   cwd=ROOT,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        prefix=b''
        try:
            process.stdin.write(struct.pack('>I',len(private))+private);process.stdin.close();process.stdin=None
            if mode in ('legacy-live','legacy-late','legacy-reset'):
                require(select.select([process.stdout],[],[],10)[0], 'legacy query barrier timeout')
                prefix=process.stdout.readline()
                if prefix==b'LEGACY_QUERY_OBSERVED\n':
                    barriers[mode].set()
            stdout,stderr=process.communicate(timeout=20)
        finally:
            if process.poll() is None:
                process.kill();process.wait()
        output = (prefix+stdout+stderr).decode(errors='replace')
        (work/(mode+'.log')).write_text(output)
        if args.expect_assertion:
            require(process.returncode == 1 and 'FAIL ' in output and not any(x in output for x in ('AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer')),
                    mode+': expected normal C assertion failure; returncode='+str(process.returncode)+'\n'+output)
        else:
            require(process.returncode == 0 and 'PASS bridge_ssh_'+mode in output, mode+': '+output)
        reports.append(mode)
        print(('ASSERTION RED ' if args.expect_assertion else 'PASS bridge_ssh_')+mode, flush=True)
    try:
        with real_server(work, public, helper) as (port, host):
            for mode in selected:
                if mode in cases[:10]:
                    run(mode, port, pwd.getpwuid(os.getuid()).pw_name, host,
                        mode if mode in ('diagnostic','flood','exit','truncated','pollution') else 'echo')
        with fault_server(public,barriers) as (port, host, events):
            for mode in selected:
                if mode in cases[10:]+legacy_cases:
                    run(mode, port, mode, host, 'echo')
                    observed = events.get(mode, [])
                    require('channel' in observed and ('exec' in observed)==(mode!='channel-deny') and 'pty' not in observed and 'shell' not in observed,
                            'unexpected SSH requests: '+mode+' '+repr(observed))
        for path in work.rglob('*'):
            if path.is_file():
                data = path.read_bytes()
                require(b'PRIVATE KEY-----' not in data and b'openssh-key-v1\0' not in data, 'private fixture material serialized')
        receipt = dict(accepted=True, cases=reports, topology='loopback OpenSSH plus Paramiko 4.0.0 protocol faults',
                       expected_assertion=args.expect_assertion,
                       private_files=False, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
        (work/'result.json').write_text(json.dumps(receipt,indent=2)+'\n')
        print('PLUGIN SSH STREAM PASS: '+str(len(reports))+' cases; logs: '+str(work), flush=True)
    finally:
        # Only this run's public files/sockets/logs; preserve logs as evidence.
        for path in list(work.iterdir()):
            if path.is_socket():
                path.unlink()


if __name__ == '__main__':
    def interrupted(signum, frame):
        raise KeyboardInterrupt('signal '+str(signum))
    signal.signal(signal.SIGINT, interrupted)
    signal.signal(signal.SIGTERM, interrupted)
    pinned = ROOT/'build/ssh-channel-toolchain/bin/python'
    if Path(sys.prefix).resolve() != pinned.parent.parent.resolve():
        require(pinned.is_file(), 'prepare the pinned SSH fixture toolchain with scripts/bootstrap-ssh-channel-tests.sh')
        os.execv(str(pinned), [str(pinned), str(Path(__file__).resolve()), *sys.argv[1:]])
    main()
