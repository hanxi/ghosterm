#!/usr/bin/env python3
"""Legacy transfer completion: compiled mutations, real IO, independent control."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_TRANSFER_MUTATION'


def main():
    source=ROOT/'core/src/sftp.c'; original=source.read_bytes()
    if MARK.encode() in original: raise RuntimeError('Residual transfer mutation')
    logs=ROOT/'tmp/sftp-transfer-mutations'; logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'))
    def run(args,label,red=None,extra=None):
        result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require transfer; shift; gt_timeout 180 "$@"',
            'transfer',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        out=result.stdout+result.stderr; (logs/(label+'.log')).write_text(out)
        okay=(result.returncode==1 and red in out) if red else result.returncode==0 and not re.search(r'(?m)^SKIP',out)
        if not okay:
            print(out,flush=True); raise RuntimeError(label+': expected '+('specific assertion red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-transfer-',dir=ROOT/'tmp') as directory:
            work=Path(directory)
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            changed=work/'core/src/sftp.c'; clean=original.decode(); build=work/'build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_probe():
                run(['cmake','--build',build,'--clean-first','--target','test_sftp_transfer','test_config'],'build')
            wrapper=work/'protocol'
            wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-transfer.py')])+' "$@"\n');wrapper.chmod(0o700)
            def protocol(case,label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,
                        dict(SFTP_TRANSFER_CASE=case,GHOSTERM_SFTP_TRANSFER_BIN=str(build/'test_sftp_transfer')))
                if not red and 'SFTP TRANSFER PASS' not in out: raise RuntimeError('Transfer probe did not execute')
            compile_probe();protocol('','baseline')
            variants=[]
            def variant(name,scope,old,new,case,assertion):
                start=clean.index(scope);end=clean.index('\n}\n',start)+3;part=clean[start:end]
                if part.count(old)!=1: raise RuntimeError(f'{name}: mutation locator count {part.count(old)}')
                variants.append((name,clean[:start]+part.replace(old,new)+clean[end:],case,'FAIL: '+assertion))
            result='transfer result matches IO and close outcomes'
            for direction in ('upload','download'):
                scope='static int job_'+direction
                variant(direction+'-remote-close',scope,'remote_close != 0 || local_close != 0','(remote_close != 0 && false) || local_close != 0',direction+'-close-deny',result)
                variant(direction+'-local-close',scope,'remote_close != 0 || local_close != 0','remote_close != 0 || (local_close != 0 && false)',direction+'-local-close',result)
                variant(direction+'-queued-cancel','static void *worker_main(void *arg) {','if (atomic_load(&j->cancelled))','if (false && atomic_load(&j->cancelled))',direction+'-cancelled','preflight failure or queued cancellation preserves target')
            variant('truncate','static int job_upload','LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC','LIBSSH2_FXF_CREAT','upload-normal','exact destination bytes with no old tail')
            variant('source-before-target','static int job_download','  int rc = GT_JOB_FAILED;','  int rc = GT_JOB_FAILED;\n  FILE *early = fopen(j->local,"wb"); if (early) fclose(early);','download-missing','preflight failure or queued cancellation preserves target')
            variant('target-open-cleanup','static int job_download','    libssh2_sftp_close(h);','    (void)h;','download-local-open','each opened remote handle is closed once')
            for name,old in [('seek-end','fseeko(f, 0, SEEK_END) != 0'),('seek-start','fseeko(f, 0, SEEK_SET) != 0'),('tell','(length = ftello(f)) < 0')]:
                variant(name,'static int job_upload',old,'('+old+' && false)','upload-'+name,result)
            variant('zero-write','static int job_upload','if (k < 0)','if (k <= 0)','upload-zero-write',result)
            for name,text,case,assertion in variants:
                changed.write_text(text+'\n/* '+MARK+' */\n');compile_probe();protocol(case,name+'-red',assertion)
                run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                changed.write_bytes(original);compile_probe();protocol(case,name+'-restored')
                assert hashlib.sha256(changed.read_bytes()).digest()==hashlib.sha256(original).digest()
                print('PASS transfer '+name+': compiled target red / config green / restored green / SHA256',flush=True)
    finally:
        if source.read_bytes()!=original: raise RuntimeError('Production changed during isolated transfer mutations')
    print('SFTP TRANSFER MUTATIONS PASS: 13 groups',flush=True)


if __name__=='__main__':
    def interrupted(signum,frame): raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
