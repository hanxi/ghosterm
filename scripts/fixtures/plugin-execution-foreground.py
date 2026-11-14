"""Owned foreground Executor, independent of the SSH attacher process.

The caller owns the exact startup argv; no shell discovery or user endpoint.
Only the public random instance ID is returned to the Native test payload.
"""
import os
from pathlib import Path
import re
import signal
import subprocess
import time


class Foreground:
    def __init__(self, command, environment, observation, endpoint, identity):
        self.command,self.environment,self.observation,self.endpoint,self.identity=command,environment,observation,endpoint,identity
        self.process=None;self.pid=None;self.expected=None
    def __enter__(self):
        self.process=subprocess.Popen(list(map(str,self.command)),env=self.environment,
            stdin=subprocess.DEVNULL,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            end=time.monotonic()+8
            while time.monotonic()<end:
                log=self.observation/'stderr';marker=self.observation/'gterm-pid'
                if log.exists() and marker.exists():
                    match=re.search(r'^gterm: local endpoint ready instance=([a-f0-9]{32})$',log.read_text(),re.M)
                    if match:
                        self.instance=match.group(1);self.pid=int(marker.read_text());self.expected=self.identity(self.pid)
                        if self.expected and '--listen' in self.expected and str(self.endpoint) in self.expected:return self
                if self.process.poll() is not None:raise RuntimeError('Owned foreground exited before IPC readiness')
                time.sleep(.01)
            raise RuntimeError('Owned foreground did not publish its public instance ID')
        except BaseException:self.close();raise
    def stop(self):
        if self.pid and self.identity(self.pid)==self.expected:os.kill(self.pid,signal.SIGTERM)
    def close(self):
        if self.process is None:return
        # Read marker even if startup failed before public readiness. The exact
        # --listen endpoint must still match before signalling this child.
        if self.pid is None:
            marker=self.observation/'gterm-pid'
            if marker.exists():
                pid=int(marker.read_text());current=self.identity(pid)
                if current and '--listen' in current and str(self.endpoint) in current:self.pid,self.expected=pid,current
        self.stop()
        try:self.process.communicate(timeout=8)
        except subprocess.TimeoutExpired:
            if self.pid and self.identity(self.pid)==self.expected:os.kill(self.pid,signal.SIGKILL)
            self.process.kill();self.process.communicate(timeout=3)
            raise RuntimeError('Owned foreground exceeded cleanup budget')
        if self.pid and self.identity(self.pid)==self.expected:raise RuntimeError('Owned foreground identity remained live')
    def __exit__(self,*_):self.close()
