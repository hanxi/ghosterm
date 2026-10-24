#!/usr/bin/env python3
"""ATTACH ownership, common control/handshake and isolated ordinary failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/session-attach-mutations'
RUNS=WORK/'runs'
MARK='GT_SESSION_ATTACH_MUTATION_MARK'
NAMES=('attachment-owner.c','attachment-owner.h','server.c','server.h','control.c','control.h','control-reply-wire.c')
PATHS={name:ROOT/'core/src/ipc'/name for name in NAMES}
PATHS['test.c']=ROOT/'core/tests/test_session_runtime.c'
MODES=tuple('attach-'+name for name in ('flow','expiry','issuer','stale','failures','capacity','malformed','codec','config','close','lost','lease'))


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
    def interrupted(signum,frame):raise RuntimeError('attach mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('attach SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('attach dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build():
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/ipc'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/'probe')])
        if rc:raise RuntimeError('attach compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('attach dependency archive changed')
    def probe(mode,reject=False):
        rc,out=run([str(WORK/'probe'),mode,str(RUNS)])
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('attach mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('attach baseline/control failed/'+mode+': '+out)
    specs=(
        ('scope principal ignored','attachment-owner.c','a.principal==b.principal && ','','attach-flow','attach-codec'),
        ('scope issuer ignored','attachment-owner.c',' && a.issuer==b.issuer','','attach-flow','attach-codec'),
        ('scope session ignored','attachment-owner.c',' && a.session==b.session','','attach-flow','attach-codec'),
        ('scope generation ignored','attachment-owner.c',' && a.generation==b.generation','','attach-flow','attach-codec'),
        ('drop ignored','attachment-owner.c','if(!r)return;','if(r)return;if(!r)return;','attach-issuer','attach-codec'),
        ('reference not released','attachment-owner.c','gt__runtime_release(&r->lease);','r->lease=NULL;','attach-flow','attach-codec'),
        ('old callback cancels next generation','attachment-owner.c','r->lease),r->scope.generation)','r->lease),r->scope.generation+1)','attach-stale','attach-codec'),
        ('token store leaked','attachment-owner.c','gt__tokens_close(o->tokens);','(void)o->tokens;','attach-flow','attach-codec'),
        ('consumed scopes not dropped on close','attachment-owner.c','if(o->slots[i].lease)gt__attachment_owner_drop','if(false && o->slots[i].lease)gt__attachment_owner_drop','attach-close','attach-codec'),
        ('owner memory not erased','attachment-owner.c','gt__secure_erase(o,bytes);a.release','a.release','attach-flow','attach-codec',2),
        ('server scratch retains token','server.c','gt__secure_erase(s->reply,sizeof(s->reply));','(void)s->reply;','attach-flow','attach-codec'),
        ('exact-boundary advance omitted','attachment-owner.c','if(gt__tokens_expire(o->tokens,now)!=GT_TOKEN_OK)','if(false)','attach-expiry','attach-codec'),
        ('issuance failure keeps reservation','attachment-owner.c','gt__attachment_owner_drop(o,&slot->scope);','(void)slot;','attach-failures','attach-codec'),
        ('reservation overwrites occupied slot','attachment-owner.c','if(!o->slots[i].lease){slot=&o->slots[i];break;}','if(i==0){slot=&o->slots[i];break;}','attach-capacity','attach-codec'),
        ('runtime identity replaced','attachment-owner.c','o->config.runtime,principal,id,&lease','o->config.runtime,73,id,&lease','attach-issuer','attach-codec'),
        ('untrusted issuer substituted','server.c','.principal=p->principal,.issuer=p->id','.principal=p->principal,.issuer=1','attach-issuer','attach-codec'),
        ('no dispatcher still reserves','server.c','s->config.data_step?s->config.attachments:NULL','s->config.attachments','attach-config','attach-codec'),
        ('creation binding mismatch accepted','attachment-owner.c',' && creation==gt__runtime_creation(o->config.runtime)',' && ((void)creation,true)','attach-config','attach-codec'),
        ('manager binding mismatch accepted','attachment-owner.c','manager==gt__runtime_manager(o->config.runtime)','manager!=NULL','attach-config','attach-codec'),
        ('owner can be shared by live servers','attachment-owner.c',' || o->server','','attach-config','attach-codec'),
        ('wrong server releases claim','attachment-owner.c',' && o->server==server','','attach-config','attach-codec'),
        ('claim accepts old reservations','attachment-owner.c','if(o->slots[i].lease)return false;','if(false && o->slots[i].lease)return false;','attach-close','attach-codec'),
        ('failed constructor keeps claim','server.c','gt__attachment_owner_unclaim(config->attachments,s);','(void)config->attachments;','attach-config','attach-codec'),
        ('server close keeps claim','server.c','gt__attachment_owner_unclaim(s->config.attachments,s);','(void)s->config.attachments;','attach-close','attach-codec'),
        ('attach reply accepts extra bytes','control-reply-wire.c','n!=GT_ATTACH_REPLY_BYTES','n<GT_ATTACH_REPLY_BYTES','attach-codec','attach-capacity'),
        ('attach reserved bytes ignored','control-reply-wire.c','if(get32(p+44))return false;','','attach-codec','attach-capacity'),
        ('zero generation accepted','control-reply-wire.c',' && r->generation &&',' &&','attach-codec','attach-capacity'),
        ('zero ttl accepted','control-reply-wire.c',' && r->ttl_ms &&',' &&','attach-codec','attach-capacity'),
        ('ttl overflow accepted','control-reply-wire.c',' && r->ttl_ms<=60000','','attach-codec','attach-capacity'),
        ('zero counter accepted','control-reply-wire.c','nonzero(r->token.bytes,8) && ','','attach-codec','attach-capacity'),
        ('zero secret accepted','control-reply-wire.c',' && nonzero(r->token.bytes+8,32)','','attach-codec','attach-capacity'),
        ('zero instance accepted','control-reply-wire.c',' && nonzero(r->instance,16)','','attach-codec','attach-capacity'),
        ('failed reply carries token','control-reply-wire.c',' && !nonzero(r->token.bytes,40)','','attach-codec','attach-capacity'),
        ('unknown attach status accepted','control-reply-wire.c',' || (unsigned)r->status>GT_ATTACH_INTERNAL','','attach-codec','attach-capacity'),
        ('unknown error accepted','control-reply-wire.c',' || (unsigned)r->detail>GT_ERR_INTERNAL','','attach-codec','attach-capacity'),
        ('success has error detail','control-reply-wire.c','r->detail==GT_OK && ','','attach-codec','attach-capacity'),
        ('zero session accepted','control-reply-wire.c','!r->session || ','','attach-codec','attach-capacity'),
        ('zero trusted issuer accepted','control.c',' || !context->issuer','','attach-config','attach-codec'),
        ('detach never dispatched','server.c','frame.header.type==GT_MSG_DETACH','frame.header.type==65535','attach-flow','attach-codec'),
        ('zero configured ttl accepted','attachment-owner.c','!config->token_ttl_ms || ','','attach-config','attach-codec'),
        ('reservation capacity overflow accepted','attachment-owner.c',' || config->capacity>256','','attach-config','attach-codec'),
    )

    try:
        restore();build()
        for mode in MODES:probe(mode)
        for spec in specs:
            label,name,old,new,target,control=spec[:6];expected=spec[6] if len(spec)>6 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('attach mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build();probe(target,True);probe(control)
                print('PASS attach mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        build()
        for mode in MODES:probe(mode)
        print(f'PASS {len(specs)} attach mutations, {len(MODES)} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
