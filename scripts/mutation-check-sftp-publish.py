#!/usr/bin/env python3
"""Exclusive-publish Core/Swift mutations against real SSH and protocol peers."""
import hashlib,os,re,shlex,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_PUBLISH_MUTATION'

def main():
    paths=[ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots={p:p.read_bytes() for p in paths}
    if any(MARK.encode() in s for s in snapshots.values()):raise RuntimeError('Residual publish mutation')
    logs=ROOT/'tmp/sftp-publish-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None,extra=None):
        r=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require publish; shift; gt_timeout 240 "$@"','publish',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        out=r.stdout+r.stderr;(logs/(label+'.log')).write_text(out)
        valid=(r.returncode==1 and red in out) if red else r.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not valid:print(out,flush=True);raise RuntimeError(label+': expected '+('specific assertion red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-publish-',dir=ROOT/'tmp') as folder:
            work=Path(folder);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c';clean=source.read_text();build=work/'core/build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_sftp_publish','test_config'],'build')
            wrapper=work/'protocol';wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-publish.py')])+' "$@"\n');wrapper.chmod(0o700)
            def protocol(case,label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,dict(SFTP_PUBLISH_CASE=case,GHOSTERM_SFTP_PUBLISH_BIN=str(build/'test_sftp_publish')))
                if not red and 'SFTP PUBLISH ' not in out:raise RuntimeError('Publish tests did not execute')
            compile_core();protocol('','baseline')
            start=clean.index('static uint32_t publish_remote_error(');end=clean.index('int gt_sftp_stat(',start);piece=clean[start:end]
            cases=[
                ('zero-budget','!out || !timeout_ms','!out','normal'),
                ('jobs','if (has_jobs) return GT_ERR_INVALID_ARG;','if (has_jobs && false) return GT_ERR_INVALID_ARG;','normal'),
                ('result-reset','{GT_SFTP_PUBLISH_NOT_ATTEMPTED, 0};','{GT_SFTP_PUBLISH_PUBLISHED, 99};','normal'),
                ('same-path',' || !strcmp(source, destination)','', 'normal'),
                ('directory-size','(directory && expected_size != 0)','(directory && expected_size != 0 && false)','normal'),
                ('size-range','expected_size > INT64_MAX','(expected_size > INT64_MAX && false)','normal'),
                ('no-overwrite','(unsigned)strlen(destination), 0);','(unsigned)strlen(destination), LIBSSH2_SFTP_RENAME_OVERWRITE);','normal'),
                ('conflict','if (result == 0) { out->state = GT_SFTP_PUBLISH_CONFLICT;','if (false && result == 0) { out->state = GT_SFTP_PUBLISH_CONFLICT;','conflict'),
                ('query-error','out->remote_status != LIBSSH2_FX_NO_SUCH_FILE','false','publish-target-deny'),
                ('unknown-status','if (publish_rejected(out->remote_status))','if (out->remote_status || publish_rejected(out->remote_status))','publish-rename-unknown'),
                ('rejection','out->state = GT_SFTP_PUBLISH_REJECTED;','out->state = GT_SFTP_PUBLISH_UNKNOWN;','publish-rename-deny'),
                ('source-kind','!= kind','== kind','allow'),
                ('source-size','attributes->filesize != size','(attributes->filesize != size && false)','publish-recheck-size'),
                ('source-mtime','before->mtime != attributes->mtime','(before->mtime != attributes->mtime && false)','recheck-mtime'),
                ('source-recheck','status = publish_source(&final, directory, expected_size, &initial);','status = GT_OK;','publish-recheck-missing-type'),
                ('late-cancel','if (result == 0) { out->state = GT_SFTP_PUBLISH_PUBLISHED;','if (result == 0) { status = open_status(deadline, cancelled, context); if(status != GT_OK) goto finished; out->state = GT_SFTP_PUBLISH_PUBLISHED;','normal'),
                ('total-budget','status = publish_lstat(sftp, session, socket, destination,','deadline = mono_ms() + timeout_ms; status = publish_lstat(sftp, session, socket, destination,','total-budget'),
                ('abort','gt__ssh_abort(sftp->ssh);','(void)sftp;','publish-rename-deny'),
                ('restore','libssh2_session_set_timeout(session, timeout);','libssh2_session_set_timeout(session, timeout + 1);','allow'),
            ]
            variants=[]
            for name,old,new,case in cases:
                assert piece.count(old)==1,(name,piece.count(old));variants.append((name,piece.replace(old,new),case))
            split=piece.index('  SFTP_LOCK(sftp);')
            variants.append(('cancel',piece[:split]+piece[split:].replace('cancelled, context','NULL, NULL'),'publish-rename-cancel'))
            for name,changed,case in variants:
                source.write_text(clean[:start]+changed+clean[end:]+'\n/* '+MARK+' */\n');compile_core();protocol(case,name+'-red','FAIL:')
                run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                source.write_text(clean);compile_core();protocol(case,name+'-restored')
                assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(snapshots[paths[0]]).digest()
                print('PASS publish '+name+': compiled target red / config green / restored green / SHA256',flush=True)
            # Compile an actual unavailable archive; no successful empty result.
            no=work/'no-lib';run(['cmake','-S',work/'core','-B',no,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'no-configure')
            probe=work/'no.c';probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void){int token=0;gt_sftp_publish_result_t out={GT_SFTP_PUBLISH_PUBLISHED,99};int s=gt_sftp_publish_controlled((void*)&token,"/source","/target",0,4,&out,100,0,0);int bad=s!=GT_ERR_UNSUPPORTED||out.state!=GT_SFTP_PUBLISH_NOT_ATTEMPTED||out.remote_status;if(bad)puts("FAIL: unavailable publish");return bad;}\n')
            def no_build():
                run(['cmake','--build',no,'--clean-first','--target','test_config'],'no-build')
                run(['cc','-std=c11','-I'+str(work/'core/include'),probe,no/'libghosterm_core.a','-lpthread','-o',work/'probe'],'no-link')
            no_build();run([work/'probe'],'no-baseline')
            old='gt__set_error("sftp publish: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;';assert clean.count(old)==1
            source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n');no_build();run([work/'probe'],'no-red','FAIL: unavailable publish');run([no/'test_config',work/'core/tests/fixtures'],'no-control')
            source.write_text(clean);no_build();run([work/'probe'],'no-restored')
            assert source.read_bytes()==snapshots[paths[0]]
            print('PASS publish unavailable: real archive / target red / config green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore';shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            fixture=work/'swift';fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPPublishEndToEnd']))+'\n');fixture.chmod(0o700)
            def swift_build():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def swift_test(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                if 'Executed 1 test' not in out:raise RuntimeError('Swift test did not execute')
            swift_build();swift_test('swift-baseline')
            start=swift_clean.index('    public func publish(');end=swift_clean.index('    /// Receive into an exclusively',start);piece=swift_clean[start:end]
            for name,old,new in [('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return false'),('budget','timeoutMilliseconds, { pointer in','3000, { pointer in'),('paths','handle, from, to, directory','handle, to, from, directory'),('directory','handle, from, to, directory','handle, from, to, false'),('remote','remoteStatus: outcome.remote_status','remoteStatus: 0')]:
                assert piece.count(old)==1
                binding.write_text(swift_clean[:start]+piece.replace(old,new)+swift_clean[end:]+'\n// '+MARK+'\n');swift_build();swift_test('swift-'+name+'-red','failed')
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                binding.write_text(swift_clean);swift_build();swift_test('swift-'+name+'-restored');assert binding.read_bytes()==snapshots[paths[1]]
                print('PASS Swift publish '+name+': target red / config green / restored green / SHA256',flush=True)
    finally:
        if any(p.read_bytes()!=s for p,s in snapshots.items()):raise RuntimeError('Production changed during publish mutations')
    print('SFTP PUBLISH MUTATIONS PASS: 20 Core + 1 unavailable + 5 Swift',flush=True)
if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
