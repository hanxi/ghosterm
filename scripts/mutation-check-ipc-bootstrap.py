#!/usr/bin/env python3
"""Shared HELLO/BIND and token ownership: isolated red/control/rollback proof."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT/'tmp/ipc-bootstrap-mutations'
MARK = 'GT_IPC_BOOTSTRAP_MUTATION_MARK'
NAMES = [f'{group}/{name}.{suffix}'
         for group, names in (
             ('ipc', ('handshake', 'bootstrap-wire', 'tokens', 'connection', 'socket-stream', 'wire', 'writer')),
             ('session', ('checkpoint-task', 'attachment', 'restore-stream')),
             ('base', ('secure-random',)))
         for name in names for suffix in ('c', 'h')]
NAMES += ['base/allocator.h', 'base/thread-options.h']
PATHS = {name: ROOT/'core/src'/name for name in NAMES}
for suite in ('tokens', 'handshake'):
    PATHS[suite+'-test.c'] = ROOT/'core/tests'/('test_ipc_'+suite+'.c')
MODES = {
    'tokens': 'lifecycle binding capacity replay revocation entropy clock allocation os-random invalid idle-expiry'.split(),
    'handshake': 'management data native compatibility features instance challenge disconnect stale unclaimed expired timeout clock entropy codec transaction-deadline schema invalid'.split(),
}


def run(args):
    process = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, start_new_session=True)
    try:
        output, _ = process.communicate(timeout=20)
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
        raise RuntimeError('bootstrap mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    copies = {name: WORK/name for name in PATHS}
    for path in copies.values():
        path.parent.mkdir(parents=True, exist_ok=True)

    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('bootstrap SHA256 rollback failed/'+name)

    def build(suite):
        sources = [str(copies[name]) for name in NAMES if name.endswith('.c')]
        rc, output = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
                          '-I', str(WORK), '-I', str(ROOT/'core/include'), *sources,
                          str(copies[suite+'-test.c']), '-pthread', '-o', str(WORK/suite)])
        if rc:
            raise RuntimeError('bootstrap compilation failed: '+output)

    def probe(suite, mode, reject=False):
        rc, output = run([str(WORK/suite), mode])
        if reject:
            if rc != 1 or 'FAIL ' not in output:
                raise RuntimeError('mutation lacks targeted assertion/'+suite+'/'+mode+': '+output)
        elif rc:
            raise RuntimeError('bootstrap baseline/control failed/'+suite+'/'+mode+': '+output)

    # label, suite, source, exact original, replacement, target, independent control
    specs = (
        ('instance not bound','tokens','ipc/tokens.c','if(!equal(instance,s->instance,GT_INSTANCE_BYTES))','if(false && !equal(instance,s->instance,GT_INSTANCE_BYTES))','binding','allocation'),
        ('principal not bound','tokens','ipc/tokens.c','e->scope.principal!=principal || ','','binding','allocation'),
        ('session not bound','tokens','ipc/tokens.c','e->scope.session!=session || ','','binding','allocation'),
        ('generation not bound','tokens','ipc/tokens.c','e->scope.generation!=generation)return GT_TOKEN_DENIED;','false)return GT_TOKEN_DENIED;','binding','allocation'),
        ('token suffix ignored','tokens','ipc/tokens.c','equal(token->bytes,e->token.bytes,GT_TOKEN_BYTES)','equal(token->bytes,e->token.bytes,8)','binding','allocation'),
        ('token reusable','tokens','ipc/tokens.c','*out=e->scope;gt__secure_erase(e,sizeof(*e));return GT_TOKEN_OK;','*out=e->scope;return GT_TOKEN_OK;','lifecycle','invalid'),
        ('issuance counter repeated','tokens','ipc/tokens.c','s->next=s->next==UINT64_MAX?0:s->next+1;','s->next=s->next==UINT64_MAX?0:s->next;','replay','invalid'),
        ('exact expiry accepted','tokens','ipc/tokens.c','now-s->entries[i].issued>=s->entries[i].ttl','now-s->entries[i].issued>s->entries[i].ttl','capacity','binding'),
        ('idle expiry skipped','tokens','ipc/tokens.c','return s?advance(s,now):GT_TOKEN_INVALID;','(void)now;return s?GT_TOKEN_OK:GT_TOKEN_INVALID;','idle-expiry','binding'),
        ('duplicate reservation issued','tokens','ipc/tokens.c','return GT_TOKEN_BUSY;','break;','capacity','binding'),
        ('expiry never releases reservation','tokens','ipc/tokens.c','s->owner.revoke(s->owner.context,&scope);','(void)s;','idle-expiry','binding'),
        ('issuer revoke ignored','tokens','ipc/tokens.c','s->entries[i].scope.issuer==issuer','s->entries[i].scope.issuer!=issuer','revocation','binding'),
        ('generation revoke too broad','tokens','ipc/tokens.c',' && s->entries[i].scope.generation==generation)revoke',' )revoke','revocation','binding'),
        ('entropy failure accepted','tokens','ipc/tokens.c','entropy!=GT_OK || !nonzero(token.bytes+8,32)','!nonzero(token.bytes+8,32)','entropy','binding'),
        ('zero secret accepted','tokens','ipc/tokens.c','entropy!=GT_OK || !nonzero(token.bytes+8,32)','entropy!=GT_OK && !nonzero(token.bytes+8,32)','entropy','binding'),
        ('clock failure not permanent','tokens','ipc/tokens.c','s->failed=true;','s->failed=false;','clock','binding'),
        ('allocator bypassed','tokens','ipc/tokens.c','gt_token_store_t *s=a.allocate(a.context,bytes);','gt_token_store_t *s=malloc(bytes);','allocation','binding'),
        ('store not erased','tokens','ipc/tokens.c','gt__secure_erase(s,bytes);','(void)bytes;','allocation','binding'),
        ('wrong allocation refund','tokens','ipc/tokens.c','a.release(a.context,s,bytes);','a.release(a.context,s,bytes-1);','allocation','binding'),
        ('compatibility unchecked','handshake','ipc/handshake.c','if(!equal(request.compatibility,h->config.identity.compatibility,GT_COMPAT_BYTES))','if(false && !equal(request.compatibility,h->config.identity.compatibility,GT_COMPAT_BYTES))','compatibility','codec'),
        ('missing feature accepted','handshake','ipc/handshake.c','else if(request.required_features&~h->config.identity.features)','else if(false && (request.required_features&~h->config.identity.features))','features','codec'),
        ('wrong instance accepted','handshake','ipc/handshake.c','else if(nonzero(request.expected_instance,GT_INSTANCE_BYTES) && !equal','else if(false && nonzero(request.expected_instance,GT_INSTANCE_BYTES) && !equal','instance','codec'),
        ('ACK challenge unchecked','handshake','ipc/handshake.c',' || !equal(frame.payload,h->challenge,GT_CHALLENGE_BYTES)','', 'challenge','codec'),
        ('management request ID zero accepted','handshake','ipc/handshake.c','(request.role==GT_ROLE_MANAGEMENT?!frame->header.request_id:frame->header.request_id!=0)','false','schema','codec'),
        ('failure leaks consumed generation','handshake','ipc/handshake.c','h->state=GT_HANDSHAKE_FAILED;drop_binding(h);','h->state=GT_HANDSHAKE_FAILED;','disconnect','management'),
        ('old binding not rechecked','handshake','ipc/handshake.c','if(h->bound && !h->config.binding_current(h->config.binding_context,&h->binding)){fail','if(false && h->bound && !h->config.binding_current(h->config.binding_context,&h->binding)){fail','stale','management'),
        ('unclaimed binding leaked','handshake','ipc/handshake.c','if(h->state!=GT_HANDSHAKE_TAKEN)fail(h,GT_HANDSHAKE_IO);','if(false)fail(h,GT_HANDSHAKE_IO);','unclaimed','management'),
        ('whole transaction deadline reset','handshake','ipc/handshake.c','h->state=GT_HANDSHAKE_BIND_ACK;','{h->state=GT_HANDSHAKE_BIND_ACK;h->started=h->now;}','transaction-deadline','codec'),
        ('deadline boundary missed','handshake','ipc/handshake.c','now-h->started>=h->config.timeout_ms','now-h->started>h->config.timeout_ms','timeout','codec'),
        ('clock rollback ignored','handshake','ipc/handshake.c',' || now<h->now','', 'clock','codec'),
        ('failed challenge entropy accepted','handshake','ipc/handshake.c','if(status!=GT_OK || !nonzero(out,n))','if(!nonzero(out,n))','entropy','codec'),
        ('constructor failure closes caller stream','handshake','ipc/handshake.c','if(!config->clock(config->clock_context,&h->now)){a.release','if(!config->clock(config->clock_context,&h->now)){gt__connection_cancel(c);a.release','invalid','codec'),
        ('binding handed off twice','handshake','ipc/handshake.c','h->bound=false;h->state=GT_HANDSHAKE_TAKEN;','h->bound=false;h->state=GT_HANDSHAKE_READY;','data','codec'),
    )
    try:
        restore()
        for suite, modes in MODES.items():
            build(suite)
            for mode in modes:
                probe(suite, mode)
        for label, suite, name, old, new, target, control in specs:
            source = originals[name].decode()
            if source.count(old) != 1:
                raise RuntimeError('bootstrap mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True);probe(suite,control)
                print('PASS bootstrap mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:
                restore()
        # Reach issuance exhaustion without a production knob or root-source edit.
        name = 'ipc/tokens.c'
        initial = originals[name].decode().replace('s->next=1;', 's->next=UINT64_MAX;')
        copies[name].write_text(initial);build('tokens');probe('tokens','exhaustion')
        copies[name].write_text(initial.replace('s->next==UINT64_MAX?0:', 's->next==UINT64_MAX?1:')+'\n/* '+MARK+' */\n')
        build('tokens');probe('tokens','exhaustion',True);probe('tokens','binding')
        restore()
        for suite, modes in MODES.items():
            build(suite)
            for mode in modes:
                probe(suite, mode)
        print(f'PASS {len(specs)+1} bootstrap mutations, {sum(map(len,MODES.values()))} baselines, exhaustion fixture and SHA256 rollback',flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
