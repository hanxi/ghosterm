#!/usr/bin/env python3
"""Shared framed connection: isolated mutations, real endpoints and pressure."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
WORK = ROOT/'tmp/ipc-connection-mutations'
RUNS = WORK/'runs'
MARK = 'GT_IPC_CONNECTION_MUTATION_MARK'
NAMES = ('connection.c','connection.h','socket-stream.c','socket-stream.h','wire.c','wire.h',
         'writer.c','writer.h','unix-endpoint.c','unix-endpoint.h')
PATHS = {name: ROOT/'core/src/ipc'/name for name in NAMES}
PATHS['secure-random.c'] = ROOT/'core/src/base/secure-random.c'
PATHS['secure-random.h'] = ROOT/'core/src/base/secure-random.h'
PATHS['test'] = ROOT/'core/tests/test_ipc_connection.c'
MODES = ('flow','receive-deadline','send-deadline','fairness','buffers','invalid-io','cancel',
         'clock','allocation','protocol','invalid','real-flow','real-pressure','real-eof','sigpipe','hup','erasure')


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
    if any(MARK.encode() in data for data in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2
    def interrupted(signum, frame):
        raise RuntimeError('framed connection mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, interrupted)
    (WORK/'ipc').mkdir(parents=True, exist_ok=True)
    RUNS.mkdir(exist_ok=True, mode=0o700)
    os.environ['TMPDIR'] = str(ROOT/'tmp')
    copies = {name: WORK/'ipc'/name for name in NAMES}
    (WORK/'base').mkdir(exist_ok=True)
    for name in ('secure-random.c','secure-random.h'):
        copies[name] = WORK/'base'/name
    copies['test'] = WORK/'test.c'
    def restore():
        for name, path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('connection SHA256 rollback failed/'+name)
    def build():
        rc, output = run([*shlex.split(os.environ.get('CC','cc')), '-std=c11','-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['secure-random.c']),str(copies['test']),'-o',str(WORK/'probe')])
        if rc:
            raise RuntimeError('connection compilation failed: '+output)
    def probe(mode, reject=False):
        rc, output = run([str(WORK/'probe'),mode,str(RUNS)])
        if reject:
            if rc!=1 or 'FAIL ' not in output:
                raise RuntimeError('mutation lacks targeted assertion/'+mode+': '+output)
        elif rc:
            raise RuntimeError('connection baseline/control failed/'+mode+': '+output)
    specs = (
        ('terminal secrets retained','connection.c','gt__secure_erase(c->storage,(size_t)c->limits.receive_limit+c->limits.send_limit);','(void)c;', 'erasure','flow'),
        ('RX secret retained','connection.c','gt__secure_erase(c->storage,c->decoder.payload_used);','(void)c;', 'erasure','flow'),
        ('TX secret retained','connection.c','if(c->writer.state==GT_WRITER_DONE)gt__secure_erase(c->storage+c->limits.receive_limit,c->writer.payload_length);','(void)c;', 'erasure','flow'),
        ('scratch secret retained','connection.c','gt__secure_erase(c->scratch,r.length);','(void)c;', 'erasure','flow'),
        ('destroy secrets retained','connection.c','gt__secure_erase(c,bytes);','(void)bytes;', 'erasure','flow'),
        ('RX progress extends deadline','connection.c','r.kind==GT_STREAM_DATA && !c->receiving','r.kind==GT_STREAM_DATA','receive-deadline','flow'),
        ('TX progress extends deadline','connection.c','gt_stream_result_t r=c->ops.write(c->ops.context,bytes,length);','c->send_started=c->now;gt_stream_result_t r=c->ops.write(c->ops.context,bytes,length);','send-deadline','flow'),
        ('deadline exact boundary missed','connection.c','>=c->limits.frame_timeout_ms','>c->limits.frame_timeout_ms','receive-deadline','flow',2),
        ('zero timestamp means RX inactive','connection.c','c->receiving=true;c->receive_started=started;','c->receiving=c->now!=0;c->receive_started=started;','receive-deadline','flow'),
        ('late read completion accepted','connection.c','c->receive_started=started;}\n    if(!check_time(c))return c->state;','c->receive_started=started;}','receive-deadline','flow'),
        ('late final write accepted','connection.c','if(!valid_io(c,r,length) || !check_time(c))return c->state;','if(!valid_io(c,r,length))return c->state;','send-deadline','flow'),
        ('partial EOF treated as clean','connection.c','finish(c,end==GT_WIRE_END?GT_CONNECTION_END:GT_CONNECTION_PROTOCOL_ERROR,0);','(void)end;finish(c,GT_CONNECTION_END,0);','protocol','flow'),
        ('RX overwrites pending reply','connection.c','uint8_t *copy=c->storage+c->limits.receive_limit;','uint8_t *copy=c->storage;','buffers','allocation'),
        ('send copy discarded','connection.c','if(h->payload_length)memcpy(copy,payload,h->payload_length);','if(h->payload_length)memset(copy,0,h->payload_length);','flow','clock'),
        ('pending send silently replaced','connection.c','if(c->writer.state==GT_WRITER_PENDING)return GT_SEND_BUSY;','if(c->writer.state==GT_WRITER_PENDING)gt__wire_writer_init(&c->writer);','fairness','clock'),
        ('output blocks incoming messages','connection.c','if(!c->decoder.ready) {','if(!c->decoder.ready && c->writer.state!=GT_WRITER_PENDING) {','fairness','protocol'),
        ('read consumes coalesced frames','connection.c','if(wanted>sizeof(c->scratch))wanted=sizeof(c->scratch);','wanted=sizeof(c->scratch);','fairness','send-deadline'),
        ('invalid adapter counts accepted','connection.c','if((r.kind==GT_STREAM_DATA && r.length && r.length<=requested) ||','if((r.kind==GT_STREAM_DATA && r.length<=requested) ||','invalid-io','flow'),
        ('backend read error ignored','connection.c','if(r.kind==GT_STREAM_ERROR){finish(c,GT_CONNECTION_IO_ERROR,r.system_error);return c->state;}','if(r.kind==GT_STREAM_ERROR){return c->state;}','invalid-io','flow'),
        ('terminal stream not closed','connection.c','c->ops.close(c->ops.context);','(void)c->ops.context;','cancel','invalid'),
        ('stream closed repeatedly','connection.c','if(c->state!=GT_CONNECTION_OPEN)return;','if(false)return;','cancel','invalid'),
        ('clock rollback ignored','connection.c',' || now<c->now','', 'clock','flow'),
        ('allocation domain bypassed','connection.c','gt_connection_t *c=a.allocate(a.context,bytes);','gt_connection_t *c=malloc(bytes);','allocation','flow'),
        ('wrong buffer release charge','connection.c','a.release(a.context,c,bytes);\n}','a.release(a.context,c,bytes-1);\n}','allocation','flow'),
        ('constructor failure steals stream','connection.c','if(!ops->clock(ops->context,&c->now)){a.release','if(!ops->clock(ops->context,&c->now)){ops->close(ops->context);a.release','clock','flow'),
        ('socket left blocking','socket-stream.c','flags|O_NONBLOCK','flags','real-pressure','flow'),
        ('socket survives exec','socket-stream.c','descriptor|FD_CLOEXEC','descriptor','real-pressure','flow'),
        ('queued input accepted after HUP','socket-stream.c','if(p.revents&POLLHUP)return result(GT_STREAM_END,0,0);','if(false)return result(GT_STREAM_END,0,0);','hup','flow'),
        ('socket write failure reports delivery','socket-stream.c','return result(GT_STREAM_ERROR,0,errno);','return result(GT_STREAM_DATA,length,0);','sigpipe','flow'),
    )
    pipe_spec = ('SIGPIPE not suppressed','socket-stream.c',
                 'int enabled=1' if sys.platform=='darwin' else 'flags=MSG_NOSIGNAL;',
                 'int enabled=0' if sys.platform=='darwin' else 'flags=0;', 'sigpipe','flow')
    specs = (*specs,pipe_spec)
    try:
        restore();build()
        for mode in MODES:
            probe(mode)
        for spec in specs:
            label,name,old,new,target,control=spec[:6]
            expected=spec[6] if len(spec)>6 else 1
            text=originals[name].decode()
            if text.count(old)!=expected:
                raise RuntimeError('connection mutation anchor drift/'+label+': '+str(text.count(old)))
            try:
                mutated=text.replace(old,new)
                if label=='SIGPIPE not suppressed' and sys.platform=='darwin':
                    # Darwin also exposes MSG_NOSIGNAL. Disable BOTH independent
                    # protections to create the actual signal-producing fault.
                    if mutated.count('flags=MSG_NOSIGNAL;')!=1:
                        raise RuntimeError('Darwin SIGPIPE alternate guard anchor drift')
                    mutated=mutated.replace('flags=MSG_NOSIGNAL;','flags=0;')
                copies[name].write_text(mutated+'\n/* '+MARK+' */\n')
                build();probe(target,True);probe(control)
                print('PASS connection mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:
                restore();shutil.rmtree(RUNS);RUNS.mkdir(mode=0o700)
        build()
        for mode in MODES:
            probe(mode)
        print(f'PASS {len(specs)} connection mutations, seventeen baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':
    raise SystemExit(main())
