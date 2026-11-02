"""Strict SFTP-v3 removal stages and acknowledgement faults; no filesystem writes."""
import socket
import struct
STAGES=('remove-parent','remove-parent-real','remove-target','remove-parent-recheck','remove-target-recheck','remove-delete')
ACTIONS=('remove-absent','remove-missing-type','remove-missing-size','remove-missing-time',
         'remove-size','remove-time','remove-alias','remove-unsupported','remove-unknown',
         'remove-lost','remove-malformed','remove-directory','remove-total-budget')
def serve(channel,peer,closing):
    def u32(v):return struct.pack('>I',v)
    def string(v):return u32(len(v))+v
    def send(body):channel.sendall(u32(len(body))+body)
    def status(req,code):send(b'\x65'+req+u32(code)+string(b'fixture')+string(b''))
    def text(body):
        if len(body)<9:raise ValueError('Truncated remove request')
        n=struct.unpack('>I',body[5:9])[0]
        if n>65535 or len(body)!=9+n:raise ValueError('Invalid remove request')
        return body[9:]
    pending=bytearray();index=0;parent=None;target=None
    channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try:block=channel.recv(65536)
        except socket.timeout:continue
        if not block:return
        pending.extend(block)
        while len(pending)>=4:
            length=struct.unpack('>I',pending[:4])[0]
            if length>65544:raise ValueError('Remove packet too large')
            if len(pending)<length+4:break
            body=bytes(pending[4:length+4]);del pending[:length+4]
            if index>=len(STAGES):peer.phase('remove-unexpected');raise ValueError('Deletion must not retry')
            stage=STAGES[index];index+=1;variant=peer.record['action']
            operation=15 if stage=='remove-delete' and variant=='remove-directory' else 13 if stage=='remove-delete' else 16 if stage in ('remove-parent-real','remove-parent-recheck') else 7
            if body[0]!=operation:peer.phase('remove-unexpected');raise ValueError('Wrong removal stage')
            path=text(body)
            if parent is None:parent=path;target=parent.rstrip(b'/')+b'/child'
            expected=target if stage in ('remove-target','remove-target-recheck','remove-delete') else parent
            if path!=expected:peer.phase('remove-wrong-path');raise ValueError('Wrong removal path')
            action=variant if peer.record['stage']==stage else 'allow';req=body[1:5]
            if not peer.phase(stage):
                if action=='disconnect':return
                status(req,3);continue
            if variant=='remove-total-budget' and stage in STAGES[:2]:peer.record['release'].wait(.12)
            if action=='eof':channel.sendall(u32(16384)+b'\x65'+req);channel.shutdown_write();return
            code={'remove-absent':2,'remove-unsupported':8,'remove-unknown':222,'remove-lost':7}.get(action)
            if code is not None:status(req,code);continue
            if action=='remove-malformed':send(b'\x65'+req);continue
            if stage in ('remove-parent-real','remove-parent-recheck'):
                canonical=path+b'-elsewhere' if action=='remove-alias' else path
                send(b'\x68'+req+u32(1)+string(canonical)+string(b'')+u32(4)+u32(0o40700))
            elif stage=='remove-delete':status(req,0)
            else:
                directory=stage=='remove-parent' or variant=='remove-directory'
                flags=13
                if action=='remove-missing-type':flags&=~4
                if action=='remove-missing-size':flags&=~1
                if action=='remove-missing-time':flags&=~8
                attrs=u32(flags)
                if flags&1:attrs+=struct.pack('>Q',5 if action=='remove-size' else 0 if directory else 4)
                if flags&4:attrs+=u32(0o40700 if directory else 0o100600)
                if flags&8:attrs+=u32(9)+u32(10 if action=='remove-time' else 9)
                send(b'\x69'+req+attrs)
