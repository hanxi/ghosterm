#!/usr/bin/env python3
"""Private Unix endpoint: real sockets/locks, isolated mutations and controls."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT/'tmp/unix-endpoint-mutations'
RUNS = WORK/'runs'
MARK = 'GT_UNIX_ENDPOINT_MUTATION_MARK'
PATHS = {'source': ROOT/'core/src/ipc/unix-endpoint.c',
         'header': ROOT/'core/src/ipc/unix-endpoint.h',
         'test': ROOT/'core/tests/test_unix_endpoint.c', 'peer-test': ROOT/'core/tests/test_endpoint_peer.c'}
MODES = ('flow', 'locks', 'stale', 'live', 'replacement', 'permissions', 'symlinks',
         'nodes', 'lock-nodes', 'peer', 'allocation', 'directory-replaced', 'invalid', 'inheritance', 'peer-configuration', 'peer-identity', 'peer-capacity', 'nested')


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
        raise RuntimeError('Unix endpoint mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    os.umask(0o077)
    (WORK/'ipc').mkdir(parents=True, exist_ok=True)
    RUNS.mkdir(exist_ok=True, mode=0o700)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    copies = {'source': WORK/'unix-endpoint.c', 'header': WORK/'ipc/unix-endpoint.h', 'test': WORK/'test.c', 'peer-test': WORK/'peer-test.c'}
    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('Unix endpoint SHA256 rollback failed/'+name)
    def build():
        rc, output = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(WORK), '-I', str(WORK/'ipc'), '-I', str(ROOT/'core/src'),
            str(copies['source']), str(copies['test']), '-o', str(WORK/'probe')])
        if rc:
            raise RuntimeError('Unix endpoint compilation failed: '+output)
        wrapper = '-Dgetpeereid=gt_test_getpeereid' if os.uname().sysname=='Darwin' else '-Dgetsockopt=gt_test_getsockopt'
        rc, output = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-Dfcntl=gt_test_fcntl', '-Daccept=gt_test_accept', wrapper, '-I', str(WORK), '-I', str(WORK/'ipc'),
            '-I', str(ROOT/'core/src'), '-c', str(copies['source']), '-o', str(WORK/'peer.o')])
        if rc:raise RuntimeError('peer endpoint compilation failed: '+output)
        rc, output = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(WORK), '-I', str(ROOT/'core/src'), str(copies['peer-test']), str(WORK/'peer.o'), '-o', str(WORK/'peer')])
        if rc:raise RuntimeError('peer test compilation failed: '+output)
    def probe(mode, reject=False):
        rc, output = run([str(WORK/('peer' if mode.startswith('peer-') else 'probe')), mode.removeprefix('peer-'), str(RUNS)])
        if reject:
            if rc != 1 or 'FAIL ' not in output:
                raise RuntimeError('mutation lacks targeted assertion/'+mode+': '+output)
        elif rc:
            raise RuntimeError('Unix endpoint baseline/control failed/'+mode+': '+output)
    # A spec can remove multiple complementary checks to create the actual
    # invalid state (rather than merely changing an error classification).
    specs = (
        ('cold runtime cannot create parents', (('if(errno!=ENOENT || !create)', 'if(errno!=ENOENT || !leaf || !create)'),), 'nested', 'flow'),
        ('read-only lookup creates directories', (('if(errno!=ENOENT || !create)', 'if(errno!=ENOENT || (!create && false))'),), 'nested', 'flow'),
        ('new parent directories are public', (('mkdirat(dir,component,0700)', 'mkdirat(dir,component,leaf?0700:0755)'),), 'nested', 'flow'),
        ('failed peer configuration poisons listener', (('if(!configure(fd)){close(fd);return GT_ENDPOINT_REJECTED;}', 'if(!configure(fd)){close(fd);return GT_ENDPOINT_IO;}'),), 'peer-configuration', 'flow'),
        ('failed peer authentication poisons listener', (('return rc==GT_ENDPOINT_UNSUPPORTED?rc:GT_ENDPOINT_REJECTED;', 'return rc;'),), 'peer-identity', 'flow'),
        ('rejected peer descriptor leaked', (('if(!configure(fd)){close(fd);return GT_ENDPOINT_REJECTED;}', 'if(!configure(fd)){return GT_ENDPOINT_REJECTED;}'),), 'peer-configuration', 'flow'),
        ('OS accept resource limits treated as fatal', (('return errno==EMFILE || errno==ENFILE || errno==ENOMEM || errno==ENOBUFS?GT_ENDPOINT_BUSY:GT_ENDPOINT_IO;', 'return GT_ENDPOINT_IO;'),), 'peer-capacity', 'flow'),

        ('world-writable ancestor admitted', ((' && !(s->st_mode&0022)', ''),), 'permissions', 'flow'),
        ('nonprivate leaf admitted', (
            ('(leaf && (st.st_uid!=e->uid || (st.st_mode&07777)!=0700))', 'false'),
            ('(leaf && (opened.st_uid!=e->uid || (opened.st_mode&07777)!=0700))', 'false'),
        ), 'permissions', 'flow'),
        ('directory symlink followed', (
            ('fstatat(dir,component,&st,AT_SYMLINK_NOFOLLOW)', 'fstatat(dir,component,&st,0)', 2),
            ('O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);\n    struct stat opened;', 'O_RDONLY|O_DIRECTORY|O_CLOEXEC);\n    struct stat opened;'),
        ), 'symlinks', 'flow'),
        ('startup lock not acquired', (('flock(lock,LOCK_EX|LOCK_NB)', '0'),), 'locks', 'peer'),
        ('startup lock blocks', (('flock(lock,LOCK_EX|LOCK_NB)', 'flock(lock,LOCK_EX)'),), 'locks', 'flow'),
        ('startup and lifetime lock conflated', (('acquire(e,&e->daemon,daemon_name)', 'acquire(e,&e->daemon,startup_name)'),), 'locks', 'peer'),
        ('directory and lock fds survive exec', (('O_CLOEXEC', '0', 3),), 'inheritance', 'flow'),
        ('accepted fd can block', (('flags|O_NONBLOCK', 'flags'),), 'flow', 'peer'),
        ('accepted fd survives exec', (('descriptor|FD_CLOEXEC', 'descriptor'),), 'flow', 'peer'),
        ('lifetime lock released after listen', (
            ('e->socket_identity=st;e->listener=fd;return GT_ENDPOINT_OK;', 'e->socket_identity=st;e->listener=fd;close(e->daemon);e->daemon=-1;return GT_ENDPOINT_OK;'),
        ), 'flow', 'peer'),
        ('live socket deleted', (
            ('if(rc==GT_ENDPOINT_OK || rc==GT_ENDPOINT_WAIT)rc=GT_ENDPOINT_BUSY;', 'if(rc==GT_ENDPOINT_OK || rc==GT_ENDPOINT_WAIT){(void)unlinkat(e->directory,socket_name,0);rc=GT_ENDPOINT_NOT_FOUND;}'),
        ), 'live', 'stale'),
        ('replacement socket deleted by old owner', (
            ('if(same(&st,&e->socket_identity) && unlinkat(e->directory,socket_name,0))', 'if(unlinkat(e->directory,socket_name,0))'),
        ), 'replacement', 'flow'),
        ('stale socket never reclaimed', (
            ('else rc=unlinkat(e->directory,socket_name,0)?GT_ENDPOINT_IO:GT_ENDPOINT_NOT_FOUND;', 'else rc=GT_ENDPOINT_BUSY;'),
        ), 'stale', 'flow'),
        ('socket permissions left at umask defaults', (
            ('fchmodat(e->directory,socket_name,0600,0)', '0'),
        ), 'flow', 'peer'),
        ('lock hard link accepted', (
            (' || node.st_nlink!=1', ''), ('node.st_nlink==1 && ', ''),
        ), 'lock-nodes', 'flow'),
        ('peer UID mismatch accepted', (('return uid==expected?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;', '(void)uid;(void)expected;return GT_ENDPOINT_OK;'),), 'peer', 'flow'),
        ('directory replacement ignored', (
            ('same(&st,&e->directory_identity)?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;', 'true?GT_ENDPOINT_OK:GT_ENDPOINT_AUTH;'),
        ), 'directory-replaced', 'flow'),
        ('endpoint escapes allocator', (('gt_unix_endpoint_t *e=a.allocate(a.context,sizeof(*e));', 'gt_unix_endpoint_t *e=malloc(sizeof(*e));'),), 'allocation', 'flow'),
        ('endpoint wrong release charge', (
            ('close(e->directory);gt_allocator_t a=e->allocator;a.release(a.context,e,sizeof(*e));', 'close(e->directory);gt_allocator_t a=e->allocator;a.release(a.context,e,sizeof(*e)-1);'),
        ), 'allocation', 'flow'),
        ('untrusted node unconditionally removed', (
            ('struct stat st;rc=socket_node(e,&st);\n  if(rc==GT_ENDPOINT_OK)', 'struct stat st;rc=socket_node(e,&st);\n  if(rc==GT_ENDPOINT_AUTH){(void)unlinkat(e->directory,socket_name,0);rc=GT_ENDPOINT_NOT_FOUND;}\n  if(rc==GT_ENDPOINT_OK)'),
        ), 'nodes', 'flow'),
    )
    try:
        restore();build()
        for mode in MODES:
            probe(mode)
        for label, edits, target, control in specs:
            text = originals['source'].decode()
            for edit in edits:
                old, new = edit[:2]
                count = edit[2] if len(edit) > 2 else 1
                if text.count(old) != count:
                    raise RuntimeError('Unix endpoint mutation anchor drift/'+label+': '+str(text.count(old)))
                text = text.replace(old, new)
            try:
                copies['source'].write_text(text+'\n/* '+MARK+' */\n')
                build();probe(target, True);probe(control)
                print('PASS Unix endpoint mutation/'+label+': target red, same-mutant control green', flush=True)
            finally:
                restore()
                shutil.rmtree(RUNS)
                RUNS.mkdir(mode=0o700)
        build()
        for mode in MODES:
            probe(mode)
        print(f'PASS {len(specs)} Unix endpoint mutations, {len(MODES)} baselines and SHA256 rollback', flush=True)
    finally:
        restore()
        shutil.rmtree(RUNS)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
