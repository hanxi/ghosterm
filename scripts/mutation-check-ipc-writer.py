#!/usr/bin/env python3
"""Shared local/SSH writer: isolated mutations, targeted failure + control."""
import hashlib
import os
from pathlib import Path
import shlex
import signal
import subprocess

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT / 'tmp/ipc-writer-mutations'
MARK = 'GT_IPC_WRITER_MUTATION_MARK'
PATHS = {'source': ROOT/'core/src/ipc/writer.c', 'header': ROOT/'core/src/ipc/writer.h',
         'wire_source': ROOT/'core/src/ipc/wire.c', 'wire_header': ROOT/'core/src/ipc/wire.h',
         'test': ROOT/'core/tests/test_ipc_writer.c'}
MODES = ('short', 'replacement', 'invalid', 'cancel', 'empty')


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
        raise RuntimeError('IPC writer mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    (WORK/'ipc').mkdir(parents=True, exist_ok=True)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    copies = {'source': WORK/'writer.c', 'header': WORK/'ipc/writer.h',
              'wire_source': WORK/'wire.c', 'wire_header': WORK/'ipc/wire.h', 'test': WORK/'test.c'}
    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest() != hashes[name] or PATHS[name].read_bytes() != originals[name]:
                raise RuntimeError('IPC writer SHA256 rollback failed/'+name)
    def build():
        rc, output = run([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', '-Wall', '-Wextra', '-Werror',
            '-I', str(WORK), '-I', str(WORK/'ipc'), str(copies['source']), str(copies['wire_source']), str(copies['test']),
            '-o', str(WORK/'probe')])
        if rc:
            raise RuntimeError('IPC writer compilation failed: '+output)
    def probe(mode, reject=False):
        rc, output = run([str(WORK/'probe'), mode])
        if reject:
            if rc != 1 or 'FAIL ' not in output:
                raise RuntimeError('mutation lacks targeted assertion/'+mode+': '+output)
        elif rc:
            raise RuntimeError('IPC writer baseline/control failed/'+mode+': '+output)
    specs = (
        ('short write skips unsent bytes', 'w->offset+=written;', 'w->offset+=length;', 'short', 'invalid'),
        ('header handed off counts as whole frame', 'w->offset==GT_WIRE_HEADER_BYTES+w->payload_length', 'w->offset>=GT_WIRE_HEADER_BYTES', 'short', 'invalid'),
        ('pending frame replaced', 'w->state==GT_WRITER_PENDING || ', '', 'replacement', 'invalid'),
        ('cancel permits stream reuse', 'w->state=GT_WRITER_FAILED;', 'w->state=GT_WRITER_IDLE;', 'cancel', 'short'),
        ('impossible write count does not fail stream', 'if(written>length){gt__wire_writer_cancel(w);return false;}', 'if(written>length){return false;}', 'invalid', 'short'),
        ('payload points to header', 'w->payload=payload;', 'w->payload=w->envelope;', 'short', 'empty'),
        ('header bytes not retained', 'memcpy(w->envelope,header,sizeof(header));', 'memset(w->envelope,0,sizeof(header));', 'short', 'invalid'),
        ('completed borrow not released', 'w->state=GT_WRITER_DONE;w->payload=NULL;', 'w->state=GT_WRITER_DONE;', 'short', 'cancel'),
    )
    try:
        restore();build()
        for mode in MODES:
            probe(mode)
        for label, old, new, target, control in specs:
            text = originals['source'].decode()
            if text.count(old) != 1:
                raise RuntimeError('nonunique IPC writer mutation anchor/'+label)
            try:
                copies['source'].write_text(text.replace(old, new)+'\n/* '+MARK+' */\n')
                build();probe(target, True);probe(control)
                print('PASS IPC writer mutation/'+label+': target red, same-mutant control green', flush=True)
            finally:
                restore()
        build()
        for mode in MODES:
            probe(mode)
        print('PASS 8 IPC writer mutations, five baselines and SHA256 rollback', flush=True)
    finally:
        restore()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
