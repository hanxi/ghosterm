"""Read-only preview protocol fixture; rejects unexpected/write requests."""
import socket
import struct

STAGES = ('preview-lstat', 'preview-open', 'preview-fstat', 'preview-read', 'preview-close')
ACTIONS = ('preview-empty', 'preview-exact', 'preview-unknown-size', 'preview-oversize', 'preview-unknown-type',
           'preview-link', 'preview-directory', 'preview-special', 'preview-growth', 'preview-shrink',
           'preview-overflow', 'preview-read-error', 'preview-late-cancel', 'preview-late-timeout')
CONTENT = b'hello\x00' + '远端预览'.encode() + b'\xff\n'


def serve(channel, peer, closing):
    def u32(v): return struct.pack('>I', v)
    def string(v): return u32(len(v)) + v
    def send(body): channel.sendall(u32(len(body)) + body)
    def status(req, code): send(b'\x65' + req + u32(code) + string(b'fixture') + string(b''))
    pending = bytearray()
    payload_sent = False
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try: block = channel.recv(65536)
        except socket.timeout: continue
        if not block: return
        pending.extend(block)
        while len(pending) >= 4:
            length = struct.unpack('>I', pending[:4])[0]
            if length > 1024 * 1024: raise ValueError('Preview fixture request too large')
            if len(pending) < length + 4: break
            body = bytes(pending[4:length+4]); del pending[:length+4]
            stage = {7:'preview-lstat',3:'preview-open',8:'preview-fstat',5:'preview-read',4:'preview-close'}.get(body[0])
            if stage is None: raise ValueError('Unexpected preview request')
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
            data = b'' if variant == 'preview-empty' else CONTENT
            if variant == 'preview-exact': data = b'x'*64
            if variant in ('preview-growth','preview-overflow'): data = b'x'*65
            if stage in ('preview-lstat','preview-fstat'):
                flags = 5; mode = 0o100644; size = len(data)
                if variant in ('preview-unknown-size','preview-overflow'): flags = 4
                if action == 'preview-oversize': size = 65
                if action == 'preview-unknown-type': flags = 1
                if action == 'preview-link': mode = 0o120777
                if action == 'preview-directory': mode = 0o40755
                if action == 'preview-special': mode = 0o10600
                if variant == 'preview-growth': size = 1
                if variant == 'preview-shrink': size = len(data)+1
                attrs = u32(flags)
                if flags & 1: attrs += struct.pack('>Q',size)
                if flags & 4: attrs += u32(mode)
                send(b'\x69'+req+attrs)
            elif stage == 'preview-open':
                n = struct.unpack('>I',body[5:9])[0]
                flags = struct.unpack('>I',body[9+n:13+n])[0]
                if flags != 1: status(req,3); continue
                send(b'\x66'+req+string(b'preview-handle'))
            elif stage == 'preview-close': status(req,0)
            else:
                if payload_sent and action in ('preview-late-cancel','preview-late-timeout'):
                    peer.phase('preview-after-data'); peer.record['release'].wait(15); return
                if payload_sent and action == 'preview-read-error': status(req,4); continue
                n = struct.unpack('>I',body[5:9])[0]
                offset, wanted = struct.unpack('>QI',body[9+n:21+n])
                block = data[offset:offset+min(wanted,7)]
                if block:
                    payload_sent = True; send(b'\x67'+req+string(block))
                else: status(req,1)
