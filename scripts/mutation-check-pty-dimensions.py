#!/usr/bin/env python3
"""POSIX dimension admission and real kernel preservation; isolated ordinary failures."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import sys

ROOT=Path(__file__).resolve().parents[1]
WORK=ROOT/'tmp/pty-dimensions-mutations'
RUNS=WORK/'runs'
MARK='GT_PTY_DIMENSIONS_MUTATION_MARK'
NAMES=('io/pty-size.h','io/pty_posix.c','session/runtime.c','session/runtime.h')
PATHS={name:ROOT/'core/src'/name for name in NAMES}
PATHS['runtime-test.c']=ROOT/'core/tests/test_session_runtime.c'
PATHS['unit.c']=ROOT/'core/tests/test_pty_dimensions.c'
MODES={'unit':('flow','defaults','admission'),'runtime':('user-dimensions',)}


def run(args,timeout=20):
    p=subprocess.Popen(args,cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:
        out,_=p.communicate(timeout=timeout)
    except BaseException:
        if p.poll() is None:
            try:os.killpg(p.pid,signal.SIGKILL)
            except ProcessLookupError:pass
        p.wait();raise
    return p.returncode,out


def main():
    originals={name:path.read_bytes() for name,path in PATHS.items()}
    hashes={name:hashlib.sha256(data).digest() for name,data in originals.items()}
    if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.[ch]')):
        return 2
    def interrupted(signum,frame):raise RuntimeError('pty-dimensions mutation interrupted: '+str(signum))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp');os.umask(0o077);WORK.mkdir(parents=True,exist_ok=True);RUNS.mkdir(exist_ok=True)
    copies={name:WORK/name for name in PATHS}
    for path in copies.values():path.parent.mkdir(parents=True,exist_ok=True)
    def restore():
        for name,path in copies.items():
            path.write_bytes(originals[name])
            if hashlib.sha256(path.read_bytes()).digest()!=hashes[name] or PATHS[name].read_bytes()!=originals[name]:
                raise RuntimeError('pty-dimensions SHA256 rollback failed/'+name)
    rc,out=run(['bash',str(ROOT/'scripts/build-core.sh')],120)
    if rc:raise RuntimeError('pty-dimensions dependency build failed: '+out)
    archive=ROOT/'core/build/libghosterm_core.a';archive_hash=hashlib.sha256(archive.read_bytes()).digest()
    def build(suite):
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11',
            '-D_DARWIN_C_SOURCE' if sys.platform=='darwin' else '-D_POSIX_C_SOURCE=200809L',
            '-Wall','-Wextra','-Werror',
            '-I',str(WORK),'-I',str(ROOT/'core/src'),'-I',str(ROOT/'core/src/io'),'-I',str(ROOT/'core/src/session'),'-I',str(ROOT/'core/include'),
            *[str(copies[name]) for name in NAMES if name.endswith('.c')],str(copies['unit.c' if suite=='unit' else 'runtime-test.c']),str(archive),'-pthread',
            *(['-lutil'] if sys.platform.startswith('linux') else []),'-o',str(WORK/suite)])
        if rc:raise RuntimeError('pty-dimensions compilation failed: '+out)
        if hashlib.sha256(archive.read_bytes()).digest()!=archive_hash:raise RuntimeError('pty-dimensions dependency archive changed')
    def probe(suite,mode,reject=False):
        rc,out=run([str(WORK/suite),mode]+([str(RUNS)] if suite=='runtime' else []))
        if reject:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('pty-dimensions mutation lacks ordinary assertion/'+mode+': '+out)
        elif rc:raise RuntimeError('pty-dimensions baseline/control failed/'+mode+': '+out)
    specs=(
        ('open width overflow ignored','io/pty-size.h','width>UINT16_MAX','(width && false)','unit','admission','unit','defaults'),
        ('open height overflow ignored','io/pty-size.h','height>UINT16_MAX','(height && false)','unit','admission','runtime','user-readonly'),
        ('maximum width rejected','io/pty-size.h','width>UINT16_MAX','width>=UINT16_MAX','unit','flow','unit','admission'),
        ('maximum height rejected','io/pty-size.h','height>UINT16_MAX','height>=UINT16_MAX','unit','defaults','unit','admission'),
        ('open zero cells rejected','io/pty-size.h','!opening && (!cols || !rows)','(opening || !opening) && (!cols || !rows)','unit','defaults','unit','admission'),
        ('resize zero cols admitted','io/pty-size.h','(!cols || !rows)','((cols && false) || !rows)','unit','flow','unit','admission'),
        ('resize zero rows admitted','io/pty-size.h','(!cols || !rows)','(!cols || (rows && false))','unit','flow','unit','admission'),
        ('overflow misclassified','io/pty-size.h','return GT_ERR_UNSUPPORTED','return GT_ERR_INVALID_ARG','unit','flow','unit','admission'),
        ('open check too late','io/pty_posix.c','if(gt__pty_size_check(o->cols','if(false && gt__pty_size_check(o->cols','unit','admission','unit','defaults'),
        ('resize rejection ignored','io/pty_posix.c','if(size_status!=GT_OK)','if(false && size_status!=GT_OK)','unit','flow','unit','admission'),
        ('runtime queues invalid dimensions','session/runtime.c','if(rc!=GT_OK){*result=','if(false && rc!=GT_OK){*result=','runtime','user-dimensions','unit','flow'),
        ('runtime unsupported reported invalid','session/runtime.c','rc==GT_ERR_UNSUPPORTED?GT_WORKER_UNSUPPORTED:GT_WORKER_INVALID','GT_WORKER_INVALID','runtime','user-dimensions','unit','flow'),
        ('runtime zero cells reported unsupported','session/runtime.c','rc==GT_ERR_UNSUPPORTED?GT_WORKER_UNSUPPORTED:GT_WORKER_INVALID','GT_WORKER_UNSUPPORTED','runtime','user-dimensions','unit','flow'),
    )

    try:
        restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        for spec in specs:
            label,name,old,new,suite,target,control_suite,control=spec[:8];expected=spec[8] if len(spec)>8 else 1
            source=originals[name].decode()
            if source.count(old)!=expected:raise RuntimeError('pty-dimensions mutation anchor drift/'+label+': '+str(source.count(old)))
            try:
                copies[name].write_text(source.replace(old,new)+'\n/* '+MARK+' */\n')
                build(suite);probe(suite,target,True)
                if control_suite!=suite:build(control_suite)
                probe(control_suite,control)
                print('PASS pty-dimensions mutation/'+label+': target red, same-mutant control green',flush=True)
            finally:restore()
        for suite,modes in MODES.items():
            build(suite)
            for mode in modes:probe(suite,mode)
        print(f'PASS {len(specs)} pty-dimensions mutations, {sum(map(len,MODES.values()))} baselines and SHA256 rollback',flush=True)
    finally:
        restore();shutil.rmtree(RUNS)
    return 0


if __name__=='__main__':raise SystemExit(main())
