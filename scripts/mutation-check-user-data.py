#!/usr/bin/env python3
"""USER wire/schema and asynchronous data ownership; ordinary isolated failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/user-data-mutations'
RUNS=WORK/'runs'
MARK='GT_USER_DATA_MUTATION_MARK'
NAMES=('ipc/user-wire.c','ipc/user-wire.h','ipc/user-dispatcher.c','ipc/user-dispatcher.h','ipc/server.c','ipc/server.h','ipc/control.c','ipc/control.h')
PATHS={name:ROOT/'core/src'/name for name in NAMES}
PATHS['runtime-test.c']=ROOT/'core/tests/test_session_runtime.c'
PATHS['unit.c']=ROOT/'core/tests/test_user_wire.c'
MODES={'unit':('requests','malformed','replies','limits'),
       'runtime':('wire-flow','wire-disconnected','wire-pressure','wire-failure','wire-malformed','wire-config','wire-backpressure')}


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
    if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2
    def interrupted(signum,frame):raise RuntimeError('user-data mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    for path in copies.values():path.parent.mkdir(parents=True,exist_ok=True)
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('user-data SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('user-data dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build(suite):
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',
            '-D_DARWIN_C_SOURCE' if sys.platform=='darwin' else '-D_POSIX_C_SOURCE=200809L',
            '-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/ipc'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['unit.c' if suite=='unit' else 'runtime-test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/suite)])
        if rc:raise RuntimeError('user-data compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('user-data dependency archive changed')
    def probe(suite,mode,reject=False):
        rc,out=run([str(WORK/suite),mode]+([str(RUNS)] if suite=='runtime' else []))
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('user-data mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('user-data baseline/control failed/'+mode+': '+out)
    specs=(
        ('USER messages accepted on management role','ipc/control.c','(type>=GT_MSG_USER_INPUT && type<=GT_MSG_USER_RESULT) || ','','runtime','wire-malformed','unit','requests'),
        ('flags ignored','ipc/user-wire.c','!f->header.flags && ','','unit','malformed','unit','requests'),
        ('request ID ignored','ipc/user-wire.c','!f->header.request_id && ','','unit','malformed','unit','requests'),
        ('zero stamp field allowed','ipc/user-wire.c','s.generation && s.command_id','s.generation || s.command_id','unit','malformed','unit','requests'),
        ('wrong byte order','ipc/user-wire.c','(v<<8)|p[i]','(v<<8)|p[n-i-1]','unit','requests','runtime','wire-config'),
        ('resize pixels truncated','ipc/user-wire.c','(uint32_t)get(p+4,4)','(uint16_t)get(p+4,4)','unit','requests','unit','limits'),
        ('resize trailing bytes accepted','ipc/user-wire.c','if(n!=36)','if(n<36)','unit','malformed','unit','requests'),
        ('resize zero cells accepted','ipc/user-wire.c','!r.size.cols || !r.size.rows','false','unit','malformed','unit','requests'),
        ('invalid color accepted','ipc/user-wire.c','get(p,4)>1','false','unit','malformed','unit','requests'),
        ('INPUT length unbounded','ipc/user-wire.c','n>GT_USER_PREFIX_BYTES+GT_USER_MAX_INPUT','false','unit','malformed','unit','requests'),
        ('maximum INPUT rejected','ipc/user-wire.c','n>GT_USER_PREFIX_BYTES+GT_USER_MAX_INPUT','n>=GT_USER_PREFIX_BYTES+GT_USER_MAX_INPUT','unit','limits','unit','requests'),
        ('reply reserved word ignored','ipc/user-wire.c','if(get(p+36,4))','if(get(p+36,4) && false)','unit','replies','unit','requests'),
        ('reply status accepts unknown','ipc/user-wire.c','(unsigned)r->status<=GT_ERR_INTERNAL','true','unit','replies','unit','requests'),
        ('reply delivery contradiction allowed','ipc/user-wire.c','((r->status==GT_OK)==(r->delivery==GT_USER_OPERATION_OK))','true','unit','replies','unit','requests'),
        ('reject reason zero accepted','ipc/user-wire.c','r->rejection>=GT_USER_REJECT_BACKPRESSURE','(unsigned)r->rejection<=GT_USER_REJECT_UNAVAILABLE','unit','replies','unit','requests'),
        ('reject reason unknown accepted','ipc/user-wire.c','r->rejection<=GT_USER_REJECT_UNAVAILABLE','r->rejection<=GT_USER_REJECT_UNAVAILABLE+1','unit','replies','unit','requests'),
        ('worker completion stamp dropped','ipc/user-dispatcher.c','c->reply.stamp=result->stamp','c->reply.stamp=(gt_worker_stamp_t){0}','runtime','wire-flow','unit','requests'),
        ('attempted failure falsely not executed','ipc/user-dispatcher.c','c->reply.delivery=result->delivery','c->reply.delivery=result->status==GT_OK?GT_USER_OPERATION_OK:GT_USER_NOT_EXECUTED','runtime','wire-failure','runtime','wire-malformed'),
        ('pre-queue rejection presented as completion','ipc/user-dispatcher.c','c->reply.rejected=true','c->reply.rejected=false','runtime','wire-flow','runtime','wire-malformed'),
        ('memory rejection reason lost','ipc/user-dispatcher.c','case GT_WORKER_NO_MEMORY:return GT_USER_REJECT_NO_MEMORY','case GT_WORKER_NO_MEMORY:return GT_USER_REJECT_UNSUPPORTED','runtime','wire-failure','runtime','wire-flow'),
        ('worker backpressure reason lost','ipc/user-dispatcher.c','case GT_WORKER_BACKPRESSURE:return GT_USER_REJECT_BACKPRESSURE','case GT_WORKER_BACKPRESSURE:return GT_USER_REJECT_NO_MEMORY','runtime','wire-backpressure','runtime','wire-flow'),
        ('generation ignored in mailbox identity','ipc/user-dispatcher.c',' && a.generation==b.generation','','runtime','wire-disconnected','unit','requests'),
        ('server retirement hook omitted','ipc/server.c','if(s->config.data_drop)s->config.data_drop(s->config.data_context,&p->binding);','','runtime','wire-malformed','unit','requests'),
        ('retirement hook before revoke','ipc/server.c','s->config.handshake.binding_drop(s->config.handshake.binding_context,&p->binding);\n    if(s->config.data_drop)s->config.data_drop(s->config.data_context,&p->binding);','if(s->config.data_drop)s->config.data_drop(s->config.data_context,&p->binding);\n    s->config.handshake.binding_drop(s->config.handshake.binding_context,&p->binding);','runtime','wire-malformed','unit','requests'),
        ('orphan drop hook admitted','ipc/server.c','(config->data_drop && !config->data_step)','false','runtime','wire-config','unit','requests'),
        ('dispatcher allocation bypasses domain','ipc/user-dispatcher.c','gt_user_dispatcher_t *d=a.allocate(a.context,n)','gt_user_dispatcher_t *d=malloc(n)','runtime','wire-config','unit','requests'),
        ('dispatcher not erased before refund','ipc/user-dispatcher.c','gt__secure_erase(d,n);a.release(a.context,d,n);return GT_RUNTIME_OK;','a.release(a.context,d,n);return GT_RUNTIME_OK;','runtime','wire-malformed','unit','requests'),
        ('connection limit accepts zero','ipc/user-dispatcher.c',' || !config->connections','','runtime','wire-config','unit','requests'),
        ('mailbox limit accepts zero','ipc/user-dispatcher.c',' || !config->commands','','runtime','wire-config','unit','requests'),
        ('instance binding ignored','ipc/user-dispatcher.c','config->instance))','(const uint8_t[16]){1}))','runtime','wire-config','unit','requests'),
        ('completion error lost','ipc/user-dispatcher.c','c->reply.status=result->status','c->reply.status=GT_OK','runtime','wire-flow','unit','requests'),
        ('pending callback lease released early','ipc/user-dispatcher.c',' && !p->pending','','runtime','wire-disconnected','unit','requests'),
        ('mailbox capacity ignored','ipc/user-dispatcher.c','if(p->count==d->config.commands)','if(false && p->count==d->config.commands)','runtime','wire-pressure','unit','requests'),
        ('request generation not bound','ipc/user-dispatcher.c',' || request.stamp.generation!=scope->generation','','runtime','wire-malformed','unit','requests'),
        ('connection maximum ignored','ipc/user-dispatcher.c',' || config->connections>64','','runtime','wire-config','unit','requests'),
        ('mailbox maximum ignored','ipc/user-dispatcher.c',' || config->commands>16','','runtime','wire-config','unit','requests'),
        ('reply delivery accepts unknown','ipc/user-wire.c','(unsigned)r->delivery<=GT_USER_OUTCOME_UNKNOWN','true','unit','replies','unit','requests'),
    )

    try:
        restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        for spec in specs:
            label,name,old,new,suite,target,control_suite,control=spec[:8];expected=spec[8] if len(spec)>8 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('user-data mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True)
                if control_suite!=suite:build(control_suite)
                probe(control_suite,control)
                print('PASS user-data mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        print(f'PASS {len(specs)} user-data mutations, {sum(map(len,MODES.values()))} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
