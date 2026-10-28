"""Test-only TCP proxy. Faults affect existing links; fresh SSH connections work."""
import pathlib, select, socket, struct, threading, time

class SSHFaultProxy:
    def __init__(self, host, port, work):
        self.target=(host,int(port)); self.work=pathlib.Path(work)
        self.listener=socket.socket();self.listener.bind(('127.0.0.1',0));self.listener.listen()
        self.listener.settimeout(.05);self.port=self.listener.getsockname()[1]
        self.lock=threading.Lock();self.links=[];self.stopped=threading.Event()
        self.thread=threading.Thread(target=self.run,daemon=True);self.thread.start()
    def fault(self, mode):
        with self.lock:
            for link in list(self.links):
                link[2]='normal' if mode=='resume' else mode
                if mode=='reset':
                    for sock in link[:2]:
                        try:sock.setsockopt(socket.SOL_SOCKET,socket.SO_LINGER,struct.pack('ii',1,0));sock.close()
                        except OSError:pass
    def relay(self, link):
        left,right=link[:2]
        # One bounded queue in each direction. Backpressure must pause IO,
        # never drop ciphertext or turn a forwarding timeout into a reset.
        pending={left:bytearray(),right:bytearray()}
        peer={left:right,right:left}
        try:
            while not self.stopped.is_set():
                if link[2]=='blackhole':self.stopped.wait(.01);continue
                if link[2]=='reset':return
                readable=[sock for sock in [left,right] if len(pending[peer[sock]])<1024*1024]
                writable=[sock for sock in [left,right] if pending[sock]]
                ready,write_ready,_=select.select(readable,writable,[],.02)
                if link[2]!='normal':continue
                for destination in write_ready:
                    try:sent=destination.send(pending[destination])
                    except BlockingIOError:continue
                    if not sent:return
                    del pending[destination][:sent]
                for source in ready:
                    try:data=source.recv(65536)
                    except BlockingIOError:continue
                    if not data:return
                    pending[peer[source]].extend(data)
        except (OSError,ValueError):pass
        finally:
            for sock in [left,right]:
                try:sock.close()
                except OSError:pass
            with self.lock:
                if link in self.links:self.links.remove(link)
    def run(self):
        while not self.stopped.is_set():
            for mode in ['reset','blackhole','resume']:
                marker=self.work/('fault-'+mode)
                if marker.exists():
                    marker.unlink();self.fault(mode)
                    ack=self.work/'fault-done.pending';ack.write_text(mode);ack.replace(self.work/'fault-done')
            try:left,_=self.listener.accept()
            except socket.timeout:continue
            except OSError:return
            try:
                right=socket.create_connection(self.target,timeout=5)
                left.setblocking(False);right.setblocking(False);link=[left,right,'normal']
                with self.lock:self.links.append(link)
                threading.Thread(target=self.relay,args=(link,),daemon=True).start()
            except OSError:left.close()
    def close(self):
        self.stopped.set();self.listener.close();self.fault('reset');self.thread.join(timeout=2)
        if self.thread.is_alive():raise RuntimeError('test proxy did not stop')
