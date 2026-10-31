#!/usr/bin/env python3
"""Exclusive-receive Core/Swift mutations against real SSH and protocol peers."""
import hashlib,os,re,shlex,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_RECEIVE_MUTATION'

def main():
    paths=[ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots={p:p.read_bytes() for p in paths}
    if any(MARK.encode() in s for s in snapshots.values()):raise RuntimeError('Residual receive mutation')
    logs=ROOT/'tmp/sftp-receive-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None,extra=None):
        r=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require receive; shift; gt_timeout 240 "$@"','receive',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        out=r.stdout+r.stderr;(logs/(label+'.log')).write_text(out)
        valid=(r.returncode==1 and red in out) if red else r.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not valid:print(out,flush=True);raise RuntimeError(label+': expected '+('specific assertion red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-receive-',dir=ROOT/'tmp') as folder:
            work=Path(folder);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c';clean=source.read_text();build=work/'core/build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_sftp_receive','test_config'],'build')
            wrapper=work/'protocol';wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-receive.py')])+' "$@"\n');wrapper.chmod(0o700)
            def protocol(case,label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,dict(SFTP_RECEIVE_CASE=case,GHOSTERM_SFTP_RECEIVE_BIN=str(build/'test_sftp_receive')))
                if not red and 'SFTP RECEIVE ' not in out:raise RuntimeError('Receive tests did not execute')
            compile_core();protocol('','baseline')
            start=clean.index('static int receive_attributes(');end=clean.index('int gt_sftp_create_observed(',start);piece=clean[start:end]
            cases=[
                ('zero-budget','!out_count || !timeout_ms','!out_count','normal'),
                ('empty-limit','max_bytes > INT64_MAX || !out_count','!max_bytes || max_bytes > INT64_MAX || !out_count','normal'),
                ('append','(flags & O_APPEND)','((flags & O_APPEND) && false)','normal'),
                ('nonempty','local_before.st_size != 0','(local_before.st_size != 0 && false)','normal'),
                ('count-reset','if (out_count) *out_count = 0;','if (out_count) *out_count = 99;','normal'),
                ('offset','pwrite(destination_fd, bytes + written, (size_t)length - written, (off_t)(count + written))','(lseek(destination_fd, 0, SEEK_SET), pwrite(destination_fd, bytes + written, (size_t)length - written, (off_t)(count + written)))','normal'),
                ('kind','!= LIBSSH2_SFTP_S_IFREG','== LIBSSH2_SFTP_S_IFREG','allow'),
                ('open-read','path, LIBSSH2_FXF_READ, 0','path, LIBSSH2_FXF_WRITE, 0','allow'),
                ('bound','(uint64_t)length > remaining','((uint64_t)length > remaining && false)','overflow'),
                ('early-eof','initial.filesize != count','(initial.filesize != count && false)','shrink'),
                ('source-size','before->filesize != a->filesize','(before->filesize != a->filesize && false)','receive-fstat-changed-size'),
                ('source-mtime','before->mtime != a->mtime','(before->mtime != a->mtime && false)','receive-finalstat-changed-mtime'),
                ('missing-size','if ((before->flags & LIBSSH2_SFTP_ATTR_SIZE) &&','if (false && (before->flags & LIBSSH2_SFTP_ATTR_SIZE) &&','receive-finalstat-missing-size'),
                ('local-size','(uint64_t)local_after.st_size != count','((uint64_t)local_after.st_size != count && false)','normal'),
                ('close','if (result != 0) status = GT_ERR_IO;','if (result != 0) status = GT_OK;','receive-close-deny'),
                ('budget','mono_ms() + timeout_ms;','mono_ms() + 20;','receive-open-timeout'),
                ('abort','gt__ssh_abort(sftp->ssh);','(void)sftp;','receive-close-deny'),
                ('restore','libssh2_session_set_timeout(session, timeout);','libssh2_session_set_timeout(session, timeout + 1);','allow'),
                ('short-write','written += (size_t)n;','written = (size_t)length;','normal'),
                ('eintr','if (n < 0 && errno == EINTR) continue;','if (n < 0 && errno == EINTR) { status = GT_ERR_IO; goto finished; }','normal'),
            ]
            variants=[]
            for name,old,new,case in cases:
                assert piece.count(old)==1,(name,piece.count(old));variants.append((name,piece.replace(old,new),case))
            split=piece.index('  SFTP_LOCK(sftp);')
            variants.append(('cancel',piece[:split]+piece[split:].replace('cancelled, context','NULL, NULL'),'receive-read-cancel'))
            for name,changed,case in variants:
                source.write_text(clean[:start]+changed+clean[end:]+'\n/* '+MARK+' */\n');compile_core();protocol(case,name+'-red','FAIL:')
                run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                source.write_text(clean);compile_core();protocol(case,name+'-restored')
                assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(snapshots[paths[0]]).digest()
                print('PASS receive '+name+': compiled target red / config green / restored green / SHA256',flush=True)
            # Compile an actual unavailable archive; no successful empty result.
            no=work/'no-lib';run(['cmake','-S',work/'core','-B',no,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'no-configure')
            probe=work/'no.c';probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void){int token=0;uint64_t n=99;int s=gt_sftp_receive_controlled((void*)&token,"/file",0,1,&n,100,0,0);int bad=s!=GT_ERR_UNSUPPORTED||n;if(bad)puts("FAIL: unavailable receive");return bad;}\n')
            def no_build():
                run(['cmake','--build',no,'--clean-first','--target','test_config'],'no-build')
                run(['cc','-std=c11','-I'+str(work/'core/include'),probe,no/'libghosterm_core.a','-lpthread','-o',work/'probe'],'no-link')
            no_build();run([work/'probe'],'no-baseline')
            old='gt__set_error("sftp receive: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;';assert clean.count(old)==1
            source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n');no_build();run([work/'probe'],'no-red','FAIL: unavailable receive');run([no/'test_config',work/'core/tests/fixtures'],'no-control')
            source.write_text(clean);no_build();run([work/'probe'],'no-restored')
            assert source.read_bytes()==snapshots[paths[0]]
            print('PASS receive unavailable: real archive / target red / config green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore';shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            fixture=work/'swift';fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPReceiveEndToEnd']))+'\n');fixture.chmod(0o700)
            def swift_build():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def swift_test(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                if 'Executed 1 test' not in out:raise RuntimeError('Swift test did not execute')
            swift_build();swift_test('swift-baseline')
            start=swift_clean.index('    public func receive(');end=swift_clean.index('    /// Exclusive remote staging creation',start);piece=swift_clean[start:end]
            for name,old,new in [('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return false'),('budget','timeoutMilliseconds, { pointer in','3000, { pointer in'),('count','return count','return 0'),('status','try check(status)','_ = status')]:
                assert piece.count(old)==(2 if name in ("cancel","budget") else 1)
                binding.write_text(swift_clean[:start]+piece.replace(old,new)+swift_clean[end:]+'\n// '+MARK+'\n');swift_build();swift_test('swift-'+name+'-red','failed')
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                binding.write_text(swift_clean);swift_build();swift_test('swift-'+name+'-restored');assert binding.read_bytes()==snapshots[paths[1]]
                print('PASS Swift receive '+name+': target red / config green / restored green / SHA256',flush=True)
    finally:
        if any(p.read_bytes()!=s for p,s in snapshots.items()):raise RuntimeError('Production changed during receive mutations')
    print('SFTP RECEIVE MUTATIONS PASS: 21 Core + 1 unavailable + 4 Swift',flush=True)
if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
