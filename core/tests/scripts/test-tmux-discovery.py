#!/usr/bin/env python3
"""Run PATH discovery against controlled login-shell environments."""
import os, pathlib, subprocess, sys, tempfile
root = pathlib.Path(__file__).resolve().parents[3]
with tempfile.TemporaryDirectory(prefix='tmux-path-', dir=root/'tmp') as work:
    base = pathlib.Path(work)
    dirs = [base/'first dir', base/'second', base/'bad']
    for directory, output in zip(dirs, ['tmux 3.1a', 'tmux 3.2a', 'unknown']):
        directory.mkdir(); executable = directory/'tmux'
        executable.write_text('#!/bin/sh\ncase " $* " in *" -N "*) exit 2;; esac\nprintf "%s\\n" '+repr(output)+'\n')
        executable.chmod(0o700)
    newline_dir = base/'evil\n'/'nested'; newline_dir.mkdir(parents=True)
    newline_exe = newline_dir/'tmux'; newline_exe.write_text('#!/bin/sh\necho tmux 3.2a\n'); newline_exe.chmod(0o700)
    shell = base/'login-shell'
    shell.write_text('#!/bin/sh\nPATH=$GT_TMUX_TEST_PATH; export PATH\nexec /bin/sh -c "$3"\n')
    shell.chmod(0o700)
    env = dict(os.environ, GT_TMUX_TEST_PATH=':'.join(map(str,[dirs[0],dirs[1],dirs[0],dirs[2]])),
               GT_TMUX_TEST_FIRST=str(dirs[0]/'tmux'), GT_TMUX_TEST_SECOND=str(dirs[1]/'tmux'),
               GT_TMUX_TEST_BAD=str(dirs[2]/'tmux'), GT_TMUX_TEST_NEWLINE=str(newline_dir), TMPDIR=work)
    subprocess.run([sys.argv[1], 'discover', str(shell), 'unused'], env=env, check=True, timeout=30)
