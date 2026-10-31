#!/usr/bin/env python3
"""Exclusive-create Core/Swift mutations against real SSH and protocol peers."""
import hashlib,os,re,shlex,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_CREATE_MUTATION'

def main():
    paths=[ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots={p:p.read_bytes() for p in paths}
    if any(MARK.encode() in s for s in snapshots.values()):raise RuntimeError('Residual create mutation')
    logs=ROOT/'tmp/sftp-create-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None,extra=None):
        r=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require create; shift; gt_timeout 240 "$@"','create',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        out=r.stdout+r.stderr;(logs/(label+'.log')).write_text(out)
        valid=(r.returncode==1 and red in out) if red else r.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not valid:print(out,flush=True);raise RuntimeError(label+': expected '+('specific assertion red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-create-',dir=ROOT/'tmp') as folder:
            work=Path(folder);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c';clean=source.read_text();build=work/'core/build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_sftp_create','test_config'],'build')
            wrapper=work/'protocol';wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-create.py')])+' "$@"\n');wrapper.chmod(0o700)
            def protocol(case,label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,dict(SFTP_CREATE_CASE=case,GHOSTERM_SFTP_CREATE_BIN=str(build/'test_sftp_create')))
                if not red and 'SFTP CREATE ' not in out:raise RuntimeError('Create tests did not execute')
            compile_core();protocol('','baseline')
            start=clean.index('int gt_sftp_create_observed(');end=clean.index('int gt_sftp_receive_controlled(',start);piece=clean[start:end]
            cases=[
                ('exclusive','LIBSSH2_FXF_CREAT | LIBSSH2_FXF_EXCL','LIBSSH2_FXF_CREAT','normal'),
                ('private-mode','LIBSSH2_FXF_EXCL, 0600','LIBSSH2_FXF_EXCL, 0644','normal'),
                ('zero-budget','!out || !timeout_ms ||','!out ||','normal'),
                ('size','(uint64_t)before.st_size != expected_size','((uint64_t)before.st_size != expected_size && false)','normal'),
                ('regular','!S_ISREG(before.st_mode)','(!S_ISREG(before.st_mode) && false)','normal'),
                ('readable','(flags & O_ACCMODE) == O_WRONLY','((flags & O_ACCMODE) == O_WRONLY && false)','normal'),
                ('pread','pread(source_fd, buffer, wanted, (off_t)out->bytes_acked)','read(source_fd, buffer, wanted)','normal'),
                ('unknown','out->state = GT_SFTP_CREATE_UNKNOWN;','out->state = GT_SFTP_CREATE_REJECTED;','open-disconnect'),
                ('rejection','out->state = GT_SFTP_CREATE_REJECTED;','out->state = GT_SFTP_CREATE_UNKNOWN;','open-deny'),
                ('malformed','libssh2_sftp_last_error(sftp->sftp) != LIBSSH2_FX_OK','(libssh2_sftp_last_error(sftp->sftp) != LIBSSH2_FX_OK || true)','open-malformed-handle'),
                ('created','out->state = GT_SFTP_CREATE_CREATED;','out->state = GT_SFTP_CREATE_UNKNOWN;','write-deny'),
                ('close','status = result == 0 ? GT_OK : GT_ERR_IO;','status = result == 0 ? GT_OK : GT_OK;','close-deny'),
                ('budget','mono_ms() + timeout_ms;','mono_ms() + 20;','open-timeout'),
                ('abort','gt__ssh_abort(sftp->ssh);','(void)sftp;','close-deny'),
                ('late-cancel','finished:\n  if (status != GT_OK','finished:\n  if (status == GT_OK) status = open_status(deadline, cancelled, context);\n  if (status != GT_OK','normal'),
                ('restore','libssh2_session_set_timeout(session, timeout);','libssh2_session_set_timeout(session, timeout + 1);','normal'),
            ]
            variants=[]
            for name,old,new,case in cases:
                assert piece.count(old)==1,(name,piece.count(old));variants.append((name,piece.replace(old,new),case))
            split=piece.index('  SFTP_LOCK(sftp);')
            variants.append(('cancel',piece[:split]+piece[split:].replace('cancelled, context','NULL, NULL'),'write-cancel'))
            for name,changed,case in variants:
                source.write_text(clean[:start]+changed+clean[end:]+'\n/* '+MARK+' */\n');compile_core();protocol(case,name+'-red','FAIL: create')
                run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                source.write_text(clean);compile_core();protocol(case,name+'-restored')
                assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(snapshots[paths[0]]).digest()
                print('PASS create '+name+': compiled target red / config green / restored green / SHA256',flush=True)
            # Compile an actual unavailable archive; no successful empty result.
            no=work/'no-lib';run(['cmake','-S',work/'core','-B',no,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'no-configure')
            probe=work/'no.c';probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void){int token=0;gt_sftp_create_result_t r={GT_SFTP_CREATE_COMPLETE,99};int s=gt_sftp_create_controlled((void*)&token,"/new",0,0,&r,100,0,0);int bad=s!=GT_ERR_UNSUPPORTED||r.state!=GT_SFTP_CREATE_NOT_ATTEMPTED||r.bytes_acked;if(bad)puts("FAIL: unavailable create");return bad;}\n')
            def no_build():
                run(['cmake','--build',no,'--clean-first','--target','test_config'],'no-build')
                run(['cc','-std=c11','-I'+str(work/'core/include'),probe,no/'libghosterm_core.a','-lpthread','-o',work/'probe'],'no-link')
            no_build();run([work/'probe'],'no-baseline')
            old='gt__set_error("sftp create: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;';assert clean.count(old)==1
            source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n');no_build();run([work/'probe'],'no-red','FAIL: unavailable create');run([no/'test_config',work/'core/tests/fixtures'],'no-control')
            source.write_text(clean);no_build();run([work/'probe'],'no-restored')
            assert source.read_bytes()==snapshots[paths[0]]
            print('PASS create unavailable: real archive / target red / config green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore';shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            fixture=work/'swift';fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPExclusiveCreateEndToEnd']))+'\n');fixture.chmod(0o700)
            def swift_build():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def swift_test(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                if 'Executed 1 test' not in out:raise RuntimeError('Swift test did not execute')
            swift_build();swift_test('swift-baseline')
            start=swift_clean.index('    public func createExclusive(');end=swift_clean.index('    /// Complete raw file bytes',start);piece=swift_clean[start:end]
            for name,old,new in [('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return false'),('budget','timeoutMilliseconds, { pointer in','3000, { pointer in'),('count','bytesAcknowledged: outcome.bytes_acked','bytesAcknowledged: 0'),('outcome','state: state,','state: state == .rejected ? .unknown : state,')]:
                assert piece.count(old)==(2 if name in ("cancel","budget") else 1)
                binding.write_text(swift_clean[:start]+piece.replace(old,new)+swift_clean[end:]+'\n// '+MARK+'\n');swift_build();swift_test('swift-'+name+'-red','XCTAssertEqual failed')
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                binding.write_text(swift_clean);swift_build();swift_test('swift-'+name+'-restored');assert binding.read_bytes()==snapshots[paths[1]]
                print('PASS Swift create '+name+': target red / config green / restored green / SHA256',flush=True)
    finally:
        if any(p.read_bytes()!=s for p,s in snapshots.items()):raise RuntimeError('Production changed during create mutations')
    print('SFTP CREATE MUTATIONS PASS: 17 Core + 1 unavailable + 4 Swift',flush=True)
if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
