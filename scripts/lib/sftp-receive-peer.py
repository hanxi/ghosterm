"""Read-only receive protocol fixture; rejects unexpected/write requests."""
import socket
import struct

STAGES = ('receive-lstat', 'receive-open', 'receive-fstat', 'receive-read', 'receive-finalstat', 'receive-close')
ACTIONS = ('receive-empty', 'receive-exact', 'receive-unknown-size', 'receive-oversize', 'receive-unknown-type',
           'receive-link', 'receive-directory', 'receive-special', 'receive-growth', 'receive-shrink',
           'receive-overflow', 'receive-read-error', 'receive-late-cancel', 'receive-late-timeout',
           'receive-changed-size','receive-changed-mtime','receive-missing-size','receive-missing-mtime')
CONTENT = bytes((i*31+7)%256 for i in range(257))


def serve(channel, peer, closing):
    def u32(v): return struct.pack('>I', v)
    def string(v): return u32(len(v)) + v
    def send(body): channel.sendall(u32(len(body)) + body)
    def status(req, code): send(b'\x65' + req + u32(code) + string(b'fixture') + string(b''))
    pending = bytearray()
    payload_sent = False
    stat_count = 0
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try: block = channel.recv(65536)
        except socket.timeout: continue
        if not block: return
        pending.extend(block)
        while len(pending) >= 4:
            length = struct.unpack('>I', pending[:4])[0]
            if length > 1024 * 1024: raise ValueError('Receive fixture request too large')
            if len(pending) < length + 4: break
            body = bytes(pending[4:length+4]); del pending[:length+4]
            stage = {7:'receive-lstat',3:'receive-open',8:'receive-fstat',5:'receive-read',4:'receive-close'}.get(body[0])
            if stage is None: raise ValueError('Unexpected receive request')
            if stage == 'receive-fstat':
                stat_count += 1
                if stat_count > 1: stage = 'receive-finalstat'
            req = body[1:5]
            action = peer.record['action'] if peer.record['stage'] == stage else 'allow'
            if not peer.phase(stage):
                if action == 'disconnect': return
                status(req,3); continue
            if action in ('eof','partial'):
                channel.sendall(u32(16384) + b'\x67' + req)
                if action == 'eof': channel.shutdown_write()
                peer.record['release'].wait(15); return
            variant = peer.record['action']
            data = b'' if variant == 'receive-empty' else CONTENT
            if variant == 'receive-exact': data = bytes((i*31+7)%256 for i in range(300))
            if variant in ('receive-growth','receive-overflow'): data = b'x'*301
            if stage in ('receive-lstat','receive-fstat','receive-finalstat'):
                flags = 13; mode = 0o100644; size = len(data);mtime = 12345
                if variant in ('receive-unknown-size','receive-overflow'): flags = 12
                if action == 'receive-oversize': size = 301
                if action == 'receive-unknown-type': flags = 1
                if action == 'receive-link': mode = 0o120777
                if action == 'receive-directory': mode = 0o40755
                if action == 'receive-special': mode = 0o10600
                if variant == 'receive-growth': size = 1
                if variant == 'receive-shrink': size = len(data)+1
                if action == 'receive-changed-size': size += 1
                if action == 'receive-changed-mtime': mtime += 1
                if action == 'receive-missing-size': flags &= ~1
                if action == 'receive-missing-mtime': flags &= ~8
                attrs = u32(flags)
                if flags & 1: attrs += struct.pack('>Q',size)
                if flags & 4: attrs += u32(mode)
                if flags & 8: attrs += u32(12345)+u32(mtime)
                send(b'\x69'+req+attrs)
            elif stage == 'receive-open':
                n = struct.unpack('>I',body[5:9])[0]
                flags = struct.unpack('>I',body[9+n:13+n])[0]
                if flags != 1: status(req,3); continue
                send(b'\x66'+req+string(b'receive-handle'))
            elif stage == 'receive-close': status(req,0)
            else:
                if payload_sent and action in ('receive-late-cancel','receive-late-timeout'):
                    peer.phase('receive-after-data'); peer.record['release'].wait(15); return
                if payload_sent and action == 'receive-read-error': status(req,4); continue
                n = struct.unpack('>I',body[5:9])[0]
                offset, wanted = struct.unpack('>QI',body[9+n:21+n])
                block = data[offset:offset+min(wanted,17)]
                if block:
                    payload_sent = True; send(b'\x67'+req+string(block))
                else: status(req,1)
