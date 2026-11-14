"""Owned opaque TCP fault wire, used only to cut the selected SSH test connection."""
import select
import socket
import socketserver
import threading


class FaultWire:
    def __init__(self, target):
        self.target = target; self.lock = threading.Lock(); self.peers = set(); self.cut = False
    def __enter__(self):
        owner = self
        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                with socket.create_connection(('127.0.0.1', owner.target), timeout=5) as upstream:
                    pair = (self.request, upstream)
                    with owner.lock:
                        if owner.cut: return
                        owner.peers.update(pair)
                    try:
                        while True:
                            ready, _, _ = select.select(pair, [], [], .1)
                            with owner.lock:
                                if owner.cut: return
                            for source in ready:
                                data = source.recv(65536)
                                if not data: return
                                (upstream if source is self.request else self.request).sendall(data)
                    except (ConnectionError, OSError): pass
                    finally:
                        with owner.lock: owner.peers.difference_update(pair)
        self.server = socketserver.ThreadingTCPServer(('127.0.0.1', 0), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        self.thread = threading.Thread(target=self.server.serve_forever); self.thread.start()
        return self
    def disconnect(self):
        with self.lock:
            self.cut = True
            for peer in tuple(self.peers):
                try: peer.shutdown(socket.SHUT_RDWR)
                except OSError: pass
    def __exit__(self, *args):
        self.disconnect(); self.server.shutdown(); self.thread.join(); self.server.server_close()
