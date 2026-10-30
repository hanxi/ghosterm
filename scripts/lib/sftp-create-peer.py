"""Exclusive-create protocol peer; verifies OPEN flags and every streamed byte."""
import socket
import struct
STAGES=('create-open','create-write','create-close')
ACTIONS=('create-short-handle','create-short-status')

def serve(channel,peer,closing):
    def u32(v):return struct.pack('>I',v)
    def string(v):return u32(len(v))+v
    def send(body):channel.sendall(u32(len(body))+body)
    def status(req,code):send(b'\x65'+req+u32(code)+string(b'fixture')+string(b''))
    pending=bytearray();channel.settimeout(.05)
    while not closing.is_set() and not channel.closed:
        try:block=channel.recv(65536)
        except socket.timeout:continue
        if not block:return
        pending.extend(block)
        while len(pending)>=4:
            length=struct.unpack('>I',pending[:4])[0]
            if length>1048576:raise ValueError('create packet too large')
            if len(pending)<length+4:break
            body=bytes(pending[4:length+4]);del pending[:length+4]
            stage={3:'create-open',6:'create-write',4:'create-close'}.get(body[0])
            if stage is None:raise ValueError('unexpected create request')
            req=body[1:5]
            if not peer.phase(stage):
                if peer.record['action']=='disconnect':return
                status(req,3);continue
            if stage=='create-open':
                action=peer.record['action']
                if action=='create-short-handle':send(b'\x66'+req+u32(0));continue
                if action=='create-short-status':send(b'\x65'+req);continue
                n=struct.unpack('>I',body[5:9])[0]
                flags,attrs,mode=struct.unpack('>III',body[9+n:21+n])
                if flags!=42 or attrs!=4 or mode&0o777!=0o600:status(req,3);continue
                send(b'\x66'+req+string(b'create-handle'))
            elif stage=='create-write':
                n=struct.unpack('>I',body[5:9])[0]
                offset,count=struct.unpack('>QI',body[9+n:21+n])
                data=body[21+n:]
                if len(data)!=count or any(v!=((offset+i)*31+7)&255 for i,v in enumerate(data)):
                    status(req,4);continue
                status(req,0)
            else:status(req,0)
