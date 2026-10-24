#!/usr/bin/env python3
"""Shared host runtime: actual PTYs, owned leases and isolated ordinary failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/session-runtime-mutations'
RUNS=WORK/'runs'
MARK='GT_SESSION_RUNTIME_MUTATION_MARK'
NAMES=('runtime.c','runtime.h','attachment.c','attachment.h')
PATHS={name:ROOT/'core/src/session'/name for name in NAMES}
PATHS['test.c']=ROOT/'core/tests/test_session_runtime.c'
MODES=('lifecycle','stream','pending','limits','failures','allocation','concurrency','invalid','service')


def run(args,timeout=20):
    p=subprocess.Popen(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:
        out,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:
            try:os.killpg(p.pid,signal.SIGKILL)
            except ProcessLookupError:pass
        p.wait();raise
    return p.returncode,out


def main():
    originals={name:path.read_bytes() for name,path in PATHS.items()}
    hashes={name:hashlib.sha256(data).digest() for name,data in originals.items()}
    if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.glob('*.[ch]')):
        return 2
    def interrupted(signum,frame):raise RuntimeError('runtime mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('runtime SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('runtime dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build():
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            str(copies['runtime.c']),str(copies['attachment.c']),str(copies['test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/'probe')])
        if rc:raise RuntimeError('runtime compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('runtime dependency archive changed')
    def probe(mode,reject=False):
        rc,out=run([str(WORK/'probe'),mode,str(RUNS)])
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('runtime mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('runtime baseline/control failed/'+mode+': '+out)
    specs=(
        ('runtime bypasses domain','runtime.c','r=a.allocate(a.context,bytes);','r=malloc(bytes);','allocation','invalid'),
        ('runtime charge refunded incorrectly','runtime.c','a.release(a.context,r,bytes);return GT_RUNTIME_OK;','a.release(a.context,r,bytes-1);return GT_RUNTIME_OK;','lifecycle','invalid'),
        ('runtime table leaked','runtime.c','a.release(a.context,r,bytes);return GT_RUNTIME_OK;','(void)a;return GT_RUNTIME_OK;','lifecycle','invalid'),
        ('session charge refunded incorrectly','runtime.c','a.release(a.context,c,sizeof(*c));','a.release(a.context,c,sizeof(*c)-1);','lifecycle','invalid'),
        ('prepared identity replaced','runtime.c','c->principal=principal;','(void)principal;c->principal=1;','lifecycle','invalid'),
        ('wrong principal can acquire','runtime.c',' || c->principal!=principal?','?','lifecycle','invalid'),
        ('lease not retained','runtime.c','c->references++;','(void)c->references;','limits','invalid'),
        ('lease never released','runtime.c','c->references--;','(void)c->references;','limits','invalid'),
        ('reference capacity ignored','runtime.c','c->references==256','c->references==257','limits','invalid'),
        ('join drops object lifetime','runtime.c','bool retain=c->started;','bool retain=false;','pending','invalid'),
        ('joined object never eligible for forget','runtime.c','c->joined=true;','c->joined=false;','lifecycle','invalid'),
        ('forget destroys borrowed object','runtime.c',' && !c->references) {',') {','failures','invalid'),
        ('shutdown ignores references','runtime.c','r->sessions[i]->references || ','','limits','invalid'),
        ('shutdown ignores active controller','runtime.c','!gt__attachment_quiescent(r->sessions[i]->attachment)','false','limits','invalid'),
        ('authority events not routed','runtime.c','(void)gt__attachment_push(c->attachment,sequence,event->kind,event->payload,event->length);','(void)c;(void)sequence;(void)event;','stream','invalid'),
        ('authority sequence corrupted','runtime.c','c->attachment,sequence,event->kind','c->attachment,sequence+1,event->kind','stream','invalid'),
        ('state invalidation callback lost','runtime.c','.state_invalidated=r->config.state.invalidated,','.state_invalidated=NULL,','failures','invalid'),
        ('color capability silently disabled','runtime.c','.color_scheme_supported=r->config.state.color_scheme_supported,','.color_scheme_supported=false,','stream','invalid'),
        ('cwd not passed to PTY','runtime.c','if(rc!=GT_OK)return rc;\n  c->attachment=','if(rc!=GT_OK)return rc;\n  c->options.cwd=NULL;\n  c->attachment=','lifecycle','invalid'),
        ('session bound to wrong manager ID','runtime.c','c->id=id;r->sessions[i]=c;','c->id=id+1;r->sessions[i]=c;','service','invalid'),
        ('missing freeze callback accepted','runtime.c',' || !config->state.ops.freeze','','invalid','lifecycle'),
        ('backend thread settings ignored','runtime.c','!gt__thread_options_valid(explicit_threads(config->backend_threads)?&config->backend_threads:NULL)','false','invalid','lifecycle'),
        ('queue event limit ignored','runtime.c',' || config->queue_events>4096','','invalid','lifecycle'),
        ('queue byte limit ignored','runtime.c',' || config->queue_bytes>16*1024*1024','','invalid','lifecycle'),
        ('active controller called quiescent','attachment.c','bool busy=a->active;','bool busy=false;','limits','invalid'),
    )
    try:
        restore();build()
        for mode in MODES:probe(mode)
        for label,name,old,new,target,control in specs:
            source=originals[name].decode()
            if source.count(old)!=1:raise RuntimeError('runtime mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build();probe(target,True);probe(control)
                print('PASS runtime mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        build()
        for mode in MODES:probe(mode)
        print(f'PASS {len(specs)} runtime mutations, {len(MODES)} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
