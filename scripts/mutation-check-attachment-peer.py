#!/usr/bin/env python3
"""Unix peer death, generation ownership and bounded I/O; isolated mutations."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/ipc/attachment-peer.c'
MARK='GT_MUTATION_ATTACHMENT_PEER'
CASES=('flow','kill','stale','pressure','signals','invalidated-fd','running','borrowed',
       'exec','invalid','eof','send-close','pending','allocator')
FILES=(SOURCE,ROOT/'core/src/ipc/attachment-peer.h',ROOT/'core/tests/test_attachment_peer.c',
       *(ROOT/'core/src/session'/name for name in ('attachment.c','attachment.h',
         'checkpoint-task.c','checkpoint-task.h','restore-stream.c','restore-stream.h')))

def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    original=SOURCE.read_bytes();text=original.decode()
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in FILES}
    if any(MARK in p.read_text() for p in FILES):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    subprocess.run(['bash',str(ROOT/'scripts/build-core.sh')],check=True,stdout=subprocess.DEVNULL)
    variants=(
        ('peer bypasses allocation domain','p=domain.allocate(domain.context,sizeof(*p))',
         'p=malloc(sizeof(*p))','allocator','flow',1),
        ('peer close releases wrong size','domain.release(domain.context,p,sizeof(*p));\n}',
         'domain.release(domain.context,p,sizeof(*p)-1);\n}','allocator','flow',1),
        ('dead peer keeps attachment slot',
         '(void)gt__attachment_detach(p->attachment,p->generation);','/* detach omitted */','kill','stale',1),
        ('EOF mistaken for temporary wait',
         'if(descriptor.revents&POLLHUP)return finish(p,GT_PEER_CLOSED,0);',
         'if(descriptor.revents&POLLHUP)return result(GT_PEER_WAIT,0,0);','eof','flow',1),
        ('unaccepted input delivered after hangup',
         'if(descriptor.revents&POLLHUP)return finish(p,GT_PEER_CLOSED,0);',
         'if(false && (descriptor.revents&POLLHUP))return finish(p,GT_PEER_CLOSED,0);','pending','flow',1),
        ('closed socket descriptor leaked',
         'close(p->fd);p->fd=-1;','p->fd=-1;','eof','flow',1),
        ('old owner revokes replacement',
         'gt__attachment_detach(p->attachment,p->generation)',
         'gt__attachment_detach(p->attachment,p->generation+!gt__attachment_current(p->attachment,p->generation))',
         'stale','flow',1),
        ('stale connection still sends',
         'if(!gt__attachment_current(p->attachment,p->generation))return finish(p,GT_PEER_STALE,0);',
         '/* stale check omitted */','stale','flow',2),
        ('attachment socket inherited across exec',
         'descriptor|FD_CLOEXEC','descriptor','exec','invalid',1),
        ('blocking socket send freezes owner',
         'flags|O_NONBLOCK','flags','pressure','exec',1),
        ('partial write reported as whole buffer',
         'if(count>0)return result(GT_PEER_DATA,(size_t)count,0);',
         'if(count>0)return result(GT_PEER_DATA,sending ? length : (size_t)count,0);','pressure','flow',1),
        ('signal restarts full timeout',
         '    if(!milliseconds(&now))return finish(p,GT_PEER_ERROR,errno);',
         '    now=deadline-timeout;','signals','flow',1),
        ('backpressure revokes healthy attachment',
         'if(!ready || now>=deadline)return result(GT_PEER_WAIT,0,0);',
         'if(!ready || now>=deadline)return finish(p,GT_PEER_CLOSED,0);','flow','eof',1),
        ('socket error folded into clean EOF',
         'if(descriptor.revents&POLLNVAL)return finish(p,GT_PEER_ERROR,EBADF);',
         'if(descriptor.revents&POLLNVAL)return finish(p,GT_PEER_CLOSED,0);','invalidated-fd','flow',1),
        ('received bytes corrupted',
         'if(count>0)return result(GT_PEER_DATA,(size_t)count,0);',
         'if(count>0){if(!sending)memset(bytes,0,(size_t)count);return result(GT_PEER_DATA,(size_t)count,0);}',
         'flow','exec',1),
        ('wrong generation admitted',
         'fd<0 || !gt__attachment_current(a,g)','fd<0','invalid','flow',1),
        ('datagram accepted as stream',
         'type!=SOCK_STREAM','false','invalid','flow',1),
    )
    with tempfile.TemporaryDirectory(prefix='attachment-peer-',dir=ROOT/'tmp') as directory:
        work=Path(directory);source=work/SOURCE.name;binary=work/'peer-probe'
        def build(content):
            source.write_text(content.replace('#include "attachment-peer.h"', '#include "attachment-peer.h"\n#include <string.h>'))
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/ipc'),'-I'+str(ROOT/'core/include'),
                str(source),str(ROOT/'core/tests/test_attachment_peer.c'),
                str(ROOT/'core/build/libghosterm_core.a'),'-pthread','-o',str(binary)],check=True)
        def run(case):
            try:
                return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,
                                      timeout=5).returncode
            except subprocess.TimeoutExpired:
                return 'timeout'
        try:
            build(text)
            for case in CASES:assert run(case)==0,case+': baseline failed'
            for label,old,new,target,control,count in variants:
                assert text.count(old)==count,label+': mutation not unique'
                build(text.replace(old,new)+f'\n/* {MARK} */\n')
                assert run(target)!=0,label+': target falsely green'
                assert run(control)==0,label+': control failed'
                print(label+': target red, control green',flush=True)
            build(text)
            for case in CASES:assert run(case)==0,case+': rollback failed'
        finally:
            source.write_bytes(original)
            assert hashlib.sha256(source.read_bytes()).digest()==hashes[SOURCE]
            for path,digest in hashes.items():assert hashlib.sha256(path.read_bytes()).digest()==digest,path
    print(f'PASS {len(CASES)} peer baselines, {len(variants)} mutations, SHA256 rollback and baselines')
    return 0

if __name__=='__main__':raise SystemExit(main())
