#!/usr/bin/env python3
"""Prove session manager assertions against isolated C copies, never root edits."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT/'tmp/session-manager-mutations'
MARK = 'GT_SESSION_MANAGER_MUTATION_MARK'
PATHS = {'manager': ROOT/'core/src/session/manager.c', 'header': ROOT/'core/src/session/manager.h',
         'test': ROOT/'core/tests/test_session_manager.c'}


def run(command, **kwargs):
    timeout = kwargs.pop('timeout', 20)
    process = subprocess.Popen(command, start_new_session=True, **kwargs)
    try:
        out, err = process.communicate(timeout=timeout)
    except BaseException:
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        process.wait()
        raise
    return subprocess.CompletedProcess(command, process.returncode, out, err)


def main():
    originals = {name: path.read_bytes() for name, path in PATHS.items()}
    hashes = {name: hashlib.sha256(data).digest() for name, data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()) or any(
            MARK in p.read_text() for p in WORK.glob('*.[ch]')):
        return 2
    def interrupted(signum, frame):
        raise RuntimeError('manager mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    WORK.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    result = run(['bash', str(ROOT/'scripts/build-core.sh')], cwd=ROOT,
                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
    if result.returncode:
        raise RuntimeError('core build failed: '+result.stdout)
    # Test objects precede the archive so every target uses isolated versions.
    copies = {name: WORK/('manager.h' if name == 'header' else name+'.c') for name in PATHS}
    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('manager SHA256 rollback failed/'+name)
    binary = WORK/'probe'
    def build():
        result = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
            '-Wall', '-Wextra', '-Werror', '-pthread',
            *[arg for p in ('core/src', 'core/src/session', 'core/src/io', 'core/include', 'core/tests') for arg in ('-I', str(ROOT/p))],
            *[str(copies[name]) for name in ('manager', 'test')],
            str(ROOT/'core/build/libghosterm_core.a'),
            *(['-lutil'] if os.uname().sysname == 'Linux' else []), '-o', str(binary)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode:
            raise RuntimeError('mutation compilation failed: '+result.stdout)
    def probe(mode, reject=False):
        result = run([str(binary), mode], cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        if reject:
            # A signal alone, compiler error or watchdog expiry is not evidence.
            if result.returncode == 0 or 'Assertion' not in result.stderr and 'assertion' not in result.stderr:
                raise RuntimeError('mutation did not fail assertion/'+mode+': '+result.stderr)
        elif result.returncode:
            raise RuntimeError('manager baseline/control failed/'+mode+': '+result.stderr)
    specs = (
        ('registry bypasses allocation domain', 'manager', 'm=a.allocate(a.context,bytes);', 'm=malloc(bytes);', 'allocation', 'startup'),
        ('registry released with wrong charge', 'manager', 'a.release(a.context,m,bytes);return GT_MANAGER_OK;', 'a.release(a.context,m,bytes-1);return GT_MANAGER_OK;', 'allocation', 'startup'),
        ('registry allocation leaked', 'manager', 'a.release(a.context,m,bytes);return GT_MANAGER_OK;', '(void)a;(void)bytes;return GT_MANAGER_OK;', 'allocation', 'startup'),
        ('worker start rejection steals context', 'manager', 'if(!worker){memset(e,0,sizeof(*e));', 'if(!worker){dispose(config->context);memset(e,0,sizeof(*e));', 'allocation', 'startup'),
        ('worker start failure occupies slot', 'manager', 'if(!worker){memset(e,0,sizeof(*e));', 'if(!worker){', 'creating', 'startup'),
        ('IDs reused after forget', 'manager', 'm->next_id=m->next_id==UINT64_MAX?0:m->next_id+1;', 'm->next_id=m->next_id;', 'lifecycle', 'startup'),
        ('stale ID targets incumbent', 'manager', 'm->entries[i].id==id', '(m->entries[i].id==id || id!=0)', 'lifecycle', 'startup'),
        ('requested capacity ignored', 'manager', 'm->capacity=capacity;', 'm->capacity=1;', 'capacity', 'lifecycle'),
        ('retained terminal record loses admission', 'manager', 'if(m->entries[i].phase==EMPTY){e=&m->entries[i];break;}', 'if(m->entries[i].phase==EMPTY || m->entries[i].phase==RETAINED){e=&m->entries[i];break;}', 'lifecycle', 'startup'),
        ('active record can be forgotten', 'manager', 'e->phase!=RETAINED?GT_MANAGER_BUSY:GT_MANAGER_OK;', 'e->phase==REAPING?GT_MANAGER_BUSY:GT_MANAGER_OK;', 'lifecycle', 'startup'),
        ('unknown result invented', 'manager', 'else out->has_result=gt__worker_result(e->worker,&out->result)==GT_WORKER_RESULT_READY;', 'else {out->has_result=true;(void)gt__worker_result(e->worker,&out->result);}', 'lifecycle', 'allocation'),
        ('worker startup error collapsed', 'manager', 'pthread_mutex_unlock(&m->lock);return rc;\n}\ngt_manager_rc_t gt__manager_list', 'if(rc==GT_MANAGER_OK && out->has_result && out->result.kind==GT_WORKER_END_ERROR)out->result.kind=GT_WORKER_END_EOF;\n  pthread_mutex_unlock(&m->lock);return rc;\n}\ngt_manager_rc_t gt__manager_list', 'startup', 'allocation'),
        ('live reap pretends complete', 'manager', 'if(gt__worker_result(e->worker,&e->result)!=GT_WORKER_RESULT_READY){pthread_mutex_unlock(&m->lock);return GT_MANAGER_BUSY;}', 'if(gt__worker_result(e->worker,&e->result)!=GT_WORKER_RESULT_READY){pthread_mutex_unlock(&m->lock);return GT_MANAGER_OK;}', 'lifecycle', 'startup'),
        ('reap skips actual join', 'manager', 'gt__worker_join(worker);dispose(context);', '(void)worker;dispose(context);', 'joining', 'allocation'),
        ('context disposed before join', 'manager', 'gt__worker_join(worker);dispose(context);', 'dispose(context);gt__worker_join(worker);', 'joining', 'allocation'),
        ('second reap falsely accepted', 'manager', 'if(rc!=GT_MANAGER_OK || e->phase==RETAINED)', 'if(e && e->phase==REAPING){pthread_mutex_unlock(&m->lock);return GT_MANAGER_OK;}\n  if(rc!=GT_MANAGER_OK || e->phase==RETAINED)', 'joining', 'startup'),
        ('terminal result erased', 'manager', 'e->dispose=NULL;e->phase=RETAINED;', 'e->dispose=NULL;memset(&e->result,0,sizeof(e->result));e->phase=RETAINED;', 'lifecycle', 'allocation'),
        ('close frees creating registry', 'manager', 'm->entries[i].phase==CREATING || m->entries[i].phase==REAPING', 'm->entries[i].phase==REAPING', 'creating', 'startup'),
        ('close frees reaping registry', 'manager', 'm->entries[i].phase==CREATING || m->entries[i].phase==REAPING', 'm->entries[i].phase==CREATING', 'joining', 'startup'),
    )
    try:
        restore();build()
        for mode in ('lifecycle', 'startup', 'allocation', 'capacity', 'joining', 'creating'):
            probe(mode)
        for label, name, old, new, target, control in specs:
            text = originals[name].decode()
            if text.count(old) != 1:
                raise RuntimeError('nonunique manager anchor/'+label)
            try:
                copies[name].write_text(text.replace(old, new)+'\n/* '+MARK+' */\n')
                build();probe(target, True);probe(control)
                print('PASS manager mutation/'+label+': target assertion red, same-mutant control green', flush=True)
            finally:
                restore()
        build()
        for mode in ('lifecycle', 'startup', 'allocation', 'capacity', 'joining', 'creating'):
            probe(mode)
        print('PASS 19 manager mutations, six baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
