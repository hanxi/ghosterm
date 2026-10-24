#!/usr/bin/env python3
"""Private daemon program-status storage: bounded copies and subtree removal."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=ROOT/'core/src/session/program-status.c'
MARK='GT_MUTATION_SESSION_PROGRAM_STATUS'
CASES=('copy','clear','aliases','limits','states','invalid','prompt','allocator','domains')
def interrupted(signum,frame):raise KeyboardInterrupt

def main():
    files=(SOURCE,ROOT/'core/src/session/program-status.h',ROOT/'core/tests/test_program_status.c')
    hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in files}
    if any(MARK in p.read_text() for p in files):return 2
    original=SOURCE.read_bytes();text=original.decode()
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
    variants=(
        ('destination domain ignored','gt__program_status_open_with_allocator(s->capacity,s->limit,allocator)',
         'gt__program_status_open_with_allocator(s->capacity,s->limit,((void)allocator,&s->allocator))','domains','states'),
        ('partial destination leaked','if(rc!=GT_STATUS_OK){gt__program_status_close(copy);return rc;}',
         'if(rc!=GT_STATUS_OK){return rc;}','domains','states'),
        ('store object escapes allocator','s=a.allocate(a.context,sizeof(*s))',
         's=malloc(sizeof(*s))','allocator','states'),
        ('entry array escapes allocator','s->entries=a.allocate(a.context,records*sizeof(*s->entries))',
         's->entries=malloc(records*sizeof(*s->entries))','allocator','states'),
        ('strings escape allocator','copy=s->allocator.allocate(s->allocator.context,total)',
         'copy=malloc(total)','allocator','states'),
        ('copy forgets allocation domain','gt__program_status_copy_with_allocator(s,s?&s->allocator:NULL,out)',
         'gt__program_status_copy_with_allocator(s,NULL,out)','allocator','states'),
        ('array release size wrong','s->capacity*sizeof(*s->entries));',
         's->count*sizeof(*s->entries));','allocator','states'),
        ('input prompt clears program state','if(!s || kind!=1)return 0;',
         'if(!s || (kind!=1 && kind!=2))return 0;','prompt','states'),
        ('working record retained at prompt','e->record.state==1 || e->record.state==3',
         'false || e->record.state==3','prompt','states'),
        ('blocked record retained at prompt','e->record.state==1 || e->record.state==3',
         'e->record.state==1 || false','prompt','states'),
        ('done descendants removed at prompt','e->record.state==1 || e->record.state==3',
         'e->record.state!=0','prompt','states'),
        ('slash boundary ignored',"id[n]=='/'","true",'clear','states'),
        ('clear deletes parent but retains descendants',
         '(!strncmp(id,parent,n) && id[n]==\'/\')','(false && !strncmp(id,parent,n))','clear','states'),
        ('empty clear does not clear all','return !n ||','return false ||','clear','states'),
        ('record cap ignored','index==s->count && s->count==s->capacity','false','limits','states'),
        ('string cap ignored','total>s->limit-(s->used-old)','false','limits','states'),
        ('old bytes charged on overwrite','s->used=s->used-old+total','s->used=s->used+total','limits','states'),
        ('progress discarded','gt_program_status_record_t stored=*r;',
         'gt_program_status_record_t stored=*r;stored.progress=-1;','states','clear'),
        ('snapshot entries skipped','for(size_t i=0;i<s->count;i++) {\n    gt_status_store_result_t rc=',
         'for(size_t i=0;i<0;i++) {\n    gt_status_store_result_t rc=','copy','states'),
        ('text silently replaced','memcpy(copy+offset,strings[i],lengths[i]);',
         'memset(copy+offset,0,lengths[i]);','copy','invalid'),
        ('field length bound ignored','for(size_t i=0;i<=4096;i++)',
         'for(size_t i=0;i<=8192;i++)','invalid','states'),
        ('borrowed clear id not copied',
         'char parent[4097];memcpy(parent,r->id,lengths[0]);','const char *parent=r->id;',
         'aliases','clear'),
        ('overwrite loses old slot','while(index<s->count && strcmp(s->entries[index].record.id,r->id))',
         'while(index<s->count)','states','clear'),
    )
    with tempfile.TemporaryDirectory(prefix='program-status-',dir=ROOT/'tmp') as directory:
        source=Path(directory)/SOURCE.name;binary=Path(directory)/'probe'
        def build(content):
            source.write_text(content)
            subprocess.run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
                           '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),
                           '-I'+str(ROOT/'core/include'),str(source),str(files[2]),'-o',str(binary)],check=True)
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
    print('PASS 9 program-status baselines, 23 mutations and SHA256 rollback')
    return 0
if __name__=='__main__':raise SystemExit(main())
