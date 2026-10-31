"""Controlled directory creation: ordered stages, canonical paths and outcomes."""
import socket
import struct

STAGES=('mkdir-parent','mkdir-parent-real','mkdir-target','mkdir-create','mkdir-child',
        'mkdir-child-real','mkdir-open','mkdir-read','mkdir-eof','mkdir-close')
ACTIONS=('mkdir-conflict','mkdir-missing-type','mkdir-file','mkdir-link','mkdir-alias',
         'mkdir-relative','mkdir-nul','mkdir-nonempty','mkdir-duplicate-dot',
         'mkdir-unsupported','mkdir-unknown','mkdir-lost','mkdir-status-eof','mkdir-race',
         'mkdir-malformed','mkdir-total-budget')

def serve(channel,peer,closing):
    def u32(v):return struct.pack('>I',v)
    def string(v):return u32(len(v))+v
    def send(body):channel.sendall(u32(len(body))+body)
    def status(req,code):send(b'\x65'+req+u32(code)+string(b'fixture')+string(b''))
    def get_string(body,offset):
        n=struct.unpack('>I',body[offset:offset+4])[0];end=offset+4+n
        if end>len(body):raise ValueError('Invalid mkdir string')
        return body[offset+4:end],end
    def names(req,items):
        body=b'\x68'+req+u32(len(items))
        for name in items:body+=string(name)+string(b'')+u32(4)+u32(0o40700)
        send(body)
    pending=bytearray();index=0;parent=None;target=None
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try:block=channel.recv(65536)
        except socket.timeout:continue
        if not block:return
        pending.extend(block)
        while len(pending)>=4:
            length=struct.unpack('>I',pending[:4])[0]
            if length>131100:raise ValueError('Mkdir packet too large')
            if len(pending)<length+4:break
            body=bytes(pending[4:length+4]);del pending[:length+4]
            if index>=len(STAGES):peer.phase('mkdir-unexpected');raise ValueError('Mkdir retry forbidden')
            stage=STAGES[index];index+=1
            operation={'mkdir-parent':7,'mkdir-parent-real':16,'mkdir-target':7,'mkdir-create':14,
                       'mkdir-child':7,'mkdir-child-real':16,'mkdir-open':11,'mkdir-read':12,'mkdir-eof':12,'mkdir-close':4}[stage]
            if body[0]!=operation:peer.phase('mkdir-unexpected');raise ValueError('Wrong mkdir stage')
            path,end=get_string(body,5)
            if parent is None:parent=path;target=parent.rstrip(b'/')+b'/child'
            expected=parent if stage in STAGES[:2] else b'dir' if stage in STAGES[7:] else target
            if path!=expected:peer.phase('mkdir-wrong-path');raise ValueError('Wrong directory path')
            if stage=='mkdir-create':
                if body[end:]!=u32(4)+u32(0o40700):peer.phase('mkdir-wrong-mode');raise ValueError('Wrong directory permissions')
            elif end!=len(body):raise ValueError('Unexpected directory request tail')
            req=body[1:5];variant=peer.record['action'];action=variant if peer.record['stage']==stage else 'allow'
            if not peer.phase(stage):
                if action=='disconnect':return
                status(req,3);continue
            if variant=='mkdir-total-budget' and stage in STAGES[:2]:peer.record['release'].wait(.24)
            if action=='eof':
                channel.sendall(u32(16384)+b'\x65'+req);channel.shutdown_write();peer.record['release'].wait(15);return
            code={'mkdir-unsupported':8,'mkdir-unknown':0xdeadbeef,'mkdir-lost':7,'mkdir-status-eof':1,'mkdir-race':4}.get(action)
            if code is not None:status(req,code);continue
            if action=='mkdir-malformed':send(b'\x65'+req);continue
            if stage in ('mkdir-parent','mkdir-child','mkdir-target'):
                if stage=='mkdir-target' and action!='mkdir-conflict':status(req,2);continue
                mode=0o120777 if action=='mkdir-link' else 0o100600 if action=='mkdir-file' else 0o40700
                send(b'\x69'+req+(u32(0) if action=='mkdir-missing-type' else u32(4)+u32(mode)))
            elif stage.endswith('-real'):
                canonical=path+b'-elsewhere' if action=='mkdir-alias' else b'relative' if action=='mkdir-relative' else path+b'\x00bad' if action=='mkdir-nul' else path
                names(req,[canonical])
            elif stage=='mkdir-create' or stage=='mkdir-close':status(req,0)
            elif stage=='mkdir-open':send(b'\x66'+req+string(b'dir'))
            elif stage=='mkdir-read':names(req,[b'file'] if action=='mkdir-nonempty' else [b'.',b'.'] if action=='mkdir-duplicate-dot' else [b'.',b'..'])
            elif stage=='mkdir-eof':status(req,1)
