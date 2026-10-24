#!/usr/bin/env python3
"""Complete DATA transaction, boundaries and context lifetime; ordinary isolated failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/data-router-mutations'
RUNS=WORK/'runs'
MARK='GT_DATA_ROUTER_MUTATION_MARK'
NAMES=('ipc/data-wire.c','ipc/data-wire.h','ipc/data-router.c','ipc/data-router.h','ipc/server.c','ipc/server.h','ipc/control.c','ipc/control.h','ipc/user-dispatcher.c','ipc/user-dispatcher.h','session/runtime.c','session/runtime.h','session/attachment.c','session/attachment.h')
PATHS={name:ROOT/'core/src'/name for name in NAMES}
PATHS['runtime-test.c']=ROOT/'core/tests/test_session_runtime.c'
PATHS['unit.c']=ROOT/'core/tests/test_data_wire.c'
MODES={'unit':('vectors','events','malformed','terminal','limits','exact'),
       'runtime':('route-flow','route-final','route-cancel','route-error','route-timeout','route-unavailable','route-crossing','route-config','route-malformed','route-clock','route-error-deadline','route-invalid-blob','route-capture-pending','route-capture-ending')}


def run(args,timeout=25):
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
    def interrupted(signum,frame):raise RuntimeError('data-router mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    for path in copies.values():path.parent.mkdir(parents=True,exist_ok=True)
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('data-router SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('data-router dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build(suite):
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',
            '-D_DARWIN_C_SOURCE' if sys.platform=='darwin' else '-D_POSIX_C_SOURCE=200809L',
            '-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/ipc'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['unit.c' if suite=='unit' else 'runtime-test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/suite)])
        if rc:raise RuntimeError('data-router compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('data-router dependency archive changed')
    def probe(suite,mode,reject=False):
        rc,out=run([str(WORK/suite),mode]+([str(RUNS)] if suite=='runtime' else []))
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('data-router mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('data-router baseline/control failed/'+mode+': '+out)
    specs=(
        ('snapshot numeric cap drifts','ipc/data-wire.h','GT_DATA_MAX_SNAPSHOT=64*1024*1024','GT_DATA_MAX_SNAPSHOT=64*1024*1024+1','unit','limits','unit','vectors'),
        ('chunk numeric cap drifts','ipc/data-wire.h','GT_DATA_MAX_CHUNK=65536','GT_DATA_MAX_CHUNK=65537','unit','limits','unit','vectors'),
        ('output numeric cap drifts','ipc/data-wire.h','GT_DATA_MAX_OUTPUT=16384','GT_DATA_MAX_OUTPUT=16385','unit','limits','unit','vectors'),
        ('management accepts DATA role messages','ipc/control.c','(type>=GT_MSG_SNAPSHOT_BEGIN && type<=GT_MSG_RESTORE_ERROR_ACK) || ','','runtime','route-config','unit','vectors'),
        ('wire flags ignored','ipc/data-wire.c','f->header.flags || ','','unit','malformed','unit','vectors'),
        ('wire request ID ignored','ipc/data-wire.c','f->header.request_id || ','','unit','malformed','unit','vectors'),
        ('zero generation allowed','ipc/data-wire.c',' || !m->generation','','unit','malformed','unit','vectors'),
        ('byte order reversed','ipc/data-wire.c','(v<<8)|p[i]','(v<<8)|p[n-i-1]','unit','vectors','runtime','route-config'),
        ('snapshot reserved bytes ignored','ipc/data-wire.c','n!=40 || get(p+32,8)','n!=40','unit','malformed','unit','events'),
        ('snapshot format zero accepted','ipc/data-wire.c',' && m->format','','unit','malformed','unit','events'),
        ('snapshot flags ignored','ipc/data-wire.c','!(m->flags&~GT_DATA_READONLY)','true','unit','malformed','unit','events'),
        ('event length ignored','ipc/data-wire.c','get(p+20,4)!=n-24','false','unit','malformed','unit','vectors'),
        ('resize pixel width truncated','ipc/data-wire.c',',(uint32_t)get(p+28,4),(uint32_t)get(p+32,4)}',',(uint16_t)get(p+28,4),(uint32_t)get(p+32,4)}','unit','events','unit','vectors'),
        ('maximum output rejected','ipc/data-wire.c','m->length<=GT_DATA_MAX_OUTPUT','m->length<GT_DATA_MAX_OUTPUT','unit','limits','unit','events'),
        ('snapshot chunk extent unchecked','ipc/data-wire.c',' && m->length<=GT_DATA_MAX_SNAPSHOT-m->offset','','unit','limits','unit','vectors'),
        ('snapshot maximum unchecked','ipc/data-wire.c',' && m->total<=GT_DATA_MAX_SNAPSHOT','','unit','limits','unit','events',2),
        ('no-snapshot status contradiction accepted','ipc/data-wire.c','(((m->flags&GT_DATA_NO_SNAPSHOT)!=0)==(m->error!=GT_OK))','true','unit','terminal','unit','events'),
        ('unknown sequence pretends to have state','ipc/data-wire.c','((m->flags&GT_DATA_NO_SNAPSHOT) || m->terminal.result.has_sequence)','true','unit','terminal','unit','events'),
        ('error status zero accepted','ipc/data-wire.c','m->error>GT_OK','m->error!=GT_ERR_INTERNAL','unit','malformed','unit','vectors'),
        ('error stage unknown accepted','ipc/data-wire.c','m->stage<=GT_DATA_ERROR_PROTOCOL','m->stage<=GT_DATA_ERROR_PROTOCOL+1','unit','malformed','unit','vectors'),
        ('exact task reaper steals other owner','session/attachment.c',' || (target && j->id!=target)',' || (target && false) ','unit','exact','unit','vectors'),
        ('capture sequence lost','ipc/data-router.c','p->sequence=capture.sequence','p->sequence=0','runtime','route-crossing','runtime','route-config'),
        ('captured source not released','ipc/data-router.c','if(p->owned_input)gt__runtime_release_capture','if(false && p->owned_input)gt__runtime_release_capture','runtime','route-final','unit','vectors'),
        ('partial encoder output ownership lost','ipc/data-router.c','*out=blob;','*out=NULL;','runtime','route-invalid-blob','unit','vectors'),
        ('encoder limit ignored','ipc/data-router.c',' || blob->length>d->config.snapshot_limit','','runtime','route-invalid-blob','unit','vectors'),
        ('empty encoder output accepted','ipc/data-router.c',' || !blob->length','','runtime','route-invalid-blob','unit','vectors'),
        ('pending capture context released early','ipc/data-router.c',' || busy_capture(d,p)','','runtime','route-capture-pending','unit','vectors'),
        ('pending encoder context released early','ipc/data-router.c',' && !p->task){gt_runtime_session_t',' ){gt_runtime_session_t','runtime','route-cancel','unit','vectors'),
        ('USER ownership not dropped','ipc/data-router.c','gt__user_dispatcher_drop(d->users,scope);','(void)scope;','runtime','route-flow','unit','vectors'),
        ('new generation finds old peer','ipc/data-router.c',' && a.generation==b.generation','','runtime','route-cancel','unit','vectors'),
        ('revoked error drain omitted','ipc/server.c','if(!s->config.data_revoked || ','if(true || ','runtime','route-error','runtime','route-config'),
        ('revoked hook needs no drop owner','ipc/server.c','(config->data_revoked && (!config->data_step || !config->data_drop))','false','runtime','route-config','unit','vectors'),
        ('restore deadline renewed by heartbeat','ipc/data-router.c','p->pong=m.nonce;','p->start=d->now;p->pong=m.nonce;','runtime','route-timeout','runtime','route-final'),
        ('error deadline renewed by inbound frames','ipc/data-router.c','if(!gt__connection_release_frame(c))return GT_SERVER_DATA_CLOSE;','p->error_start=d->now;if(!gt__connection_release_frame(c))return GT_SERVER_DATA_CLOSE;','runtime','route-error-deadline','unit','vectors',2),
        ('PONG nonce mismatched','ipc/data-router.c','m.nonce=p->pong;','m.nonce=p->pong+1;','runtime','route-flow','runtime','route-final'),
        ('snapshot END completion omitted','ipc/data-router.c','if(p->offset==p->total)','if(false && p->offset==p->total)','runtime','route-flow','runtime','route-config'),
        ('READY L never applied','ipc/data-router.c','!p->live_sent || gt__attachment_ready_live(gt__runtime_attachment(p->lease),scope->generation,m.sequence)!=GT_RESTORE_OK','!p->live_sent || (false && gt__attachment_ready_live(gt__runtime_attachment(p->lease),scope->generation,m.sequence)!=GT_RESTORE_OK)','runtime','route-flow','unit','vectors'),
        ('LIVE granted before READY L','ipc/data-router.c','if(live==GT_RESTORE_OK){m.type=GT_MSG_LIVE_BEGIN;','if(live==GT_RESTORE_OK){(void)gt__attachment_ready_live(a,p->scope.generation,cut);m.type=GT_MSG_LIVE_BEGIN;','runtime','route-flow','runtime','route-config'),
        ('terminal sent before final deltas','ipc/data-router.c','gt_restore_event_t event;gt_restore_rc_t next=','if(p->has_terminal && !p->terminal_sent){m.type=GT_MSG_SESSION_END;m.terminal=p->terminal;p->terminal_sent=true;return send(d,p,c,&m)?GT_SERVER_DATA_IDLE:GT_SERVER_DATA_CLOSE;}\n  gt_restore_event_t event;gt_restore_rc_t next=','runtime','route-final','runtime','route-config'),
        ('missing snapshot failure falsely OK','ipc/data-router.c','m.error=p->final_error;','m.error=GT_OK;','runtime','route-unavailable','runtime','route-config'),
        ('canceled capture cannot await terminal publication','ipc/data-router.c','if(p->capture_error==GT_ERR_CANCELLED && !p->task)','if(false && p->capture_error==GT_ERR_CANCELLED && !p->task)','runtime','route-capture-ending','runtime','route-config'),
        ('clock failure ignored at open','ipc/data-router.c','if(status==GT_OK && !tick(d))','if(false && status==GT_OK && !tick(d))','runtime','route-config','unit','vectors'),
        ('router allocation bypasses domain','ipc/data-router.c','gt_data_router_t *d=a.allocate(a.context,n)','gt_data_router_t *d=malloc(n)','runtime','route-config','unit','vectors'),
        ('zero chunk size admitted','ipc/data-router.c',' || !c->chunk_bytes','','runtime','route-config','unit','vectors'),
        ('snapshot budget maximum ignored','ipc/data-router.c',' || c->snapshot_limit>64*1024*1024','','runtime','route-config','unit','vectors'),
        ('connection maximum ignored','ipc/data-router.c',' || c->connections>64','','runtime','route-config','unit','vectors'),
        ('restore deadline zero admitted','ipc/data-router.c','!c->restore_timeout_ms || ','','runtime','route-config','unit','vectors'),
    )

    try:
        restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        for spec in specs:
            label,name,old,new,suite,target,control_suite,control=spec[:8];expected=spec[8] if len(spec)>8 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('data-router mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True)
                if control_suite!=suite:build(control_suite)
                probe(control_suite,control)
                print('PASS data-router mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        print(f'PASS {len(specs)} data-router mutations, {sum(map(len,MODES.values()))} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
