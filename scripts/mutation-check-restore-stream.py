#!/usr/bin/env python3
"""S/L restore boundaries and bounded byte-ring mutations."""
import hashlib
import os
from pathlib import Path
import signal
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parent.parent
SOURCE=ROOT/'core/src/session/restore-stream.c'
original=SOURCE.read_bytes();sha=hashlib.sha256(original).digest()
MARK='MUTATION_RESTORE_STREAM'
if MARK.encode() in original: raise SystemExit(2)
def interrupted(signum,frame): raise KeyboardInterrupt
for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
(ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp')
CASES=('flow','empty','limits','protocol','phases','reset','interrupt','invalid','allocator','ended')
with tempfile.TemporaryDirectory(prefix='restore-stream-',dir=ROOT/'tmp') as directory:
    source=Path(directory)/SOURCE.name;binary=Path(directory)/'probe'
    def build(text):
        source.write_text(text)
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                        '-I'+str(ROOT/'core/src'),'-I'+str(ROOT/'core/src/session'),
                        '-I'+str(ROOT/'core/include'),str(source),str(ROOT/'core/tests/test_restore_stream.c'),
                        '-o',str(binary)],check=True)
    def run(mode):
        return subprocess.run([str(binary),mode],stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL,timeout=10).returncode
    text=original.decode()
    variants=(
        ('end fence omitted','s->phase=GT_RESTORE_ENDED;return GT_RESTORE_OK;',
         'return GT_RESTORE_OK;','ended','flow'),
        ('ended accepts input','s->phase==GT_RESTORE_LIVE && sequence>=s->cut',
         '(s->phase==GT_RESTORE_LIVE || s->phase==GT_RESTORE_ENDED) && sequence>=s->cut','ended','flow'),
        ('ended cannot drain',' && s->phase!=GT_RESTORE_ENDED','', 'ended','flow'),
        ('post end push admitted','if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL; /* Preserve accepted tail. */',
         '/* end push admitted */','ended','flow'),
        ('end before final data','s->phase!=GT_RESTORE_LIVE || sequence>s->last',
         's->phase!=GT_RESTORE_LIVE','ended','flow'),
        ('late ready destroys final tail','if(s->phase==GT_RESTORE_ENDED)return GT_RESTORE_PROTOCOL;\n  if(s->phase!=GT_RESTORE_WAIT_L',
         'if(s->phase!=GT_RESTORE_WAIT_L','ended','flow'),
        ('bytes bypass domain','s->bytes=a.allocate(a.context,bytes)',
         's->bytes=malloc(bytes)','allocator','empty'),
        ('entries bypass domain','s->entries=a.allocate(a.context,events*sizeof(*s->entries))',
         's->entries=malloc(events*sizeof(*s->entries))','allocator','empty'),
        ('failed init retains bytes','{gt__restore_destroy(s);return false;}',
         '{return false;}','allocator','empty'),
        ('destroy loses byte charge','if(s->bytes)s->allocator.release(s->allocator.context,s->bytes,s->byte_limit);',
         '/* byte charge leaked */','allocator','invalid'),
        ('array release uses wrong size','s->event_limit*sizeof(*s->entries));',
         '(s->event_limit-1)*sizeof(*s->entries));','allocator','empty'),
        ('early input allowed','s->phase==GT_RESTORE_LIVE && sequence>=s->cut',
         's->phase!=GT_RESTORE_DETACHED && sequence>=s->cut','empty','invalid'),
        ('event gap accepted','sequence!=s->last+1','false','protocol','empty'),
        ('byte limit ignored','length>s->byte_limit-s->used','false','limits','empty'),
        ('header limit ignored','s->count==s->event_limit','false','limits','empty'),
        ('short read consumes frame','if(capacity<entry->length)return GT_RESTORE_SPACE;',
         'if(capacity<entry->length){s->head=(s->head+1)%s->event_limit;s->count--;return GT_RESTORE_SPACE;}','flow','empty'),
        ('payload copied wrongly','if(first)memcpy(buffer,s->bytes+entry->offset,first);',
         'if(first)memset(buffer,0,first);','flow','empty'),
        ('wrapped payload lost','if(entry->length>first)memcpy((uint8_t *)buffer+first,s->bytes,entry->length-first);',
         'if(entry->length>first)memset((uint8_t *)buffer+first,0,entry->length-first);','flow','empty'),
        ('L changes to S','s->cut=s->last;s->phase=GT_RESTORE_DRAINING;',
         's->cut=s->snapshot;s->phase=GT_RESTORE_DRAINING;','flow','empty'),
        ('delta passes L before marker','if(s->phase==GT_RESTORE_DRAINING && s->dequeued==s->cut)return GT_RESTORE_WAIT;',
         '/* cut barrier omitted */','flow','empty'),
        ('second READY phase collapsed','s->phase!=GT_RESTORE_WAIT_L || sequence!=s->cut',
         'false || sequence!=s->cut','phases','empty'),
        ('READY before snapshot offered','!s->offered || sequence!=s->snapshot',
         'false || sequence!=s->snapshot','phases','empty'),
        ('future input accepted','sequence<=s->dequeued','true','flow','empty'),
        ('stale generation accepted','generation!=s->generation || s->phase==GT_RESTORE_DETACHED',
         'false || s->phase==GT_RESTORE_DETACHED','protocol','empty'),
        # 掉线只清空队列而不落相位，半恢复的一代就能继续取事件并走到 LIVE。
        ('detach leaves the phase behind','{clear(s);s->phase=GT_RESTORE_DETACHED;}','{clear(s);}','interrupt','flow'),
    )
    try:
        build(text)
        for mode in CASES:assert run(mode)==0,mode
        for name,before,after,target,control in variants:
            assert text.count(before)==1,name
            build(text.replace(before,after)+f'\n/* {MARK} */\n')
            assert run(target)!=0,name+': target falsely green'
            assert run(control)==0,name+': control failed'
            print(name+': target red, control green',flush=True)
        build(text)
        for mode in CASES:assert run(mode)==0,mode
    finally:
        source.write_bytes(original)
        assert hashlib.sha256(source.read_bytes()).digest()==sha
        assert hashlib.sha256(SOURCE.read_bytes()).digest()==sha
print('rollback SHA256 and all restore-stream baselines green')
