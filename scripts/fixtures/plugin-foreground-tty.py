"""Own a tmux PTY while the explicit foreground child uses its IPC socket.

All typed input is inert recorded data, both during and after the child. No
shell ever interprets it. The child retains ordinary TTY descriptors at start.
"""
import errno
import os
from pathlib import Path
import select
import subprocess
import sys
import termios
import tty

record=Path(sys.argv[1]);saved=termios.tcgetattr(0);tty.setraw(0)
child=None
try:
    child=subprocess.Popen(sys.argv[2:])
    Path(str(record)+'.ready').write_text(str(child.pid))
    total=0;exited=False
    while True:
        status=child.poll()
        if status is not None and not exited:
            Path(str(record)+'.status').write_text(str(status));exited=True
        if not select.select([0],[],[],.02)[0]:continue
        try:data=os.read(0,4096)
        except OSError as failure:
            if failure.errno==errno.EIO:break
            raise
        if not data:break
        total+=len(data)
        if total>4096:raise RuntimeError('Owned foreground input budget')
        with Path(str(record)+'.received').open('ab') as output:output.write(data)
finally:
    if child is not None and child.poll() is None:
        child.terminate()
        try:child.wait(timeout=6)
        except subprocess.TimeoutExpired:child.kill();child.wait(timeout=3)
    try:termios.tcsetattr(0,termios.TCSANOW,saved)
    except termios.error:pass
