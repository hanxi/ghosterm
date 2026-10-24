#!/usr/bin/env python3
"""Fixed terminal-result encoding: no native layout or ambiguous hidden fields."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'daemon/probes/session-result-wire.c'
TEST=ROOT/'daemon/probes/session-result-wire-test.c'
CASES=('valid','capacity','magic','generation','flags','kind','status','sequence','exit','reserved','length')
MARK='GT_MUTATION_RESULT_WIRE'
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    files=(SOURCE,TEST,ROOT/'daemon/probes/session-result-wire.h')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    original=SOURCE.read_bytes();text=original.decode()
    variants=(
        ('magic ignored','memcmp(p,"GWR1",4)','false','magic','generation'),
        ('unknown flags ignored','flags&~3u','false','flags','reserved'),
        ('reserved bits ignored','get32(p+52)','false','reserved','magic'),
        ('trailing bytes accepted','n!=RSW_BYTES','n<RSW_BYTES','length','magic'),
        ('zero generation accepted','|| !g ||','|| false ||','generation','magic'),
        ('contradictory fields accepted','if(!valid(&r))return false;',
         '/* validation omitted */','kind','reserved'),
        ('last sequence lost','put64(p+16,r->sequence)','put64(p+16,0)','valid','magic'),
        ('capacity ignored','capacity<RSW_BYTES','((void)capacity,false)','capacity','magic'),
        ('failure leaves stale record','if(out)memset(out,0,sizeof(*out));','/* output not cleared */','reserved','valid'),
        ('failure leaves stale generation','if(generation)*generation=0;','/* generation not cleared */','reserved','valid'),
        ('preinit sequence falsely known',' || (r->stage==GT_WORKER_STAGE_STATE_OPEN && r->has_sequence)',
         '','sequence','magic'),
    )
    with tempfile.TemporaryDirectory(prefix='result-wire-',dir=ROOT/'tmp') as directory:
        work=Path(directory);source=work/SOURCE.name;binary=work/'probe'
        def build(content):
            source.write_text(content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/include'),'-I'+str(ROOT/'daemon/probes'),
                str(source),str(TEST),'-o',str(binary)],check=True)
        def run(case):return subprocess.run([str(binary),case],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=5).returncode
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
    print(f'PASS {len(CASES)} result-wire baselines, {len(variants)} mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
