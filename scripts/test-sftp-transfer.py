#!/usr/bin/env python3
"""Run via ssh_e2e.sh with the pinned Paramiko interpreter."""
import importlib.util
import json
import os
from pathlib import Path
import socket
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CASES = [('download-open-deny', 'transfer-open'), ('download-read-deny', 'transfer-read'),
         ('upload-write-deny', 'transfer-write'), ('download-close-deny', 'transfer-close'),
         ('upload-close-deny', 'transfer-close')]


def main():
    if len(sys.argv) != 6: raise RuntimeError('Run through core/tests/scripts/ssh_e2e.sh')
    host, port, user, key, work = sys.argv[1:]
    binary = Path(os.environ.get('GHOSTERM_SFTP_TRANSFER_BIN', ROOT/'core/build/test_sftp_transfer')).resolve()
    selected = os.environ.get('SFTP_TRANSFER_CASE')
    spec = importlib.util.spec_from_file_location('channels', ROOT/'scripts/lib/android-ssh-channels.py')
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    def run(arguments):
        result = subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp-transfer; shift; gt_timeout 30 "$@"',
            'sftp-transfer',str(ROOT),str(binary),*arguments], capture_output=True,text=True)
        print(result.stdout + result.stderr, end='',flush=True)
        if result.returncode or 'SFTP TRANSFER PASS' not in result.stdout: raise RuntimeError('Transfer probe failed')
    if selected not in [name for name,_ in CASES]:
        run([host,port,user,key,work]+([selected] if selected else []))
        if selected: return
    with module.fixture(work,True) as data:
        peer = data['channels']
        def control(**fields):
            with socket.create_connection(('127.0.0.1',peer['controlPort']),timeout=2) as sock:
                sock.sendall((json.dumps(dict(token=peer['token'],**fields))+'\n').encode())
                return json.loads(sock.makefile().readline())
        for name,stage in CASES:
            if selected and selected!=name: continue
            directory=Path(work)/name; directory.mkdir()
            username=control(op='new',stage=stage,action='deny')['user']
            run(['127.0.0.1',str(peer['port']),username,key,str(directory),name])
            events=control(op='state',user=username)['events']
            unique=[]
            for event in events:
                if not unique or unique[-1]!=event: unique.append(event)
            expected=['channel','subsystem','version','transfer-open']
            if stage!='transfer-open':
                expected += ['transfer-write'] if name.startswith('upload') else ['transfer-fstat','transfer-read']
                expected += ['transfer-close']
            if unique!=expected: raise RuntimeError(f'{name}: unexpected protocol events {unique!r}')
            print('PASS protocol '+name,flush=True)
    print('SFTP TRANSFER PROTOCOL PASS',flush=True)


if __name__=='__main__': main()
