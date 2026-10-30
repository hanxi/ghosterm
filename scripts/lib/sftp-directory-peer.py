"""Test-only SFTP v3 directory replies. Authentication is handled by the SSH peer."""
import socket
import struct

STAGES = ('realpath', 'opendir', 'readdir', 'closedir')
ACTIONS = ('empty', 'empty-name', 'slash-name', 'nul-name', 'duplicate',
           'malformed-path', 'relative-path', 'long-name', 'invalid-utf8-name')


def serve(channel, peer, closing):
    def u32(value): return struct.pack('>I', value)
    def string(value): return u32(len(value)) + value
    def send(body): channel.sendall(u32(len(body)) + body)
    def status(request, code): send(b'\x65' + request + u32(code) + string(b'fixture') + string(b''))
    def entry(name, flags=0, size=0, mode=0):
        attrs = u32(flags)
        if flags & 1: attrs += struct.pack('>Q', size)
        if flags & 4: attrs += u32(mode)
        if flags & 8: attrs += u32(10) + u32(20)
        return string(name) + string(b'') + attrs
    pending = bytearray()
    reads = 0
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try:
            block = channel.recv(65536)
        except socket.timeout:
            continue
        if not block: return
        pending.extend(block)
        while len(pending) >= 4:
            length = struct.unpack('>I', pending[:4])[0]
            if length > 1024 * 1024: raise ValueError('Fixture request too large')
            if len(pending) < length + 4: break
            body = bytes(pending[4:length+4]); del pending[:length+4]
            if len(body) < 5: raise ValueError('Invalid fixture request')
            stage = {16: 'realpath', 11: 'opendir', 12: 'readdir', 4: 'closedir'}.get(body[0])
            if stage is None: raise ValueError('Unexpected SFTP directory request')
            request = body[1:5]
            action = peer.record['action'] if peer.record['stage'] == stage else 'allow'
            if not peer.phase(stage):
                if action == 'disconnect': return
                status(request, 3); continue
            if action in ('eof', 'partial'):
                channel.sendall(u32(16384) + b'\x68' + request)
                if action == 'eof': channel.shutdown_write()
                peer.record['release'].wait(15)
                return
            if stage == 'realpath':
                if action == 'malformed-path':
                    # Declares 512 bytes but supplies one, smaller than the client buffer.
                    send(b'\x68' + request + u32(1) + u32(512) + b'/')
                else:
                    path = b'relative' if action == 'relative-path' else b'/fixture'
                    send(b'\x68' + request + u32(1) + entry(path))
            elif stage == 'opendir':
                send(b'\x66' + request + string(b'directory-handle'))
            elif stage == 'closedir':
                status(request, 0)
            elif reads:
                status(request, 1)
            else:
                reads += 1
                names = [entry(b'zero', 13, 0, 0o100644), entry(b'unknown'),
                         entry(b'folder', 4, mode=0o40755), entry(b'link', 4, mode=0o120777),
                         entry('数据.txt'.encode(), 13, 7, 0o100600)]
                if action == 'empty': status(request, 1); continue
                bad = {'empty-name': b'', 'slash-name': b'../escape', 'nul-name': b'a\0b',
                       'long-name': b'x' * 5000, 'invalid-utf8-name': b'\xff'}.get(action)
                if bad is not None: names = [entry(bad)]
                if action == 'duplicate': names = [entry(b'same'), entry(b'same')]
                send(b'\x68' + request + u32(len(names)) + b''.join(names))
