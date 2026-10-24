#!/usr/bin/env python3
"""CREATE publication/retention/ownership and real IPC reply-loss proof."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/creation-mutations'
RUNS=WORK/'runs'
MARK='GT_CREATION_MUTATION_MARK'
NAMES=('session/creation.c','session/creation.h','session/create-spec.c','session/create-spec.h',
       'session/manager.c','session/manager.h','ipc/server.c','ipc/server.h','ipc/control.c','ipc/control.h','ipc/control-reply-wire.c','io/pty-size.h')
PATHS={name:ROOT/'core/src'/name for name in NAMES}
PATHS['unit.c']=ROOT/'core/tests/test_creation.c'
PATHS['server-test.c']=ROOT/'core/tests/test_ipc_server.c'
MODES=('lifecycle','replay','expiry','failures','asynchronous','clock','allocation','codec',
       'invalid','launch-failures','limits','unsupported','registration')
WIRE=('create','create-lost','create-malformed','create-expiry','create-principal','create-disabled','create-config','create-codec')


def run(args,timeout=20):
    p=subprocess.Popen(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:
        output,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:
            try:os.killpg(p.pid,signal.SIGKILL)
            except ProcessLookupError:pass
        p.wait();raise
    return p.returncode,output


def main():
    originals={name:path.read_bytes() for name,path in PATHS.items()}
    hashes={name:hashlib.sha256(data).digest() for name,data in originals.items()}
    if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2
    def interrupted(signum,frame):raise RuntimeError('creation mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077)
    copies={name:WORK/name for name in PATHS}
    for path in copies.values():path.parent.mkdir(parents=True,exist_ok=True)
    RUNS.mkdir(exist_ok=True,mode=0o700)
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('creation SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('creation dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build(suite):
        source=copies['unit.c' if suite=='unit' else 'server-test.c']
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/ipc'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(source),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/suite)])
        if rc:raise RuntimeError('creation compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('creation dependency archive changed')
    def probe(suite,mode,reject=False):
        rc,out=run([str(WORK/suite),mode,str(RUNS)])
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('creation baseline/control failed/'+mode+': '+out)
    # label, suite, source, old, new, target, same-mutant independent control
    specs=(
        ('CREATE reply accepts failed success detail','wire','ipc/control-reply-wire.c',' || detail!=GT_OK','','create-codec','create-disabled'),
        ('CREATE reply accepts zero nonce','wire','ipc/control-reply-wire.c','!nonzero(p,16) || ','','create-codec','create-disabled'),
        ('CREATE reply accepts extra bytes','wire','ipc/control-reply-wire.c','n!=GT_CREATE_REPLY_BYTES','n<GT_CREATE_REPLY_BYTES','create-codec','create-disabled'),
        ('CREATE reply claims success without session','wire','ipc/control-reply-wire.c','(!id || detail!=GT_OK)','(detail!=GT_OK)','create-codec','create-disabled'),
        ('CREATE unknown reply leaks session','wire','ipc/control-reply-wire.c',':id!=0)',':false)','create-codec','create-disabled'),
        ('CREATE reply unknown status accepted','wire','ipc/control-reply-wire.c','state>GT_CREATION_INTERNAL || ','','create-codec','create-disabled'),
        ('nonce lookup ignored','unit','session/creation.c','if(r)return r->length==n && !memcmp(r->payload,bytes,n)?r->result:result(GT_CREATION_CONFLICT,GT_OK);','if(false && r)return r->length==n && !memcmp(r->payload,bytes,n)?r->result:result(GT_CREATION_CONFLICT,GT_OK);','replay','codec'),
        ('principal scope ignored','unit','session/creation.c',' && c->records[i].principal==principal',' && principal','replay','codec'),
        ('nonce conflict ignored','unit','session/creation.c','r->length==n && !memcmp(r->payload,bytes,n)?r->result','true?r->result','replay','codec'),
        ('expiry boundary missed','unit','session/creation.c','now-c->records[i].issued>=c->config.retention_ms','now-c->records[i].issued>c->config.retention_ms','expiry','codec'),
        ('expired request not erased','wire','session/creation.c','gt__secure_erase(&c->records[i],sizeof(record));','c->records[i].active=false;','create-expiry','create-disabled'),
        ('idle service does not expire ledger','wire','ipc/server.c','(s->config.creation && gt__creation_advance(s->config.creation,now)!=GT_OK)','false','create-expiry','create-disabled'),
        ('ledger close stops committed session','unit','session/creation.c','gt__secure_erase(c,bytes);a.release(a.context,c,bytes);','(void)gt__manager_terminate(c->config.manager,1);gt__secure_erase(c,bytes);a.release(a.context,c,bytes);','lifecycle','codec'),
        ('clock rollback not permanent','unit','session/creation.c','c->failed=true;','c->failed=false;','clock','codec'),
        ('job refund charge wrong','unit','session/creation.c','a.release(a.context,j,sizeof(*j));','a.release(a.context,j,sizeof(*j)-1);','lifecycle','codec',2),
        ('ledger allocation bypassed','unit','session/creation.c','gt_creation_t *c=a.allocate(a.context,bytes);','gt_creation_t *c=malloc(bytes);','allocation','codec'),
        ('job allocation bypassed','unit','session/creation.c','launch *j=c->allocator.allocate(c->allocator.context,sizeof(*j));','launch *j=malloc(sizeof(*j));','allocation','codec'),
        ('original context not disposed','unit','session/creation.c','j->dispose(j->factory_context,j->original.context);','(void)j->dispose;','failures','codec'),
        ('prepare rejection ignored','unit','session/creation.c','if(rc!=GT_OK) {','if(false && rc!=GT_OK) {','failures','codec'),
        ('worker start rejection falsely accepted','unit','session/creation.c','if(status==GT_MANAGER_OK)return','if(status==GT_MANAGER_OK || status==GT_MANAGER_START_FAILED)return','launch-failures','codec'),
        ('output callback lost','unit','session/creation.c','wrapped.output=j->original.output?output:NULL;','(void)output;wrapped.output=NULL;','lifecycle','codec'),
        ('event callback lost','unit','session/creation.c','wrapped.event=j->original.event?event:NULL;','(void)event;wrapped.event=NULL;','lifecycle','codec'),
        ('finished callback lost','unit','session/creation.c','wrapped.finished=j->original.finished?finished:NULL;','(void)finished;wrapped.finished=NULL;','lifecycle','codec'),
        ('job strings cleared before worker uses them','unit','session/creation.c','uint64_t id=0;gt_manager_rc_t status=','gt__secure_erase(j->text,sizeof(j->text));uint64_t id=0;gt_manager_rc_t status=','lifecycle','codec'),
        ('malformed request consumes capacity','unit','session/creation.c','if(decoded!=GT_OK && decoded!=GT_ERR_UNSUPPORTED)return','if(false && decoded!=GT_OK && decoded!=GT_ERR_UNSUPPORTED)return','invalid','codec'),
        ('unsupported policy runs factory','unit','session/creation.c','decoded==GT_ERR_UNSUPPORTED?result(GT_CREATION_UNSUPPORTED,GT_ERR_UNSUPPORTED):start(c,principal,bytes,n)','start(c,principal,bytes,n)','unsupported','codec'),
        ('runtime binding optional','unit','session/creation.c',' || !config->bind_id','','invalid','codec'),
        ('prepare loses trusted principal','unit','session/creation.c','prepare(c->config.factory_context,principal,','prepare(c->config.factory_context,1,','registration','codec'),
        ('binding loses trusted principal','unit','session/creation.c','j->original.context,j->principal,id','j->original.context,1,id','registration','codec'),
        ('binding receives unreserved identity','unit','session/manager.c','register_id(config->context,e->id)','register_id(config->context,0)','registration','codec'),
        ('binding rejection ignored','unit','session/manager.c','register_id(config->context,e->id)!=GT_OK','(register_id(config->context,e->id),false)','registration','codec'),
        ('binding rejection retains slot','unit','session/manager.c','pthread_mutex_lock(&m->lock);memset(e,0,sizeof(*e));pthread_mutex_unlock(&m->lock);return GT_MANAGER_REGISTER_FAILED;','pthread_mutex_lock(&m->lock);pthread_mutex_unlock(&m->lock);return GT_MANAGER_REGISTER_FAILED;','registration','codec'),
        ('binding rejection reuses identity','unit','session/manager.c','pthread_mutex_lock(&m->lock);memset(e,0,sizeof(*e));pthread_mutex_unlock(&m->lock);return GT_MANAGER_REGISTER_FAILED;','pthread_mutex_lock(&m->lock);m->next_id=e->id;memset(e,0,sizeof(*e));pthread_mutex_unlock(&m->lock);return GT_MANAGER_REGISTER_FAILED;','registration','codec'),
        ('binding runs under registry mutex','unit','session/manager.c','pthread_mutex_unlock(&m->lock);\n  if(register_id','if(register_id','registration','codec'),
        ('binding rejection loses reason','unit','session/creation.c','gt_status_t reason=j->binding_status;','gt_status_t reason=GT_ERR_INTERNAL;','registration','codec'),
        ('NUL in text accepted','unit','session/create-spec.c','if(c && c<0x80)continue;','if(c<0x80)continue;','invalid','limits'),
        ('reserved creation bytes ignored','unit','session/create-spec.c',' || get32(p+44)','', 'codec','limits'),
        ('extra payload ignored','unit','session/create-spec.c','if(remaining || !environment(out))','if(!environment(out))','codec','limits'),
        ('oversize payload accepted','unit','session/create-spec.c',' || n>GT_CREATE_MAX_BYTES','', 'limits','lifecycle'),
        ('argument count overflow accepted','unit','session/create-spec.c','out->argc>GT_CREATE_MAX_ARGS','out->argc>GT_CREATE_MAX_ARGS+1','limits','lifecycle'),
        ('duplicate environment keys accepted','unit','session/create-spec.c','if(eq && (size_t)(eq-s->env[j])==key && !memcmp(s->env[j],p,key))','if(false && eq && (size_t)(eq-s->env[j])==key && !memcmp(s->env[j],p,key))','codec','limits'),
        ('TERM environment collision accepted','unit','session/create-spec.c'," || (key==4 && !memcmp(p,\"TERM\",4))",'', 'codec','limits'),
        ('surrogate UTF8 accepted','unit','session/create-spec.c','if(c==0xed)hi=0x9f;','if(c==0xed)hi=0xbf;','codec','limits'),
        ('pixel size silently truncated','unit','io/pty-size.h','width>UINT16_MAX || height>UINT16_MAX','(width || height) && false', 'codec','limits'),
        ('caller principal replaced with constant','wire','ipc/server.c','.principal=p->principal,.issuer=p->id','.principal=1,.issuer=p->id','create-principal','create-disabled'),
        ('creation manager binding ignored','wire','session/creation.c',' && c->config.manager==manager','', 'create-config','create-disabled'),
        ('creation instance binding ignored','wire','session/creation.c',' && !memcmp(c->config.instance,instance,16)','', 'create-config','create-disabled'),
        ('CREATE too-small RX allowed','wire','ipc/server.c','config->limits.receive_limit<GT_CREATE_MAX_BYTES || ','', 'create-config','create-disabled'),
        ('lost reply query discards published nonce','wire','ipc/control.c','gt__creation_query(creation,principal,request->payload,now)','(gt_creation_result_t){GT_CREATION_UNKNOWN,GT_OK,0}','create-lost','create-disabled'),
    )
    try:
        restore()
        for suite,modes in (('unit',MODES),('wire',WIRE)):
            build(suite)
            for mode in modes:probe(suite,mode)
        for spec in specs:
            label,suite,name,old,new,target,control=spec[:7];expected=spec[7] if len(spec)>7 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('creation mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True);probe(suite,control)
                print('PASS creation mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:
                restore();shutil.rmtree(RUNS);RUNS.mkdir(mode=0o700)
        for suite,modes in (('unit',MODES),('wire',WIRE)):
            build(suite)
            for mode in modes:probe(suite,mode)
        print(f'PASS {len(specs)} creation mutations, {len(MODES)+len(WIRE)} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
