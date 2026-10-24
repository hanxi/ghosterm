#!/usr/bin/env python3
"""Daemon command lifecycle: preserve unknown exit/time and value snapshots."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/session/command-status.c'
MARK='GT_MUTATION_SESSION_COMMAND_STATUS'
CASES=('lifecycle','unknown','clock','copy','restart','invalid')
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    files=(SOURCE,ROOT/'core/src/session/command-status.h',ROOT/'core/tests/test_command_status.c')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    original=SOURCE.read_bytes();text=original.decode()
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    variants=(
        ('missing exit code becomes success','next.has_exit_code=e->has_exit_code;',
         'next.has_exit_code=true;','unknown','lifecycle'),
        ('negative exit code discarded','next.exit_code=e->has_exit_code?e->exit_code:0;',
         'next.exit_code=e->has_exit_code && e->exit_code>0?e->exit_code:0;','lifecycle','clock'),
        ('end leaves command running','next.running=false;',
         'next.running=true;','lifecycle','invalid'),
        ('clockless start is not running','next.running=true;',
         'next.running=e->has_time;','clock','lifecycle'),
        ('missing start fabricates duration','s->running && s->has_start_time && e->has_time',
         'e->has_time','unknown','lifecycle'),
        ('backwards time wraps',' && e->monotonic_ns>=s->start_ns',
         '','clock','lifecycle'),
        ('zero duration treated as unknown','e->monotonic_ns>=s->start_ns',
         'e->monotonic_ns>s->start_ns','unknown','lifecycle'),
        ('new command retains old exit','next.has_exit_code=false;',
         '/* retain exit validity */','restart','copy'),
        ('new command retains old duration','next.has_duration=false;',
         '/* retain duration validity */','restart','invalid'),
        ('error text not copied','memmove(next.error,e->error,length);',
         'next.error[0]=0;','copy','unknown'),
        ('command text not copied','memmove(next.command,e->command,length);',
         'next.command[0]=0;','copy','unknown'),
        ('field length limit removed','for(size_t i=0;i<=4096;i++)',
         'for(size_t i=0;i<=8192;i++)','invalid','lifecycle'),
        ('prompt erases last command','if(e->kind!=3 && e->kind!=4)return GT_OK;',
         'if(e->kind!=3 && e->kind!=4){memset(s,0,sizeof(*s));return GT_OK;}','lifecycle','unknown'),
    )
    with tempfile.TemporaryDirectory(prefix='command-status-',dir=ROOT/'tmp') as directory:
        source=Path(directory)/SOURCE.name;binary=Path(directory)/'probe'
        def build(content):
            source.write_text(content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                           '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),'-I'+str(ROOT/'core/include'),
                           str(source),str(files[2]),'-o',str(binary)],check=True)
        def run(case):
            return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5).returncode
        try:
            build(text)
            for case in CASES:assert run(case)==0,case
            for label,old,new,target,control in variants:
                assert text.count(old)==1,label
                build(text.replace(old,new)+f'\n/* {MARK} */\n')
                assert run(target)!=0,label+': falsely green'
                assert run(control)==0,label+': control failed'
                print(label+': target red, control green',flush=True)
            build(text)
            for case in CASES:assert run(case)==0,case
        finally:
            source.write_bytes(original)
            assert hashlib.sha256(source.read_bytes()).digest()==hashes[SOURCE]
            for path,digest in hashes.items():assert hashlib.sha256(path.read_bytes()).digest()==digest,path
    print('PASS 6 command-status baselines, 13 mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
