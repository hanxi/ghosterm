"""Publication peer: exact request sequence, path ownership and decoded status."""
import socket
import struct

STAGES = ('publish-source', 'publish-target', 'publish-recheck', 'publish-rename')
ACTIONS = ('publish-conflict', 'publish-missing-size', 'publish-missing-type',
           'publish-link', 'publish-size', 'publish-mtime', 'publish-missing-mtime',
           'publish-unsupported', 'publish-unknown', 'publish-connection-lost',
           'publish-status-eof', 'publish-race', 'publish-malformed', 'publish-total-budget')


def serve(channel, peer, closing):
    def u32(v): return struct.pack('>I', v)
    def string(v): return u32(len(v)) + v
    def send(body): channel.sendall(u32(len(body)) + body)
    def status(req, code): send(b'\x65' + req + u32(code) + string(b'fixture') + string(b''))
    def get_string(body, offset):
        length = struct.unpack('>I', body[offset:offset+4])[0]
        end = offset + 4 + length
        if end > len(body): raise ValueError('Invalid publication path length')
        return body[offset+4:end], end
    pending = bytearray(); index = 0; source = None
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try: block = channel.recv(65536)
        except socket.timeout: continue
        if not block: return
        pending.extend(block)
        while len(pending) >= 4:
            length = struct.unpack('>I', pending[:4])[0]
            if length > 131100: raise ValueError('Publication packet exceeds limit')
            if len(pending) < length+4: break
            body = bytes(pending[4:length+4]); del pending[:length+4]
            if index >= len(STAGES):
                peer.phase('publish-unexpected'); raise ValueError('Publication retry/fallback is forbidden')
            stage = STAGES[index]; index += 1
            if body[0] != (18 if stage == 'publish-rename' else 7):
                peer.phase('publish-unexpected'); raise ValueError('Wrong publication operation')
            path, end = get_string(body,5)
            if source is None: source = path
            target = source.removesuffix(b'-source') + b'-target'
            expected = target if stage == 'publish-target' else source
            if path != expected: raise ValueError('Wrong publication path')
            if stage == 'publish-rename':
                destination, end = get_string(body,end)
                if destination != target: raise ValueError('Wrong rename destination')
            if end != len(body): raise ValueError('Unexpected publication packet tail')
            req = body[1:5]
            variant = peer.record['action']
            action = variant if peer.record['stage'] == stage else 'allow'
            if not peer.phase(stage):
                if action == 'disconnect': return
                status(req,3); continue
            if variant == 'publish-total-budget' and stage in STAGES[:2]: peer.record['release'].wait(.24)
            if action == 'eof':
                channel.sendall(u32(16384) + b'\x65' + req); channel.shutdown_write()
                peer.record['release'].wait(15); return
            code = {'publish-unsupported':8,'publish-unknown':0xdeadbeef,
                    'publish-connection-lost':7,'publish-status-eof':1,'publish-race':4}.get(action)
            if code is not None: status(req,code); continue
            if action == 'publish-malformed': send(b'\x65'+req); continue
            if stage == 'publish-rename': status(req,0); continue
            if stage == 'publish-target' and action != 'publish-conflict': status(req,2); continue
            flags=13; mode=0o100600; size=4; mtime=12345
            if action == 'publish-missing-size': flags &= ~1
            if action == 'publish-missing-type': flags &= ~4
            if action == 'publish-missing-mtime': flags &= ~8
            if action == 'publish-link': mode=0o120777
            if action == 'publish-size': size += 1
            if action == 'publish-mtime': mtime += 1
            attrs=u32(flags)
            if flags & 1: attrs += struct.pack('>Q',size)
            if flags & 4: attrs += u32(mode)
            if flags & 8: attrs += u32(12345)+u32(mtime)
            send(b'\x69'+req+attrs)
