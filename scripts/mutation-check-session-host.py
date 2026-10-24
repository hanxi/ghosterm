#!/usr/bin/env python3
"""Shared session host: ordinary isolated failures, no engine/PTY linkage."""
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
WORK = ROOT / 'tmp/session-host-mutations'
MARK = 'GT_SESSION_HOST_MUTATION_MARK'
NAMES = ('daemon/src/host.c', 'daemon/src/host.h', 'daemon/src/memory.c', 'daemon/src/memory.h',
         'daemon/src/main.c', 'daemon/tests/test-host-process.c')
PATHS = {name: ROOT / name for name in NAMES}
MODES = ('flow', 'competing', 'malformed', 'allocation', 'retention', 'shutdown', 'capacity', 'sessions', 'version')


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
        raise RuntimeError('session host mutation interrupted: ' + str(signum))

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
                raise RuntimeError('session host SHA256 rollback failed/' + name)

    restore()
    dependencies=[ROOT/'core/build/libghosterm_core.a', build_dir/'libghosterm_daemon_vt.a',
                  build_dir/'libghosterm_vt_snapshot.a',build_dir/'generated/session-contract.h', build_dir/'libghosterm_session_activation.a']
    cache={}
    for line in (build_dir/'CMakeCache.txt').read_text().splitlines():
        if '=' in line:cache[line.split(':',1)[0]]=line.split('=',1)[1]
    include=Path(cache['GHOSTERM_VT_INCLUDE_DIR']);engine=Path(cache['GHOSTERM_VT_LIBRARY'])
    dependencies += [engine,*sorted(include.rglob('*.h'))]
    dependency_hashes={str(p):hashlib.sha256(p.read_bytes()).hexdigest() for p in dependencies}
    pkg_rc,pkg_out=run(['pkg-config','--libs','libssh2'])
    ssh_libraries=shlex.split(pkg_out) if pkg_rc==0 else []
    def build():
        common=[*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-D_DEFAULT_SOURCE',
            '-I',str(WORK/'daemon/src'),'-I',str(ROOT/'daemon/src'),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/include'),
            '-I',str(ROOT/'terminal/session-state'),'-I',str(ROOT/'terminal/state'),'-I',str(include),'-I',str(build_dir/'generated')]
        libs=[str(dependencies[1]),str(dependencies[2]),str(dependencies[4]),str(dependencies[0]),str(engine),*ssh_libraries,'-pthread','-Wl,-rpath,'+str(engine.parent),
              *(['-lutil'] if sys.platform.startswith('linux') else [])]
        for source,name in (('daemon/src/main.c','server'),('daemon/tests/test-host-process.c','test')):
            rc,out=run([*common,str(copies['daemon/src/host.c']),str(copies['daemon/src/memory.c']),str(copies[source]),*libs,'-o',str(WORK/name)])
            if rc:raise RuntimeError('host compilation failed/'+name+': '+out)
        if any(hashlib.sha256(Path(p).read_bytes()).hexdigest()!=h for p,h in dependency_hashes.items()):raise RuntimeError('host dependencies changed')
    def probe(mode,reject=False):
        args=([str(WORK/'server'),'--version'] if mode=='version' else [str(WORK/'test'),mode,str(WORK/'server'),str(ROOT)])
        rc,out=run(args,45)
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('host mutation needs ordinary FAIL/exit 1/'+mode+': '+out)
        elif rc:raise RuntimeError('host baseline/control failed/'+mode+': '+out)
    specs = (
        ('prototype four-session cap retained', 'daemon/src/main.c', '.sessions=32,.connections=64', '.sessions=4,.connections=64', 'sessions', 'version'),
        ('prototype eight-connection cap retained', 'daemon/src/main.c', '.sessions=32,.connections=64', '.sessions=32,.connections=8', 'capacity', 'sessions'),
        ('failed constructor keeps output handle', 'daemon/src/host.c', 'if(out)*out=NULL;', '(void)out;', 'allocation', 'flow'),
        ('IO peer principal left unauthenticated', 'daemon/src/host.c', '(uint64_t)geteuid()+1', '0', 'flow', 'allocation'),
        ('independent reaper not driven', 'daemon/src/host.c', 'while(!atomic_load(&h->stopping))', 'while(false && !atomic_load(&h->stopping))', 'retention', 'allocation'),
        ('terminal worker never joined', 'daemon/src/host.c', 'gt__runtime_reap(h->runtime,rows[i].id)!=GT_MANAGER_OK', 'false', 'retention', 'allocation'),
        ('finished session never reclaimed', 'daemon/src/host.c', 'gt_runtime_rc_t rc=gt__runtime_forget(h->runtime,rows[i].id);', 'gt_runtime_rc_t rc=GT_RUNTIME_BUSY;', 'retention', 'capacity'),
        ('full peer slot leaks accepted descriptor', 'daemon/src/host.c', 'if(!p)close(fd);', 'if(!p)(void)fd;', 'capacity', 'flow'),
        ('dead connection slot never reused', 'daemon/src/host.c', 'h->peers[i].issuer=0;', '(void)h->peers[i].issuer;', 'capacity', 'malformed'),
        ('host self allocation not refunded', 'daemon/src/host.c', 'a.release(a.context,h,n);', '(void)a;', 'allocation', 'version'),
        ('DATA dispatcher omitted', 'daemon/src/host.c', '.data_step=gt__data_router_step,', '.data_step=NULL,', 'flow', 'version'),
        ('C allocation cap ignored', 'daemon/src/memory.c', '!n || n>m->limit || used>m->limit-n', '!n', 'allocation', 'flow'),
        ('C domain refunds lost', 'daemon/src/memory.c', 'free(p);atomic_fetch_sub(&m->used,n);', 'free(p);(void)m;', 'allocation', 'version'),
        ('SIGINT bypasses explicit shutdown', 'daemon/src/main.c', '!sigaction(SIGINT,&action,NULL)', 'true', 'shutdown', 'competing'),
        ('SIGTERM bypasses explicit shutdown', 'daemon/src/main.c', '!sigaction(SIGTERM,&action,NULL)', 'true', 'flow', 'allocation'),
        ('server compatibility fingerprint diverges', 'daemon/src/main.c', 'if(error==GT_OK)error=gt__session_host_open', 'config.identity.compatibility[0]^=1;\n    if(error==GT_OK)error=gt__session_host_open', 'flow', 'allocation'),
        ('explicit host stop ignores signal', 'daemon/src/main.c', 'while(error==GT_OK && !atomic_load_explicit(&stopping,memory_order_relaxed))', 'while(error==GT_OK)', 'shutdown', 'allocation'),
        ('one session terminates another', 'daemon/src/host.c', 'gt__data_router_reap(h->router);', 'gt__data_router_reap(h->router);gt_managed_info_t check[64];size_t cn=0;if(gt__manager_list(gt__runtime_manager(h->runtime),check,64,&cn)==GT_MANAGER_OK)for(size_t k=0;k<cn;k++)if(check[k].termination_requested)for(size_t j=0;j<cn;j++)(void)gt__manager_terminate(gt__runtime_manager(h->runtime),check[j].id);', 'shutdown', 'flow'),
    )
    try:
        build()
        for mode in MODES:
            probe(mode)
        for label, name, old, new, target, control in specs:
            source = original[name].decode()
            if source.count(old) != 1:
                raise RuntimeError('session host mutation anchor drift/' + label + ': ' + str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old, new) + '\n/* ' + MARK + ' */\n')
                build(); probe(target, True); probe(control)
                print('PASS session host mutation/' + label + ': ordinary red, same-mutant control green', flush=True)
            finally:
                restore()
        build()
        for mode in MODES:
            probe(mode)
        (ROOT / 'tmp/session-host-proof.json').write_text(json.dumps(
            {'sources': hashes, 'dependencies': dependency_hashes, 'baselines': MODES, 'mutations': len(specs)}, indent=2) + '\n')
        print(f'PASS {len(specs)} session host mutations, {len(MODES)} baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    sys.exit(main())
