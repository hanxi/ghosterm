#!/usr/bin/env python3
"""Shared server: real private endpoints/PTYs, isolated faults and rollback."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT/'tmp/ipc-server-mutations'
RUNS = WORK/'runs'
MARK = 'GT_IPC_SERVER_MUTATION_MARK'
NAMES = ('server.c','server.h','control.c','control.h','control-info.c')
PATHS = {name: ROOT/'core/src/ipc'/name for name in NAMES}
PATHS['test'] = ROOT/'core/tests/test_ipc_server.c'
MODES = ('management','unsupported','request-id','malformed','capacity','idle',
         'handshake-timeout','clock','allocation','issuer','expiry','data','stale',
         'codec','fairness','shutdown','kill','invalid','missing','ended',
         'data-disabled','data-lease','data-failure','full-list')


def run(args, timeout=20):
    process = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=timeout)
    except BaseException:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        raise
    return process.returncode, output


def main():
    originals = {name: path.read_bytes() for name, path in PATHS.items()}
    hashes = {name: hashlib.sha256(data).digest() for name, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()) or any(
            MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2

    def interrupted(signum, frame):
        raise RuntimeError('shared server mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    (WORK/'ipc').mkdir(parents=True, exist_ok=True);RUNS.mkdir(exist_ok=True, mode=0o700)
    copies = {name: WORK/'ipc'/name for name in NAMES};copies['test'] = WORK/'test.c'

    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('server SHA256 rollback failed/'+name)

    def clean_runs():
        shutil.rmtree(RUNS);RUNS.mkdir(mode=0o700)

    # Existing worker/manager/PTY dependencies come from the independently built
    # root archive. Mutated server/control definitions shadow those archive
    # members; never patch the archive or root sources. Verify archive unchanged.
    rc, output = run(['bash',str(ROOT/'scripts/build-core.sh')], 120)
    if rc:
        raise RuntimeError('server dependency build failed: '+output)
    archive = ROOT/'core/build/libghosterm_core.a'
    archive_hash = hashlib.sha256(archive.read_bytes()).digest()

    def build():
        rc, output = run([*shlex.split(os.environ.get('CC','cc')), '-std=c11','-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/ipc'),'-I',str(ROOT/'core/include'),
            str(copies['server.c']),str(copies['control.c']),str(copies['control-info.c']),str(copies['test']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/'probe')])
        if rc:
            raise RuntimeError('server compilation failed: '+output)
        if hashlib.sha256(archive.read_bytes()).digest() != archive_hash:
            raise RuntimeError('server dependency archive changed during isolated mutations')

    def probe(mode, reject=False):
        rc, output = run([str(WORK/'probe'),mode,str(RUNS)])
        if reject:
            if rc != 1 or 'FAIL ' not in output:
                raise RuntimeError('mutation lacks ordinary targeted assertion/'+mode+': '+output)
        elif rc:
            raise RuntimeError('server baseline/control failed/'+mode+': '+output)

    specs = (
        ('disconnect stops session','server.c','uint64_t issuer=p->id;p->id=0;','(void)gt__manager_terminate(s->config.manager,1);uint64_t issuer=p->id;p->id=0;','management','codec'),
        ('service close stops borrowed manager','server.c','s->state=GT_SERVER_CLOSED;','(void)gt__manager_terminate(s->config.manager,1);s->state=GT_SERVER_CLOSED;','shutdown','codec'),
        ('issuer cancellation omitted','server.c','if(s->config.handshake.tokens)gt__tokens_revoke_issuer(s->config.handshake.tokens,issuer);','(void)issuer;','issuer','unsupported'),
        ('idle token expiry omitted','server.c','(s->config.handshake.tokens && gt__tokens_expire(s->config.handshake.tokens,now)!=GT_TOKEN_OK)','false','expiry','unsupported'),
        ('consumed data generation leaked','server.c','s->config.handshake.binding_drop(s->config.handshake.binding_context,&p->binding);','(void)p->binding;','data','unsupported'),
        ('stale data owner kept','server.c','if(!s->config.handshake.binding_current(s->config.handshake.binding_context,&p->binding))','if(false && !s->config.handshake.binding_current(s->config.handshake.binding_context,&p->binding))','stale','unsupported'),
        ('data dispatcher failure ignored','server.c','else if(rc!=GT_SERVER_DATA_IDLE)retire(s,p);','else if(false && rc!=GT_SERVER_DATA_IDLE)retire(s,p);','data-failure','unsupported'),
        ('data output renews lease','server.c','if(rc==GT_SERVER_DATA_PROGRESS)p->last_activity=s->now;','if(rc==GT_SERVER_DATA_PROGRESS || rc==GT_SERVER_DATA_IDLE)p->last_activity=s->now;','data-lease','unsupported'),
        ('idle boundary missed','server.c','s->now-p->last_activity>=s->config.idle_timeout_ms','s->now-p->last_activity>s->config.idle_timeout_ms','idle','codec'),
        ('polling renews idle connection','server.c','if(s->now-p->last_activity>=','p->last_activity=s->now;if(s->now-p->last_activity>=','idle','codec'),
        ('request ID replay accepted','server.c','frame.header.request_id<=p->last_request','frame.header.request_id<p->last_request','request-id','codec'),
        ('oversized terminate accepted','control.c','if(n!=8 || !(id=get64(request->payload)))','if(n<8 || !(id=get64(request->payload)))','malformed','codec'),
        ('zero session accepted','control.c','if(n!=8 || !(id=get64(request->payload)))','if(n!=8 || ((id=get64(request->payload)),false))','malformed','codec'),
        ('terminate falsely succeeds','control.c','status(gt__manager_terminate(manager,id))','status(GT_MANAGER_OK)','management','codec'),
        ('reply loses request ID','control.c','.request_id=request->header.request_id','.request_id=0','management','codec'),
        ('missing session becomes success','control.c','case GT_MANAGER_NOT_FOUND:return GT_CONTROL_NOT_FOUND;','case GT_MANAGER_NOT_FOUND:return GT_CONTROL_OK;','missing','codec'),
        ('unknown operation falsely succeeds','control.c','put32(bytes,GT_CONTROL_UNSUPPORTED);','put32(bytes,GT_CONTROL_OK);','unsupported','codec'),
        ('list rows silently truncated','control.c','put32(bytes+4,(uint32_t)count);length+=(uint32_t)count*GT_CONTROL_INFO_BYTES;','put32(bytes+4,0);length=8;','management','codec'),
        ('unknown exit presented as zero','control-info.c','if(v->has_exit_code){flags|=8u;','if(true){flags|=8u;','codec','missing'),
        ('unknown exit availability lost','control-info.c','put32(out+28,(uint32_t)v->termination_status);','put32(out+28,GT_OK);','codec','missing'),
        ('invalid result row accepted','control-info.c','if(!valid_info(&r))return false;','if(false && !valid_info(&r))return false;','codec','missing'),
        ('reserved row bytes ignored','control-info.c','for(unsigned i=52;i<64;i++)if(p[i])return false;','for(unsigned i=52;i<52;i++)if(p[i])return false;','codec','missing'),
        ('extra row bytes accepted','control-info.c','n!=GT_CONTROL_INFO_BYTES','n<GT_CONTROL_INFO_BYTES','codec','missing'),
        ('reply capacity checked after mutation','control.c','if(!bytes || capacity<GT_CONTROL_REPLY_BYTES)return GT_CONTROL_NO_SPACE;','if(!bytes || !capacity)return GT_CONTROL_NO_SPACE;','invalid','codec'),
        ('server allocator bypassed','server.c','gt_server_t *s=a.allocate(a.context,bytes);','gt_server_t *s=malloc(bytes);','invalid','codec'),
        ('connection allocation uncharged','server.c','gt__connection_open(ops,s->config.limits,&s->allocator,','gt__connection_open(ops,s->config.limits,NULL,','allocation','codec'),
        ('handshake allocation uncharged','server.c','gt__handshake_open(p->connection,&handshake,&s->allocator,','gt__handshake_open(p->connection,&handshake,NULL,','allocation','codec'),
        ('failed handshake leaves socket open','server.c','gt__connection_destroy(p->connection);p->connection=NULL;\n    return status==','p->connection=NULL;\n    return status==','allocation','codec'),
        ('issuer ID reused','server.c','s->next_id=s->next_id==UINT64_MAX?0:s->next_id+1;','s->next_id=s->next_id==UINT64_MAX?0:s->next_id;','capacity','codec'),
        ('global clock rollback ignored','server.c',' || now<s->now','', 'clock','codec'),
        ('global clock failure not latched','server.c','s->state=GT_SERVER_CLOCK_FAILED;','s->state=GT_SERVER_RUNNING;','clock','codec'),
        ('failed service accepts new peer','server.c','else if(s->state!=GT_SERVER_RUNNING)rejection=GT_SERVER_ADMIT_CLOSED;','else if(false)rejection=GT_SERVER_ADMIT_CLOSED;', 'clock','codec'),
        ('backpressure blocks other peers','server.c','peer *p=&s->peers[(s->cursor+i)%s->config.connections];if(p->id)step_peer(s,p);','peer *p=&s->peers[(s->cursor+i)%s->config.connections];if(p->id){step_peer(s,p);if(gt__connection_wants_write(p->connection)){s->cursor=(s->cursor+i)%s->config.connections;return s->state;}}','fairness','codec'),
        ('data accepted without dispatcher','server.c','if(!s->config.data_step)handshake.tokens=NULL;','if(false)handshake.tokens=NULL;','data-disabled','codec'),
        ('connection cap exceeded','server.c','config->connections>64','config->connections>65','invalid','codec'),
        ('list capacity too small','control.h','GT_CONTROL_INFO_BYTES=64,GT_CONTROL_MAX_ROWS=64','GT_CONTROL_INFO_BYTES=64,GT_CONTROL_MAX_ROWS=63','full-list','codec'),
    )
    try:
        restore();build()
        for mode in MODES:
            probe(mode)
        for label,name,old,new,target,control in specs:
            source=originals[name].decode()
            if source.count(old)!=1:
                raise RuntimeError('server mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build();probe(target,True);probe(control)
                print('PASS server mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:
                restore();clean_runs()
        initial=originals['server.c'].decode().replace('s->next_id=1;', 's->next_id=UINT64_MAX;')
        copies['server.c'].write_text(initial);build();probe('exhaustion')
        copies['server.c'].write_text(initial.replace('s->next_id==UINT64_MAX?0:', 's->next_id==UINT64_MAX?1:')+'\n/* '+MARK+' */\n')
        build();probe('exhaustion',True);probe('codec');restore();clean_runs()
        build()
        for mode in MODES:
            probe(mode)
        print(f'PASS {len(specs)+1} server mutations, {len(MODES)} baselines, issuer exhaustion and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':
    raise SystemExit(main())
