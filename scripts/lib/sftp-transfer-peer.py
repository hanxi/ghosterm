"""SFTP transfer faults on a real SSH channel; no production filesystem writes."""
import socket
import struct

STAGES = ('transfer-open', 'transfer-fstat', 'transfer-read', 'transfer-write', 'transfer-close')
CONTENT = b'copy\x00' + '传输'.encode() + b'\xff\n'


def serve(channel, peer, closing):
    def u32(v): return struct.pack('>I', v)
    def string(v): return u32(len(v)) + v
    def send(body): channel.sendall(u32(len(body)) + body)
    def status(req, code): send(b'\x65' + req + u32(code) + string(b'fixture') + string(b''))
    pending = bytearray()
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try: block = channel.recv(65536)
        except socket.timeout: continue
        if not block: return
        pending.extend(block)
        while len(pending) >= 4:
            length = struct.unpack('>I', pending[:4])[0]
            if length > 1024 * 1024: raise ValueError('Transfer fixture request too large')
            if len(pending) < length + 4: break
            body = bytes(pending[4:length+4]); del pending[:length+4]
            stage = {3:'transfer-open', 8:'transfer-fstat', 5:'transfer-read', 6:'transfer-write', 4:'transfer-close'}.get(body[0])
            if stage is None: raise ValueError('Unexpected transfer request')
            req = body[1:5]
            if not peer.phase(stage):
                status(req,3); continue
            if stage == 'transfer-open': send(b'\x66'+req+string(b'transfer-handle'))
            elif stage == 'transfer-fstat': send(b'\x69'+req+u32(5)+struct.pack('>Q',len(CONTENT))+u32(0o100644))
            elif stage == 'transfer-read':
                n = struct.unpack('>I',body[5:9])[0]
                offset,wanted = struct.unpack('>QI',body[9+n:21+n])
                data = CONTENT[offset:offset+wanted]
                if data: send(b'\x67'+req+string(data))
                else: status(req,1)
            else: status(req,0)
