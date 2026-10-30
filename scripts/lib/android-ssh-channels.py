"""Test-only SSH protocol peer with explicit channel/PTY/shell barriers.

Each connection owns a Paramiko transport thread. Deliberately blocking its server
callback simulates a peer that received that exact request but never replies; this
is fault injection, not a production server design. Normal PTY/PID evidence stays
with the separate OpenSSH fixture. Control and SSH listeners bind loopback only.
"""
from contextlib import contextmanager
import base64, hashlib, hmac, json, logging, secrets, socket, socketserver, threading, time

@contextmanager
def fixture(work, enabled):
    if not enabled:
        yield {}
        return
    import paramiko
    from pathlib import Path
    if paramiko.__version__ != '4.0.0':
        raise RuntimeError('SSH channel fixture requires Paramiko 4.0.0')
    # Expected cancellation/reset errors must not dump protocol or auth material.
    logger = logging.getLogger('paramiko.transport')
    previous = logger.disabled
    logger.disabled = True
    key = paramiko.Ed25519Key.from_private_key_file(str(Path(work)/'hostkey'))
    client = (Path(work)/'clientkey.pub').read_text().split()[1]
    token = secrets.token_hex(32)
    lock = threading.Lock()
    records = {}
    class Peer(paramiko.ServerInterface):
        def __init__(self, connection):
            self.record = None
            self.connection = connection
        def get_allowed_auths(self, username): return 'publickey'
        def check_auth_publickey(self, username, supplied):
            with lock: self.record = records.get(username)
            return paramiko.AUTH_SUCCESSFUL if self.record and hmac.compare_digest(supplied.get_base64(),client) else paramiko.AUTH_FAILED
        def phase(self, stage):
            record = self.record
            if record is None: return False
            with lock:
                record['events'].append(stage)
                action = record['action'] if record['stage'] == stage else 'allow'
            if action == 'hold': record['release'].wait(15)
            if action == 'disconnect':
                self.connection.shutdown(socket.SHUT_RDWR)
                return False
            return action != 'deny'
        def check_channel_request(self, kind, chanid):
            return paramiko.OPEN_SUCCEEDED if kind == 'session' and self.phase('channel') else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
        def check_channel_pty_request(self, channel, term, width, height, pixelwidth, pixelheight, modes):
            return self.phase('pty')
        def check_channel_shell_request(self, channel): return self.phase('shell')
    class SSHHandler(socketserver.BaseRequestHandler):
        def handle(self):
            transport = paramiko.Transport(self.request)
            peer = Peer(self.request)
            try:
                transport.add_server_key(key)
                transport.start_server(server=peer)
                while transport.is_active() and not closing.is_set():
                    channel = transport.accept(0.05)
                    if channel:
                        # Retain accepted channels until this connection closes.
                        channels.append(channel)
            except (EOFError, OSError, paramiko.SSHException): pass
            finally: transport.close()
    class Control(socketserver.StreamRequestHandler):
        def handle(self):
            self.connection.settimeout(5)
            request = json.loads(self.rfile.readline(4096))
            if not hmac.compare_digest(str(request.get('token','')),token): return
            with lock:
                op = request['op']
                if op == 'new':
                    if request['stage'] not in ('channel','pty','shell') or request['action'] not in ('hold','deny','allow','disconnect'): raise ValueError('Invalid fixture mode')
                    name = 'channel-'+secrets.token_hex(12)
                    records[name] = dict(stage=request['stage'],action=request['action'],events=[],release=threading.Event())
                    result = dict(user=name)
                else:
                    record = records[request['user']]
                    if op == 'release': record['release'].set()
                    elif op != 'state': raise ValueError('Unknown fixture operation')
                    result = dict(events=list(record['events']))
            self.wfile.write(json.dumps(result).encode()+b'\n')
    closing = threading.Event()
    channels = []
    class Server(socketserver.ThreadingTCPServer):
        daemon_threads = True
    with Server(('127.0.0.1',0),SSHHandler) as ssh, Server(('127.0.0.1',0),Control) as control:
        workers = [threading.Thread(target=s.serve_forever,daemon=True) for s in (ssh,control)]
        for worker in workers: worker.start()
        try:
            yield dict(channels=dict(port=ssh.server_address[1],controlPort=control.server_address[1],token=token,
                public=key.get_name()+' '+key.get_base64(),
                fingerprint='SHA256:'+base64.b64encode(hashlib.sha256(key.asbytes()).digest()).decode().rstrip('=')))
        finally:
            closing.set()
            with lock:
                for record in records.values(): record['release'].set()
            ssh.shutdown(); control.shutdown()
            for worker in workers: worker.join()
            for channel in channels: channel.close()
            logger.disabled = previous
