#!/usr/bin/env python3
"""Shared client bootstrap: ordinary isolated failures, no engine/PTY linkage."""
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/client-bootstrap-mutations'
MARK = 'GT_CLIENT_BOOTSTRAP_MUTATION_MARK'
NAMES = ('ipc/client-bootstrap.c', 'ipc/client-bootstrap.h', 'ipc/bootstrap-wire.c',
         'ipc/bootstrap-wire.h', 'ipc/control-client.c', 'ipc/control-client.h',
         'ipc/control-reply-wire.c')
PATHS = {name: ROOT / 'core/src' / name for name in NAMES}
PATHS['test.c'] = ROOT / 'core/tests/test_client_bootstrap.c'
PATHS['check.py'] = ROOT / 'scripts/check-attachment-client-deps.py'
MODES = ('management', 'data', 'rejections', 'malformed', 'deadlines', 'control',
         'control-bad', 'control-timeout', 'heartbeat', 'allocation', 'deps')


def run(args, timeout=30):
    p = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, start_new_session=True)
    try:
        out, _ = p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:
            try:
                os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        p.wait()
        raise
    return p.returncode, out


def main():
    original = {name: path.read_bytes() for name, path in PATHS.items()}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in original.items()}
    if any(MARK.encode() in b for b in original.values()) or any(
            MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2

    def interrupted(signum, frame):
        raise RuntimeError('client bootstrap mutation interrupted: ' + str(signum))

    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    os.environ['TMPDIR'] = str(ROOT / 'tmp')
    os.umask(0o077)
    copies = {name: WORK / name for name in PATHS}
    for p in copies.values():
        p.parent.mkdir(parents=True, exist_ok=True)

    def restore():
        for name, p in copies.items():
            p.write_bytes(original[name])
            if hashlib.sha256(p.read_bytes()).hexdigest() != hashes[name] or PATHS[name].read_bytes() != original[name]:
                raise RuntimeError('client bootstrap SHA256 rollback failed/' + name)

    restore()
    rc, out = run(['bash', str(ROOT / 'scripts/build-core.sh')], 120)
    if rc:
        raise RuntimeError('client dependency build failed: ' + out)
    archive = ROOT / 'core/build/libghosterm_core.a'
    archive_hash = hashlib.sha256(archive.read_bytes()).hexdigest()

    def build():
        rc, out = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
                       '-D_DARWIN_C_SOURCE' if sys.platform == 'darwin' else '-D_POSIX_C_SOURCE=200809L',
                       '-Wall', '-Wextra', '-Werror', '-I', str(WORK), '-I', str(WORK / 'ipc'),
                       '-I', str(ROOT / 'core/src'), '-I', str(ROOT / 'core/src/ipc'),
                       '-I', str(ROOT / 'core/include'),
                       *[str(copies[n]) for n in NAMES if n.endswith('.c')],
                       str(copies['test.c']), str(archive), '-pthread',
                       *(['-lutil'] if sys.platform.startswith('linux') else []), '-o', str(WORK / 'test')])
        if rc:
            raise RuntimeError('client bootstrap compilation failed: ' + out)
        if hashlib.sha256(archive.read_bytes()).hexdigest() != archive_hash:
            raise RuntimeError('client bootstrap archive changed')

    def probe(mode, reject=False):
        args = ([sys.executable, str(copies['check.py']), str(WORK / 'test')] if mode == 'deps'
                else [str(WORK / 'test'), mode])
        rc, out = run(args)
        if reject:
            if rc != 1 or 'FAIL ' not in out:
                raise RuntimeError('client mutation needs ordinary FAIL/exit 1/' + mode + ': ' + out)
        elif rc:
            raise RuntimeError('client baseline/control failed/' + mode + ': ' + out)

    specs = (
        ('compatibility false OK accepted', 'ipc/client-bootstrap.c', 'if(memcmp(r.identity.compatibility,c->config.hello.compatibility,GT_COMPAT_BYTES))', 'if(false && memcmp(r.identity.compatibility,c->config.hello.compatibility,GT_COMPAT_BYTES))', 'malformed', 'management'),
        ('required features ignored', 'ipc/client-bootstrap.c', 'if(c->config.hello.required_features&~r.identity.features)', 'if(false && (c->config.hello.required_features&~r.identity.features))', 'malformed', 'management'),
        ('wrong instance false OK accepted', 'ipc/client-bootstrap.c', 'if(nonzero(c->config.hello.expected_instance,GT_INSTANCE_BYTES) && memcmp', 'if(false && nonzero(c->config.hello.expected_instance,GT_INSTANCE_BYTES) && memcmp', 'malformed', 'management'),
        ('HELLO reply role ignored', 'ipc/client-bootstrap.c', ' || r.role!=c->config.hello.role', '', 'malformed', 'management'),
        ('HELLO reply request ID ignored', 'ipc/client-bootstrap.c', ' || f.header.request_id!=(c->config.hello.role==GT_ROLE_MANAGEMENT?1u:0u)', '', 'malformed', 'management'),
        ('BIND reply session ignored', 'ipc/client-bootstrap.c', 'r.session!=c->config.binding.session || ', '', 'malformed', 'management'),
        ('BIND reply generation ignored', 'ipc/client-bootstrap.c', 'r.generation!=c->config.binding.generation || ', '', 'malformed', 'management'),
        ('BIND reply request ID ignored', 'ipc/client-bootstrap.c', 'f.header.type!=GT_MSG_BIND_REPLY || f.header.request_id ||', 'f.header.type!=GT_MSG_BIND_REPLY ||', 'malformed', 'management'),
        ('BIND ACK shortwrite bypasses READY barrier', 'ipc/client-bootstrap.c', 'c->stage=hello?FLUSH_HELLO_ACK:FLUSH_BIND_ACK;', 'c->stage=hello?FLUSH_HELLO_ACK:FLUSH_BIND_ACK;\n    if(!hello && !c->rejected)c->info.state=GT_BOOTSTRAP_READY;', 'data', 'management'),
        ('rejection skips ACK drain', 'ipc/client-bootstrap.c', 'c->info.state=GT_BOOTSTRAP_DRAINING;', 'c->info.state=GT_BOOTSTRAP_FAILED;', 'rejections', 'management'),
        ('rejection has false success reason', 'ipc/client-bootstrap.c', 'c->info.error=rejection(r.status);', 'c->info.error=GT_BOOTSTRAP_NO_ERROR;(void)rejection(r.status);', 'rejections', 'management'),
        ('transaction timeout misses boundary', 'ipc/client-bootstrap.c', 'now-c->started>=c->config.timeout_ms', 'now-c->started>c->config.timeout_ms', 'deadlines', 'management'),
        ('client clock rollback ignored', 'ipc/client-bootstrap.c', ' || now<c->now', '', 'deadlines', 'management'),
        ('DATA expected instance not checked', 'ipc/client-bootstrap.c', '!memcmp(config->hello.expected_instance,config->binding.instance,GT_INSTANCE_BYTES) && ', '', 'allocation', 'management'),
        ('bootstrap connection handed over twice', 'ipc/client-bootstrap.c', 'c->connection=NULL;c->info.state=GT_BOOTSTRAP_TAKEN;', 'c->connection=NULL;c->info.state=GT_BOOTSTRAP_READY;', 'data', 'allocation'),
        ('bootstrap self storage not erased', 'ipc/client-bootstrap.c', 'gt__secure_erase(c,sizeof(*c));', '', 'data', 'deps'),
        ('control wrong request ID accepted', 'ipc/control-client.c', ' || f->header.request_id!=c->result.request_id', '', 'control-bad', 'control'),
        ('control wrong reply type accepted', 'ipc/control-client.c', 'if(f->header.type!=expected || n<4', 'if((void)expected, n<4', 'control-bad', 'control'),
        ('creation nonce mismatch accepted', 'ipc/control-client.c', ' || memcmp(nonce,r->nonce,16)', '', 'control-bad', 'control'),
        ('INFO different session accepted', 'ipc/control-client.c', ' || r->rows[0].id!=r->session', '', 'control-bad', 'control'),
        ('ATTACH other instance accepted', 'ipc/control-client.c', 'return r->attachment.status!=GT_ATTACH_OK || !memcmp(r->attachment.instance,c->config.instance,GT_INSTANCE_BYTES);', 'return true;', 'control-bad', 'control'),
        ('LIST duplicate IDs accepted', 'ipc/control-client.c', 'for(size_t j=0;j<i;j++)if(r->rows[j].id==r->rows[i].id)return false;', '', 'control-bad', 'control'),
        ('LIST failure carries successful rows', 'ipc/control-client.c', ' || (r->status!=GT_CONTROL_OK && count)', '', 'control-bad', 'control'),
        ('ambiguous creation reported as confirmed', 'ipc/control-client.c', '.delivery=GT_CONTROL_CLIENT_UNKNOWN', '.delivery=GT_CONTROL_CLIENT_REPLY', 'control-timeout', 'control'),
        ('pending operation nonce lost', 'ipc/control-client.c', 'memcpy(c->result.nonce,nonce,16);gt__secure_erase(nonce,sizeof(nonce));c->pending=0;', 'gt__secure_erase(nonce,sizeof(nonce));c->pending=0;', 'control-timeout', 'control'),
        ('control request deadline misses boundary', 'ipc/control-client.c', 'now-c->started>=c->config.request_timeout_ms', 'now-c->started>c->config.request_timeout_ms', 'control-timeout', 'control'),
        ('control clock rollback ignored', 'ipc/control-client.c', ' || now<c->now', '', 'control-timeout', 'control'),
        ('control request backpressure ignored', 'ipc/control-client.c', 'if(c->pending || gt__connection_wants_write(c->connection))return GT_CONTROL_CLIENT_BACKPRESSURE;', 'if(false && (c->pending || gt__connection_wants_write(c->connection)))return GT_CONTROL_CLIENT_BACKPRESSURE;', 'control-timeout', 'control'),
        ('auto heartbeat is incorrectly disabled', 'ipc/control-client.c', 'c->now-c->last_activity>=c->config.heartbeat_ms', 'c->now-c->last_activity>c->config.heartbeat_ms', 'heartbeat', 'control'),
        ('auto PING leaks user result callback', 'ipc/control-client.c', 'if(!automatic)c->config.result', 'if((void)automatic,true)c->config.result', 'heartbeat', 'control'),
        ('request ID not advanced', 'ipc/control-client.c', 'c->last_id=h.request_id;', 'c->last_id=0;', 'heartbeat', 'allocation'),
        ('unsupported CREATE mislabeled invalid', 'ipc/control-client.c', 'encoded==GT_ERR_UNSUPPORTED?GT_CONTROL_CLIENT_UNSUPPORTED:GT_CONTROL_CLIENT_INVALID', 'GT_CONTROL_CLIENT_INVALID', 'allocation', 'control'),
        ('unsupported CREATE reason discarded', 'ipc/control-client.c', '*error=encoded;', '*error=GT_OK;', 'allocation', 'control'),
        ('control self storage not erased', 'ipc/control-client.c', 'gt__secure_erase(c,sizeof(*c));', '', 'control', 'deps'),
    )
    try:
        build()
        for mode in MODES:
            probe(mode)
        for label, name, old, new, target, control in specs:
            source = original[name].decode()
            if source.count(old) != (2 if label=='rejection skips ACK drain' else 1):
                raise RuntimeError('client bootstrap mutation anchor drift/' + label + ': ' + str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old, new) + '\n/* ' + MARK + ' */\n')
                build(); probe(target, True); probe(control)
                print('PASS client bootstrap mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally:
                restore()
        try:
            source = original['ipc/control-reply-wire.c'].decode()
            copies['ipc/control-reply-wire.c'].write_text(source + '\n/* ' + MARK + ' */\nvoid gt__client_dependency_mutation(gt_session_manager_t *m){gt_managed_info_t info;(void)gt__manager_info(m,1,&info);}\n')
            build(); probe('deps', True); probe('management')
            print('PASS client bootstrap mutation/host runtime linked: ordinary red, same-mutant management green', flush=True)
        finally:
            restore()
        build()
        for mode in MODES:
            probe(mode)
        (ROOT / 'tmp/client-bootstrap-proof.json').write_text(json.dumps(
            {'sources': hashes, 'archive': archive_hash, 'baselines': MODES, 'mutations': len(specs)+1}, indent=2) + '\n')
        print(f'PASS {len(specs)+1} client bootstrap mutations, {len(MODES)} baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    sys.exit(main())
