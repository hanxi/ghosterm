#!/usr/bin/env python3
"""Prove worker wake assertions against isolated C copies, never root edits."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT/'tmp/worker-wake-mutations'
MARK = 'GT_WORKER_WAKE_MUTATION_MARK'
PATHS = {'worker': ROOT/'core/src/session/worker.c', 'pty': ROOT/'core/src/io/pty_posix.c',
         'test': ROOT/'core/tests/test_worker_wake.c'}


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
            MARK in p.read_text() for p in WORK.glob('*.c')):
        return 2
    def interrupted(signum, frame):
        raise RuntimeError('wake mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    WORK.mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    result = run(['bash', str(ROOT/'scripts/build-core.sh')], cwd=ROOT,
                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
    if result.returncode:
        raise RuntimeError('core build failed: '+result.stdout)
    # Test objects precede the archive so every target uses isolated versions.
    copies = {name: WORK/(name+'.c') for name in PATHS}
    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('wake SHA256 rollback failed/'+name)
    binary = WORK/'probe'
    def build():
        result = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-D_DEFAULT_SOURCE',
            '-Wall', '-Wextra', '-Werror', '-pthread',
            *[arg for p in ('core/src', 'core/src/session', 'core/src/io', 'core/include') for arg in ('-I', str(ROOT/p))],
            *[str(copies[name]) for name in ('worker', 'pty', 'test')],
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
            raise RuntimeError('wake baseline/control failed/'+mode+': '+result.stderr)
    specs = (
        ('accepted input never wakes', 'worker', 'w->bytes += length;\n      notify_locked(w);', 'w->bytes += length;', 'input', 'pending'),
        ('stop never wakes', 'worker', 'w->stopping = true;\n  notify_locked(w);', 'w->stopping = true;', 'stop', 'pending'),
        ('queued commands sleep again', 'worker', 'w->count || w->stopping ? 0 : 10', '10', 'pending', 'input'),
        ('owner never drains notification', 'worker', 'char pending[64];ssize_t n;\n  do {n=read(w->wake[0],pending,sizeof(pending));} while(n>0 || (n<0 && errno==EINTR));', '/* no drain */', 'input', 'real'),
        ('wake descriptors inherit exec', 'worker', 'fdflags|FD_CLOEXEC', 'fdflags', 'input', 'real'),
        ('wake descriptors block', 'worker', 'flags|O_NONBLOCK', 'flags', 'input', 'real'),
        ('join leaks wake writer', 'worker', 'close(w->wake[0]);close(w->wake[1]);', 'close(w->wake[0]);', 'input', 'real'),
        ('pipe failure pretends start succeeded', 'worker', 'if (!pipe_ready || gt__thread_create', 'if (pipe_ready && gt__thread_create', 'failure', 'real'),
        ('failed pipe closes caller stdin', 'worker', 'w->wake[0]=w->wake[1]=-1;', 'w->wake[0]=w->wake[1]=0;', 'failure', 'input'),
        ('PTY ignores command fd', 'pty', 'pfds[2].fd = command_fd;', '(void)command_fd;pfds[2].fd = -1;', 'real', 'pending'),
        ('PTY consumes owner notification', 'pty', '      return GT_TRANSPORT_READ_TIMEOUT;\n    if (pr == 0)', '      {char byte;(void)read(command_fd,&byte,1);return GT_TRANSPORT_READ_TIMEOUT;}\n    if (pr == 0)', 'real', 'pending'),
        ('command wake starves ready output', 'pty', '(pfds[2].revents & POLLIN) &&\n        !(pfds[0].revents & (POLLIN | POLLHUP | POLLERR))', '(pfds[2].revents & POLLIN)', 'real', 'pending'),
    )
    try:
        restore();build()
        for mode in ('input', 'stop', 'pending', 'legacy', 'real', 'failure'):
            probe(mode)
        for label, name, old, new, target, control in specs:
            text = originals[name].decode()
            if text.count(old) != 1:
                raise RuntimeError('nonunique wake anchor/'+label)
            try:
                copies[name].write_text(text.replace(old, new)+'\n/* '+MARK+' */\n')
                build();probe(target, True);probe(control)
                print('PASS wake mutation/'+label+': target assertion red, same-mutant control green', flush=True)
            finally:
                restore()
        build()
        for mode in ('input', 'stop', 'pending', 'legacy', 'real', 'failure'):
            probe(mode)
        print('PASS 12 wake mutations, six baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
