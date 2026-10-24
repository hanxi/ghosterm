#!/usr/bin/env python3
"""Disposable TCP reset/blackhole relay for the real SSH reconnect fixture.
Command arguments may contain {host}/{port}; GT_TEST_RELAY_CONTROL is set only
for the child. Existing blackholed upstreams stay open after client disconnect,
so reattachment must wait for the real daemon's old-owner lease to expire.
"""
import argparse
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import threading
import time
ROOT=Path(__file__).resolve().parents[1]
class Relay:
    def __init__(self,target,control):
        self.target=target;self.control=control;self.connections=[];self.seen=[];self.accepted=0;self.blackhole=False;self.stopping=threading.Event();self.error=None
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen();self.listener.setblocking(False)
        self.commands=socket.socket(socket.AF_UNIX);self.commands.bind(str(control));self.commands.listen();self.commands.setblocking(False)
    def close_pair(self,p,reset=False):
        for s in p['s']:
            if s:
                if reset:
                    try:s.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0))
                    except OSError:pass
                s.close()
        if p in self.connections:self.connections.remove(p)
    def serve(self):
        try:
            while not self.stopping.is_set():
                readers=[self.listener,self.commands];writers=[];mapping={}
                for p in self.connections:
                    for i,s in enumerate(p['s']):
                        if not s:continue
                        mapping[s]=(p,i)
                        if p['blocked'] or len(p['q'][1-i])<262144:readers.append(s)
                        if p['q'][i] and not p['blocked']:writers.append(s)
                readable,writable,_=select.select(readers,writers,[],0.01)
                for s in readable:
                    if s is self.listener:
                        client,_=s.accept()
                        try:upstream=socket.create_connection(self.target,timeout=3)
                        except BaseException:client.close();raise
                        client.setblocking(False);upstream.setblocking(False)
                        self.accepted+=1;self.connections.append({'s':[client,upstream],'q':[bytearray(),bytearray()],'blocked':self.blackhole});continue
                    if s is self.commands:
                        channel,_=s.accept();channel.settimeout(1)
                        try:
                            command=channel.recv(32);self.seen.append(command)
                            if command==b'reset':
                                for p in list(self.connections):self.close_pair(p,True)
                            elif command==b'blackhole':
                                self.blackhole=True
                                for p in self.connections:p['blocked']=True;p['q']=[bytearray(),bytearray()]
                            elif command==b'online':self.blackhole=False
                            else:raise RuntimeError('invalid relay command')
                            print('relay '+command.decode(),flush=True);channel.sendall(b'OK')
                        finally:channel.close()
                        continue
                    p,i=mapping[s]
                    if p not in self.connections or p['s'][i] is not s:continue
                    try:data=s.recv(32768)
                    except BlockingIOError:continue
                    except (ConnectionResetError,BrokenPipeError):data=b''
                    if not data:
                        if p['blocked']:
                            # Withhold FIN from the other SSH peer. The server
                            # still owns the old session streams until timeout.
                            s.close();p['s'][i]=None
                            if not any(p['s']):self.close_pair(p)
                        else:self.close_pair(p)
                    elif not p['blocked']:p['q'][1-i].extend(data)
                for s in writable:
                    p,i=mapping[s]
                    if p not in self.connections or p['blocked'] or p['s'][i] is not s:continue
                    try:n=s.send(p['q'][i]);del p['q'][i][:n]
                    except BlockingIOError:pass
                    except (ConnectionResetError,BrokenPipeError):self.close_pair(p)
        except BaseException as e:self.error=e;self.stopping.set()
    def close(self):
        self.stopping.set();self.thread.join(timeout=4)
        for p in list(self.connections):self.close_pair(p)
        self.listener.close();self.commands.close()

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--target-host',required=True);p.add_argument('--target-port',required=True,type=int);p.add_argument('--timeout',default=120,type=int);p.add_argument('command',nargs=argparse.REMAINDER);args=p.parse_args()
    command=args.command[1:] if args.command[:1]==['--'] else args.command
    if not command:raise RuntimeError('fixture command required')
    (ROOT/'tmp').mkdir(exist_ok=True);work=Path(tempfile.mkdtemp(prefix='net-',dir=ROOT/'tmp'));relay=Relay((args.target_host,args.target_port),work/'control');child=None
    def interrupted(sig,frame):raise RuntimeError('network fixture interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    relay.thread=threading.Thread(target=relay.serve);relay.thread.start()
    try:
        env=dict(os.environ,GT_TEST_RELAY_CONTROL=str(relay.control),GT_TEST_RECONNECT='1',TMPDIR=str(ROOT/'tmp'))
        command=[x.replace('{host}','127.0.0.1').replace('{port}',str(relay.listener.getsockname()[1])) for x in command]
        child=subprocess.Popen(command,cwd=ROOT,env=env,start_new_session=True);deadline=time.monotonic()+args.timeout
        while child.poll() is None:
            if relay.error:raise RuntimeError('relay failed: '+str(relay.error))
            if time.monotonic()>=deadline:raise RuntimeError('network fixture deadline expired')
            time.sleep(0.02)
        if child.returncode:raise RuntimeError('SSH reconnect fixture failed: '+str(child.returncode))
        if relay.seen!=[b'reset',b'blackhole',b'online'] or relay.accepted<3:raise RuntimeError('missing real network fault/reconnection evidence')
        print('PASS real SSH reset, silent drop, old-upstream lease and fresh connection recovery',flush=True)
    finally:
        if child and child.poll() is None:os.killpg(child.pid,signal.SIGKILL);child.wait()
        relay.close();shutil.rmtree(work)
if __name__=='__main__':main()
