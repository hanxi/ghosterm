#!/usr/bin/env python3
"""Real SSH with tmux present only in the remote login shell's PATH."""
import os
from pathlib import Path
import shlex
import subprocess
import sys

host, port, user, key, work = sys.argv[1:]
folder = Path(work)
binary = os.environ['GT_TMUX_TEST_BINARY']
first, second, empty = (folder / name for name in ("first dir", "second' dir", 'empty'))
for directory in (first, second, empty):
    directory.mkdir()
for directory, version in ((first, 'tmux 3.1a'), (second, 'tmux 3.6a')):
    executable = directory / 'tmux'
    executable.write_text('#!/bin/sh\nprintf "%s\\n" '+shlex.quote(version)+'\n')
    executable.chmod(0o700)
login = folder / "login ' shell"
endpoint = folder / 'endpoint'
endpoint.write_text('#!/bin/sh\nexport PATH='+shlex.quote(str(empty))+
                    '\nexport SHELL='+shlex.quote(str(login))+
                    '\nexec /bin/sh -c "$SSH_ORIGINAL_COMMAND"\n')
endpoint.chmod(0o700)
option = shlex.quote(str(endpoint)).replace('\\', '\\\\').replace('"', '\\"')
(folder / 'authorized_keys').write_text('command="'+option+'" '+Path(key+'.pub').read_text())
for mode, path, startup in (
    ('discover-ssh', str(first)+':'+str(second)+':'+str(first), ''),
    ('login-open', str(second), ''),
    ('login-missing', str(empty), ''),
    ('login-dirty', str(second), "printf 'unexpected startup output\\n'\n"),
    ('login-stderr', str(second), "printf 'startup warning\\n' >&2\n"),
    ('login-failed', str(second), 'exit 73\n'),
):
    login.write_text('#!/bin/sh\n[ "$1" = -l ] && [ "$2" = -c ] || exit 74\n'+startup+
                     'export PATH='+shlex.quote(path)+'\nexec /bin/sh -c "$3"\n')
    login.chmod(0o700)
    subprocess.run([binary, mode, str(first/'tmux') if mode == 'discover-ssh' else str(second/'tmux'),
                    str(second/'tmux'), host, port, user, key, work], check=True, timeout=30)
print('PASS remote login PATH: order/dedup, default open, absence, stdout/stderr/failure')
