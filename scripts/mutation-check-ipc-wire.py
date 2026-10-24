#!/usr/bin/env python3
"""Shared local/SSH envelope: isolated mutations, targeted failure + control."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/ipc-wire-mutations'
MARK = 'GT_IPC_WIRE_MUTATION_MARK'
PATHS = {'source': ROOT/'core/src/ipc/wire.c', 'header': ROOT/'core/src/ipc/wire.h',
         'test': ROOT/'core/tests/test_ipc_wire.c'}
MODES = ('golden', 'flow', 'malformed', 'limits', 'eof', 'invalid', 'empty', 'binary')


def run(args):
    process = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, start_new_session=True)
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
        raise RuntimeError('IPC wire mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    (WORK/'ipc').mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    copies = {'source': WORK/'wire.c', 'header': WORK/'ipc/wire.h', 'test': WORK/'test.c'}
    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('IPC wire SHA256 rollback failed/'+name)
    def build():
        rc, output = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(WORK), '-I', str(WORK/'ipc'), str(copies['source']), str(copies['test']),
            '-o', str(WORK/'probe')])
        if rc:
            raise RuntimeError('IPC wire compilation failed: '+output)
    def probe(mode, reject=False):
        rc, output = run([str(WORK/'probe'), mode])
        if reject:
            if rc != 1 or 'FAIL ' not in output:
                raise RuntimeError('mutation lacks targeted assertion/'+mode+': '+output)
        elif rc:
            raise RuntimeError('IPC wire baseline/control failed/'+mode+': '+output)
    specs = (
        ('invalid magic accepted', 'memcmp(in,"GTSD",4) || ', '', 'malformed', 'flow'),
        ('wrong version accepted', 'get16(in+4)!=GT_WIRE_VERSION', 'false', 'malformed', 'flow'),
        ('reserved flags accepted', '!h.type || h.flags || ', '!h.type || ', 'malformed', 'flow'),
        ('zero message type accepted', '!h.type || h.flags || ', 'h.flags || ', 'malformed', 'flow'),
        ('per-stream limit bypassed', 'h.payload_length>limit', 'h.payload_length>GT_WIRE_MAX_PAYLOAD', 'limits', 'flow'),
        ('small storage admitted', 'capacity<limit || ', '', 'invalid', 'flow'),
        ('payload never copied', 'if(n)memcpy(d->storage+d->payload_used,p,n);', '(void)p;', 'binary', 'golden'),
        ('request ID wrong endian', 'v>>(56-8*i)', 'v>>(8*i)', 'golden', 'malformed'),
        ('truncated EOF accepted', 'if(d->envelope_used || d->payload_used)return fail(d);', 'if(false)return fail(d);', 'eof', 'flow'),
        ('parse failure not sticky', 'if(d)d->failed=true;', '(void)d;', 'malformed', 'flow'),
        ('following frame consumed', 'd->payload_used+=n;*consumed+=n;', 'd->payload_used+=n;*consumed+=length;', 'flow', 'golden'),
        ('frame cannot advance', 'd->ready=false;d->envelope_used=0;', 'd->ready=true;d->envelope_used=0;', 'flow', 'golden'),
    )
    try:
        restore();build()
        for mode in MODES:
            probe(mode)
        for label, old, new, target, control in specs:
            text = originals['source'].decode()
            if text.count(old) != 1:
                raise RuntimeError('nonunique IPC wire mutation anchor/'+label)
            try:
                copies['source'].write_text(text.replace(old, new)+'\n/* '+MARK+' */\n')
                build();probe(target, True);probe(control)
                print('PASS IPC wire mutation/'+label+': target red, same-mutant control green', flush=True)
            finally:
                restore()
        build()
        for mode in MODES:
            probe(mode)
        print('PASS 12 IPC wire mutations, eight baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
