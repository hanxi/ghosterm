#!/usr/bin/env python3
"""Shared activation: ordinary isolated failures, no engine/PTY linkage."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/activation-mutations'
MARK = 'GT_ACTIVATION_MUTATION_MARK'
NAMES = ('daemon/src/activation.c', 'daemon/src/activation.h', 'daemon/src/startup-wire.c', 'daemon/src/startup-wire.h',
         'daemon/src/proxy.c', 'daemon/src/main.c', 'daemon/tests/test-activation.c', 'daemon/tests/activation-io.c', 'daemon/tests/test-host-process.c')
PATHS = {name: ROOT / name for name in NAMES}
PATHS['check.py'] = ROOT / 'scripts/check-attachment-client-deps.py'
MODES = ('launch', 'data', 'old-instance', 'cancel', 'locks', 'incompatible', 'features', 'timeout', 'existing', 'trusted', 'stdio',
         'malformed', 'role', 'concurrent', 'codec', 'barrier', 'backpressure', 'disconnect', 'recheck', 'committed-cancel', 'empty', 'pipes', 'stderr', 'unsafe-entry', 'publish', 'short', 'flow', 'deps', 'help')


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
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir',type=Path,required=True)
    args=parser.parse_args();build_dir=args.build_dir.resolve()
    original = {name: path.read_bytes() for name, path in PATHS.items()}
    hashes = {name: hashlib.sha256(data).hexdigest() for name, data in original.items()}
    if any(MARK.encode() in b for b in original.values()) or any(
            MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2

    def interrupted(signum, frame):
        raise RuntimeError('activation mutation interrupted: ' + str(signum))

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
                raise RuntimeError('activation SHA256 rollback failed/' + name)

    restore()
    cache={}
    for line in (build_dir/'CMakeCache.txt').read_text().splitlines():
        if '=' in line:cache[line.split(':',1)[0]]=line.split('=',1)[1]
    include=Path(cache['GHOSTERM_VT_INCLUDE_DIR']);engine=Path(cache['GHOSTERM_VT_LIBRARY'])
    core=ROOT/'core/build/libghosterm_core.a';client=ROOT/'core/build/libghosterm_client_protocol.a'
    vt=build_dir/'libghosterm_daemon_vt.a';snapshot=build_dir/'libghosterm_vt_snapshot.a';host=build_dir/'libghosterm_session_host.a'
    dependencies=[core,client,vt,snapshot,host,build_dir/'generated/session-contract.h',engine,ROOT/'core/src/ipc/socket-stream.c',*sorted(include.rglob('*.h'))]
    dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
    pkg_rc,pkg_out=run(['pkg-config','--libs','libssh2'])
    ssh_libraries=shlex.split(pkg_out) if pkg_rc==0 else []
    def build():
        common=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-D_DEFAULT_SOURCE',
            '-I',str(WORK/'daemon/src'),'-I',str(ROOT/'daemon/src'),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/include'),
            '-I',str(ROOT/'terminal/session-state'),'-I',str(ROOT/'terminal/state'),'-I',str(include),'-I',str(build_dir/'generated')]
        activation=[str(copies['daemon/src/activation.c']),str(copies['daemon/src/startup-wire.c'])]
        tail=[str(client),str(core),*ssh_libraries,'-pthread','-Wl,-rpath,'+str(engine.parent),*(['-lutil'] if sys.platform.startswith('linux') else [])]
        engine_libs=[str(host),str(vt),str(snapshot),str(engine)]
        # Core archive is a fallback only: linked boundary checks prove the proxy
        # does not extract host objects. A negative variant deliberately does.
        for source,name,extras in (('daemon/src/main.c','server',engine_libs),('daemon/src/proxy.c','proxy',[]),('daemon/tests/test-activation.c','test',[]),('daemon/tests/test-host-process.c','flow',engine_libs)):
            rc,out=run([*common,str(copies[source]),*activation,*extras,*tail,'-o',str(WORK/name)])
            if rc:raise RuntimeError('activation compilation failed/'+name+': '+out)
        rc,out=run([*common,'-Dsend=gt_test_send','-Drecv=gt_test_recv','-c',str(ROOT/'core/src/ipc/socket-stream.c'),'-o',str(WORK/'short.o')])
        if rc:raise RuntimeError('short IO shim compilation failed: '+out)
        rc,out=run([*common,'-Drecv=gt_test_recv','-c',str(copies['daemon/src/activation.c']),'-o',str(WORK/'short-activation.o')])
        if rc:raise RuntimeError('short activation compilation failed: '+out)
        rc,out=run([*common,'-DGT_TEST_SHORT_IO=1',str(copies['daemon/tests/test-activation.c']),str(copies['daemon/tests/activation-io.c']),str(WORK/'short.o'),str(WORK/'short-activation.o'),str(copies['daemon/src/startup-wire.c']),*tail,'-o',str(WORK/'short')])
        if rc:raise RuntimeError('short IO test compilation failed: '+out)
        rc,out=run([*common,str(copies['daemon/src/main.c']),str(copies['daemon/tests/activation-io.c']),str(WORK/'short.o'),*activation,*engine_libs,*tail,'-o',str(WORK/'short-server')])
        if rc:raise RuntimeError('short server compilation failed: '+out)
        if any(hashlib.sha256(Path(p).read_bytes()).hexdigest()!=h for p,h in dependency_hashes.items()):raise RuntimeError('activation dependencies changed')
    def probe(mode,reject=False):
        if mode=='deps':args=[sys.executable,str(copies['check.py']),str(WORK/'proxy')]
        elif mode=='help':args=[str(WORK/'proxy'),'--help']
        elif mode=='flow':args=[str(WORK/'flow'),'proxy-flow',str(WORK/'server'),str(ROOT),str(WORK/'proxy')]
        else:args=[str(WORK/('short' if mode=='short' else 'test')),'launch' if mode=='short' else mode,str(WORK/('short-server' if mode=='short' else 'server')),str(WORK/'proxy'),str(ROOT)]
        rc,out=run(args,45)
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('activation mutation needs ordinary FAIL/exit 1/'+mode+': '+out)
        elif rc:raise RuntimeError('activation baseline/control failed/'+mode+': '+out)
    specs = [
        ('new process stays in caller session', 'daemon/src/activation.c', [('POSIX_SPAWN_SETSID|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF', 'POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF')], 'launch', 'trusted'),
        ('blocked signals leak into daemon', 'daemon/src/activation.c', [('POSIX_SPAWN_SETSID|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF', 'POSIX_SPAWN_SETSID|POSIX_SPAWN_SETSIGDEF')], 'launch', 'trusted'),
        ('ignored SIGPIPE leaks into shell host', 'daemon/src/activation.c', [('POSIX_SPAWN_SETSID|POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF', 'POSIX_SPAWN_SETSID|POSIX_SPAWN_SETSIGMASK')], 'stdio', 'trusted'),
        ('daemon inherits SSH stderr', 'daemon/src/activation.c', [('i<3 && !rc', 'i<2 && !rc'), ('if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,pair[1],3);', 'if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,2,2);\n  if(!rc)rc=posix_spawn_file_actions_adddup2(&actions,pair[1],3);')], 'stdio', 'trusted'),
        ('untrusted helper path accepted', 'daemon/src/activation.c', [(' || !trusted_helper(helper)', ' || (false && !trusted_helper(helper))')], 'trusted', 'data'),
        ('writable helper path accepted', 'daemon/src/activation.c', [(' && !(st.st_mode&0022)', '')], 'trusted', 'data'),
        ('helper symlink followed', 'daemon/src/activation.c', [('O_NOFOLLOW', '0', 2)], 'trusted', 'data'),
        ('data entry starts a replacement daemon', 'daemon/src/activation.c', [('c->hello.role==GT_ROLE_MANAGEMENT && !nonzero(c->hello.expected_instance,16)', 'c->hello.role==GT_ROLE_DATA || !nonzero(c->hello.expected_instance,16)')], 'data', 'trusted'),
        ('old instance silently replaced', 'daemon/src/activation.c', [('c->hello.role==GT_ROLE_MANAGEMENT && !nonzero(c->hello.expected_instance,16)', 'c->hello.role==GT_ROLE_MANAGEMENT && (true || !nonzero(c->hello.expected_instance,16))')], 'old-instance', 'trusted'),
        ('startup lock omitted', 'daemon/src/activation.c', [('er=gt__unix_endpoint_startup_lock(endpoint);', 'er=GT_ENDPOINT_OK;')], 'concurrent', 'trusted'),
        ('locked endpoint not rechecked', 'daemon/src/activation.c', [('er=gt__unix_endpoint_connect(endpoint,0,&out->fd);', 'er=locked && !spawned?GT_ENDPOINT_NOT_FOUND:gt__unix_endpoint_connect(endpoint,0,&out->fd);')], 'recheck', 'trusted'),
        ('startup cancellation ignored', 'daemon/src/activation.c', [('if(c->cancelled && c->cancelled(c->context))', 'if(false && c->cancelled && c->cancelled(c->context))')], 'cancel', 'data'),
        ('timeout mislabeled as absent', 'daemon/src/activation.c', [('result=GT_ACTIVATION_TIMEOUT;', 'result=GT_ACTIVATION_NOT_FOUND;')], 'locks', 'data'),
        ('readiness compatibility ignored', 'daemon/src/activation.c', [('if(memcmp(identity.compatibility,c->hello.compatibility,32))', 'if(false && memcmp(identity.compatibility,c->hello.compatibility,32))')], 'incompatible', 'features'),
        ('required features ignored', 'daemon/src/activation.c', [('if(c->hello.required_features&~identity.features)', 'if(false && (c->hello.required_features&~identity.features))')], 'features', 'incompatible'),
        ('partial READY mistaken for full frame', 'daemon/src/activation.c', [('received+=(size_t)n', 'received=72')], 'short', 'trusted'),
        ('partial ACK mistaken for full write', 'daemon/src/activation.c', [('sent+=w.length;out->committed=true;', 'sent=24;out->committed=true;')], 'short', 'trusted'),
        ('ACK publication forgotten', 'daemon/src/activation.c', [('out->committed=true;', 'out->committed=false;')], 'committed-cancel', 'trusted'),
        ('committed daemon killed on cancellation', 'daemon/src/activation.c', [(' || out->committed', '')], 'committed-cancel', 'trusted'),
        ('readiness magic ignored', 'daemon/src/startup-wire.c', [('memcmp(in,"GTSR",4) || ', '')], 'codec', 'trusted'),
        ('readiness version ignored', 'daemon/src/startup-wire.c', [('get(in+4,2)!=1 || ', '')], 'codec', 'trusted'),
        ('readiness length ignored', 'daemon/src/startup-wire.c', [('get(in+6,2)!=72 || ', '')], 'codec', 'trusted'),
        ('readiness reserved fields ignored', 'daemon/src/startup-wire.c', [(' || get(in+12,4)', '')], 'codec', 'trusted'),
        ('failure reply carries identity', 'daemon/src/startup-wire.c', [('else if(nonzero(in+16,56))return false;', 'else if(false && nonzero(in+16,56))return false;')], 'codec', 'trusted'),
        ('unknown readiness accepted as OK', 'daemon/src/startup-wire.c', [('if(status)*status=GT_ERR_PROTOCOL;', 'if(status)*status=GT_OK;')], 'codec', 'trusted'),
        ('ACK instance ignored', 'daemon/src/startup-wire.c', [('!memcmp(in,"GTSA",4) && get(in+4,2)==1 && get(in+6,2)==24 && !memcmp(in+8,instance,16)', '!memcmp(in,"GTSA",4) && get(in+4,2)==1 && get(in+6,2)==24')], 'codec', 'trusted'),
        ('commit instance ignored', 'daemon/src/startup-wire.c', [('!memcmp(in,"GTSC",4) && get(in+4,2)==1 && get(in+6,2)==24 && !memcmp(in+8,instance,16)', '!memcmp(in,"GTSC",4) && get(in+4,2)==1 && get(in+6,2)==24')], 'codec', 'trusted'),
        ('detached entry accepts unsafe context', 'daemon/src/main.c', [('if(detached && !detached_context())return 2;', '(void)detached_context;')], 'unsafe-entry', 'codec'),
        ('service IO starts without ready ACK', 'daemon/src/main.c', [('if(detached && !gt__startup_serve(3,error,&config.identity,5000,NULL,cancelled))error=GT_ERR_CANCELLED;', '(void)cancelled;')], 'barrier', 'trusted'),
        ('proxy ignores requested role', 'daemon/src/proxy.c', [(' && hello->role==role', '')], 'role', 'malformed'),
        ('invalid first frame reported as IO', 'daemon/src/proxy.c', [('return GT_ACTIVATION_INVALID;', 'return GT_ACTIVATION_IO;', 1)], 'malformed', 'role'),
        ('original HELLO not forwarded', 'daemon/src/proxy.c', [('memcpy(input.bytes,hello,84);input.end=84;', '(void)hello;')], 'backpressure', 'codec'),
        ('transmitted bytes never retired', 'daemon/src/proxy.c', [('b->begin+=n;', '')], 'backpressure', 'codec'),
        ('buffer compaction loses pending bytes', 'daemon/src/proxy.c', [('memmove(b->bytes,b->bytes+b->begin,n);', '')], 'backpressure', 'codec'),
        ('full input queue hides client death', 'daemon/src/proxy.c', [('return ((fds[0].revents|fds[1].revents)&(POLLHUP|POLLERR|POLLNVAL))!=0;', 'return false;'), ('if((fds[0].revents|fds[1].revents)&(POLLHUP|POLLERR))break;', 'if(false && ((fds[0].revents|fds[1].revents)&(POLLHUP|POLLERR)))break;')], 'disconnect', 'codec'),
        ('stderr pressure blocks teardown', 'daemon/src/proxy.c', [('fcntl(2,F_SETFL,flags|O_NONBLOCK)', 'fcntl(2,F_SETFL,flags)')], 'stderr', 'codec'),
    ]
    specs.extend([
        ('publication claims rename without moving', 'daemon/src/proxy.c', [('int result=okay?renameat(a,from,b,to):-1;', 'int result=okay?0:-1;')], 'publish', 'codec'),
        ('publication replaces an existing empty release', 'daemon/src/proxy.c', [('if(!fstatat(b,to,&st,AT_SYMLINK_NOFOLLOW) || errno!=ENOENT)okay=false;', '(void)st;')], 'publish', 'codec'),
        ('publication follows parent links', 'daemon/src/proxy.c', [('O_NOFOLLOW', '0', 2)], 'publish', 'codec'),
        ('publication trusts writable parents', 'daemon/src/proxy.c', [('(st.st_mode&0022)', 'false')], 'publish', 'codec'),
    ])
    specs.append(('unrelated descriptors inherited','daemon/src/activation.c',
        [('flags|=POSIX_SPAWN_CLOEXEC_DEFAULT;','flags|=0;')] if sys.platform=='darwin' else
        [('if(!rc)rc=posix_spawn_file_actions_addclosefrom_np(&actions,4);','(void)actions;')], 'launch','trusted'))
    try:
        build()
        for mode in MODES:
            probe(mode)
        for label, name, edits, target, control in specs:
            source = original[name].decode()
            for edit in edits:
                old,new=edit[:2];expected=edit[2] if len(edit)>2 else 1
                if source.count(old)!=expected:raise RuntimeError('activation mutation anchor drift/'+label+': '+str(source.count(old)))
                source=source.replace(old,new,expected)
            try:
                copies[name].write_text(source + '\n/* ' + MARK + ' */\n')
                build(); probe(target, True); probe(control)
                print('PASS activation mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally:
                restore()
        try:
            name='daemon/src/activation.c';source=original[name].decode()
            copies[name].write_text(source+'\n/* '+MARK+' */\n#include "session/manager.h"\nvoid gt__activation_dependency_negative(gt_session_manager_t *m){gt_managed_info_t i;(void)gt__manager_info(m,1,&i); }\n')
            build();probe('deps',True);probe('help')
            print('PASS activation mutation/host runtime linked: ordinary red, same-mutant help green',flush=True)
        finally:restore()
        build()
        for mode in MODES:
            probe(mode)
        (ROOT / 'tmp/activation-proof.json').write_text(json.dumps(
            {'sources': hashes, 'dependencies': dependency_hashes, 'baselines': MODES, 'mutations': (len(specs)+1)}, indent=2) + '\n')
        print(f'PASS {(len(specs)+1)} activation mutations, {len(MODES)} baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    sys.exit(main())
