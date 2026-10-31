#!/usr/bin/env python3
"""Controlled directory Core/Swift mutations against real SSH and protocol peers."""
import hashlib,os,re,shlex,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_MKDIR_MUTATION'

def main():
    paths=[ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots={p:p.read_bytes() for p in paths}
    if any(MARK.encode() in s for s in snapshots.values()):raise RuntimeError('Residual mkdir mutation')
    logs=ROOT/'tmp/sftp-mkdir-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None,extra=None):
        r=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require mkdir; shift; gt_timeout 240 "$@"','mkdir',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        out=r.stdout+r.stderr;(logs/(label+'.log')).write_text(out)
        valid=(r.returncode==1 and red in out) if red else r.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not valid:print(out,flush=True);raise RuntimeError(label+': expected '+('specific assertion red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-mkdir-',dir=ROOT/'tmp') as folder:
            work=Path(folder);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c';clean=source.read_text();build=work/'core/build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_sftp_mkdir','test_config'],'build')
            wrapper=work/'protocol';wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-mkdir.py')])+' "$@"\n');wrapper.chmod(0o700)
            def protocol(case,label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,dict(SFTP_MKDIR_CASE=case,GHOSTERM_SFTP_MKDIR_BIN=str(build/'test_sftp_mkdir')))
                if not red and 'SFTP MKDIR ' not in out:raise RuntimeError('Mkdir tests did not execute')
            compile_core();protocol('','baseline')
            start=clean.index('/* Validate before any IO;');end=clean.index('int gt_sftp_stat(',start);piece=clean[start:end]
            cases=[
                ('zero-budget','!out || !timeout_ms','!out','normal'),
                ('jobs','if (has_jobs) return GT_ERR_INVALID_ARG;','if (has_jobs && false) return GT_ERR_INVALID_ARG;','normal'),
                ('reset','{GT_SFTP_MKDIR_NOT_ATTEMPTED, 0};','{GT_SFTP_MKDIR_COMPLETE, 99};','normal'),
                ('parent','!mkdir_parent(parent)','(!mkdir_parent(parent) && false)','normal'),
                ('name', "strchr(name, '/')", "(strchr(name, '/') && false)", 'normal'),
                ('name-limit','strlen(name) > 255','(strlen(name) > 255 && false)','normal'),
                ('path-limit','strlen(parent) + strlen(name) + (parent[1] ? 1 : 0) >= 65536','(strlen(parent) + strlen(name) + (parent[1] ? 1 : 0) >= 65536 && false)','normal'),
                ('root','if (parent[1]) strcat(path, "/");','if (true) strcat(path, "/");','root'),
                ('mode','(unsigned)strlen(path), 0700);','(unsigned)strlen(path), 0755);','normal'),
                ('conflict','if (result == 0) { out->state = GT_SFTP_MKDIR_CONFLICT;','if (false && result == 0) { out->state = GT_SFTP_MKDIR_CONFLICT;','conflict'),
                ('absence','out->remote_status != LIBSSH2_FX_NO_SUCH_FILE','false','mkdir-target-deny'),
                ('unknown','if (publish_rejected(out->remote_status))','if (out->remote_status || publish_rejected(out->remote_status))','mkdir-create-unknown'),
                ('reject','out->state = GT_SFTP_MKDIR_REJECTED;','out->state = GT_SFTP_MKDIR_UNKNOWN;','mkdir-create-deny'),
                ('ack','out->state = GT_SFTP_MKDIR_CREATED;','out->state = GT_SFTP_MKDIR_UNKNOWN;','normal'),
                ('canonical','return strcmp(canonical, path) ? GT_ERR_IO : GT_OK;','return GT_OK;','mkdir-child-real-alias'),
                ('nul', "memchr(canonical, '\\0', (size_t)result)", "(memchr(canonical, '\\0', (size_t)result) && false)", 'mkdir-child-real-nul'),
                ('empty','if (!dot || (dots & dot))','if (false && (!dot || (dots & dot)))','nonempty'),
                ('dots','if (!dot || (dots & dot))','if (!dot || ((dots & dot) && false))','duplicate-dot'),
                ('late-cancel','else { out->state = GT_SFTP_MKDIR_COMPLETE; status = GT_OK; }','else { status = open_status(deadline, cancelled, context); if(status != GT_OK) goto finished; out->state = GT_SFTP_MKDIR_COMPLETE; status = GT_OK; }','normal'),
                ('abort','gt__ssh_abort(sftp->ssh);','(void)sftp;','mkdir-create-deny'),
                ('restore','libssh2_session_set_timeout(session, timeout);','libssh2_session_set_timeout(session, timeout + 1);','allow'),
            ]
            # Distinct pre/post type validation, close acknowledgement and total budget.
            parent_check='status = publish_source(&attributes, true, 0, NULL);'
            assert piece.count(parent_check)==2
            for label,offset,case in [('parent-type',0,'mkdir-parent-missing-type'),('child-type',1,'mkdir-child-missing-type')]:
                position=piece.index(parent_check) if offset==0 else piece.rindex(parent_check)
                changed=piece[:position]+piece[position:].replace(parent_check,'status = GT_OK;',1)
                cases.append((label,piece,changed,case))
            close_check='if (result != 0) { out->remote_status = publish_remote_error(sftp, result); status = GT_ERR_IO; }'
            cases.append(('close',close_check,close_check.replace('result != 0','result != 0 && false'),'mkdir-close-deny'))
            cases.append(('total-budget',piece,piece.replace('const int64_t deadline','int64_t deadline').replace('status = mkdir_realpath(sftp, session, socket, parent,','deadline = mono_ms() + timeout_ms; status = mkdir_realpath(sftp, session, socket, parent,'),'total-budget'))
            variants=[]
            for name,old,new,case in cases:
                assert piece.count(old)==1,(name,piece.count(old));variants.append((name,piece.replace(old,new),case))
            split=piece.index('  SFTP_LOCK(sftp);')
            variants.append(('cancel',piece[:split]+piece[split:].replace('cancelled, context','NULL, NULL'),'mkdir-create-cancel'))
            for name,changed,case in variants:
                source.write_text(clean[:start]+changed+clean[end:]+'\n/* '+MARK+' */\n');compile_core();protocol(case,name+'-red','FAIL:')
                run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                source.write_text(clean);compile_core();protocol(case,name+'-restored')
                assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(snapshots[paths[0]]).digest()
                print('PASS mkdir '+name+': compiled target red / config green / restored green / SHA256',flush=True)
            # Compile an actual unavailable archive; no successful empty result.
            no=work/'no-lib';run(['cmake','-S',work/'core','-B',no,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'no-configure')
            probe=work/'no.c';probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void){int token=0;gt_sftp_mkdir_result_t out={GT_SFTP_MKDIR_COMPLETE,99};int s=gt_sftp_mkdir_controlled((void*)&token,"/parent","child",&out,100,0,0);int bad=s!=GT_ERR_UNSUPPORTED||out.state!=GT_SFTP_MKDIR_NOT_ATTEMPTED||out.remote_status;if(bad)puts("FAIL: unavailable mkdir");return bad;}\n')
            def no_build():
                run(['cmake','--build',no,'--clean-first','--target','test_config'],'no-build')
                run(['cc','-std=c11','-I'+str(work/'core/include'),probe,no/'libghosterm_core.a','-lpthread','-o',work/'probe'],'no-link')
            no_build();run([work/'probe'],'no-baseline')
            old='gt__set_error("sftp mkdir controlled: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;';assert clean.count(old)==1
            source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n');no_build();run([work/'probe'],'no-red','FAIL: unavailable mkdir');run([no/'test_config',work/'core/tests/fixtures'],'no-control')
            source.write_text(clean);no_build();run([work/'probe'],'no-restored')
            assert source.read_bytes()==snapshots[paths[0]]
            print('PASS mkdir unavailable: real archive / target red / config green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore';shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            fixture=work/'swift';fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPDirectoryCreationEndToEnd']))+'\n');fixture.chmod(0o700)
            def swift_build():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def swift_test(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                if 'Executed 1 test' not in out:raise RuntimeError('Swift test did not execute')
            swift_build();swift_test('swift-baseline')
            start=swift_clean.index('    public func createDirectory(parent:');end=swift_clean.index('    /// Publish a complete task-owned',start);piece=swift_clean[start:end]
            for name,old,new in [('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return false'),('budget','&outcome, timeoutMilliseconds,','&outcome, 3000,'),('paths','handle, directory, leaf,','handle, leaf, directory,'),('state','state: state, remoteStatus:','state: .unknown, remoteStatus:'),('remote','remoteStatus: outcome.remote_status','remoteStatus: 0')]:
                assert piece.count(old)==1
                binding.write_text(swift_clean[:start]+piece.replace(old,new)+swift_clean[end:]+'\n// '+MARK+'\n');swift_build();swift_test('swift-'+name+'-red','failed')
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                binding.write_text(swift_clean);swift_build();swift_test('swift-'+name+'-restored');assert binding.read_bytes()==snapshots[paths[1]]
                print('PASS Swift mkdir '+name+': target red / config green / restored green / SHA256',flush=True)
    finally:
        if any(p.read_bytes()!=s for p,s in snapshots.items()):raise RuntimeError('Production changed during mkdir mutations')
    print('SFTP MKDIR MUTATIONS PASS: 26 Core + 1 unavailable + 5 Swift',flush=True)
if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
