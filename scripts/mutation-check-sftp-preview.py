#!/usr/bin/env python3
"""Compiled real-protocol preview mutations with independent controls and SHA restore."""
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
MARK = 'GT_SFTP_PREVIEW_MUTATION'


def main():
    sources = [ROOT/'core/src/sftp.c', ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots = {p:p.read_bytes() for p in sources}
    if any(MARK.encode() in s for s in snapshots.values()): raise RuntimeError('Residual preview mutation')
    logs=ROOT/'tmp/sftp-preview-mutations'; logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None,extra=None):
        r=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require preview; shift; gt_timeout 300 "$@"',
                          'preview',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        out=r.stdout+r.stderr; (logs/(label+'.log')).write_text(out)
        okay=(r.returncode==1 and red in out) if red else r.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not okay:
            print(out,flush=True); raise RuntimeError(label+': expected '+('specific red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-preview-',dir=ROOT/'tmp') as directory:
            work=Path(directory); (work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c'; clean=source.read_text(); build=work/'core/build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_core(): run(['cmake','--build',build,'--clean-first','--target','test_sftp_preview','test_api','test_config'],'build')
            wrapper=work/'protocol'
            wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-preview.py')])+' "$@"\n'); wrapper.chmod(0o700)
            def protocol(case,label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,
                        dict(SFTP_PREVIEW_CASE=case,GHOSTERM_SFTP_PREVIEW_BIN=str(build/'test_sftp_preview')))
                if not red and 'SFTP PREVIEW PROTOCOL PASS' not in out: raise RuntimeError('Protocol not executed')
            compile_core(); protocol('','baseline'); run([build/'test_api'],'api-baseline')
            start=clean.index('int gt_sftp_preview_controlled('); end=clean.index('int gt_sftp_stat(',start)
            preview=clean[start:end]
            cases=[
                ('zero','!out_count || !timeout_ms','!out_count','normal'),
                ('budget','const int64_t deadline = mono_ms() + timeout_ms;','const int64_t deadline = mono_ms() + 5000;','preview-read-timeout'),
                ('readonly','LIBSSH2_FXF_READ, 0','LIBSSH2_FXF_READ | LIBSSH2_FXF_WRITE, 0','allow'),
                ('extra-byte','capacity + 1 - count','capacity - count','overflow'),
                ('short-file','attributes.filesize != count','false && attributes.filesize != count','shrink'),
                ('close','if (result != 0) status = GT_ERR_IO;','if (result != 0) status = GT_OK;','preview-close-deny'),
                ('handle-free','if (file) libssh2_sftp_close(file);','(void)file;','preview-read-cancel'),
                ('abort','gt__ssh_abort(sftp->ssh);','(void)sftp;','preview-close-deny'),
                ('publish','memcpy(buffer, bytes, count);','memset(buffer, 0, count);','normal'),
                ('mode-restore','libssh2_session_set_blocking(session, blocking);','(void)blocking;','normal'),
            ]
            variants=[]
            for name,old,new,case in cases:
                assert preview.count(old)==1,(name,preview.count(old))
                variants.append((name,preview.replace(old,new),case))
            split=preview.index('  SFTP_LOCK(sftp);')
            variants.append(('cancel',preview[:split]+preview[split:].replace('cancelled, context','NULL, NULL'),'late-cancel'))
            # LSTAT and FSTAT protect different moments; each guard has its own fault.
            type_guard='if (!(attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ||\n      (attributes.permissions & LIBSSH2_SFTP_S_IFMT) != LIBSSH2_SFTP_S_IFREG) {'
            size_guard='if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) && attributes.filesize > capacity) {'
            for name,old,action in [('type',type_guard,'preview-unknown-type'),('size',size_guard,'preview-oversize')]:
                assert preview.count(old)==2
                for index,stage in enumerate(('preview-lstat','preview-fstat')):
                    offset=preview.index(old) if index==0 else preview.index(old,preview.index(old)+1)
                    changed=preview[:offset]+preview[offset:].replace(old,'if (false) {',1)
                    variants.append((stage+'-'+name,changed,stage+'-'+action.removeprefix('preview-')))
            for name,changed,case in variants:
                source.write_text(clean[:start]+changed+clean[end:]+'\n/* '+MARK+' */\n'); compile_core()
                protocol(case,name+'-red','FAIL: preview')
                run([build/'test_config',work/'core/tests/fixtures'],name+'-control')
                source.write_text(clean); compile_core(); protocol(case,name+'-restored')
                assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(snapshots[sources[0]]).digest()
                print('PASS preview '+name+': compiled target red / config green / restored green / SHA256',flush=True)
            # A real no-lib archive must reject valid calls explicitly, without a fake successful empty preview.
            unavailable=work/'unavailable'
            run(['cmake','-S',work/'core','-B',unavailable,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'unavailable-configure')
            probe=work/'unavailable.c'
            probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void) { int token=0; uint8_t bytes[16]={1}; size_t n=7; int rc=gt_sftp_preview_controlled((void *)&token,"file",bytes,sizeof bytes,&n,100,NULL,NULL); int bad=rc!=GT_ERR_UNSUPPORTED || n || bytes[0]; if(bad) puts("FAIL: unavailable preview"); return bad; }\n')
            def compile_unavailable():
                run(['cmake','--build',unavailable,'--clean-first','--target','test_api'],'unavailable-build')
                run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(work/'core/include'),probe,unavailable/'libghosterm_core.a','-lpthread','-o',work/'no-lib'],'unavailable-link')
            compile_unavailable();run([work/'no-lib'],'unavailable-baseline')
            old='gt__set_error("sftp preview: libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;'; assert clean.count(old)==1
            source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n')
            compile_unavailable();run([work/'no-lib'],'unavailable-red','FAIL: unavailable preview');run([unavailable/'test_api'],'unavailable-control')
            source.write_text(clean);compile_unavailable();run([work/'no-lib'],'unavailable-restored');assert hashlib.sha256(source.read_bytes()).digest()==hashlib.sha256(snapshots[sources[0]]).digest()
            print('PASS preview unavailable: real archive / target red / API green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore'
            shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift'; swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            fixture=work/'swift-fixture'
            fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known_hosts"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPPreviewEndToEnd']))+'\n');fixture.chmod(0o700)
            def compile_swift(): run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def swift_test(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                if 'Executed 1 test' not in out:raise RuntimeError('Swift preview test not executed')
            compile_swift();swift_test('swift-baseline')
            start=swift_clean.index('    public func preview(');end=swift_clean.index('    public func browse(',start)
            preview=swift_clean[start:end]
            for name,old,new,error in [
                ('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return false','XCTAssertThrowsError failed'),
                ('budget','timeoutMilliseconds, { pointer in','3000, { pointer in','XCTAssertThrowsError failed'),
                ('limit','count: maxBytes)','count: maxBytes + 1)','XCTAssertThrowsError failed'),
                ('bytes','return Data(bytes.prefix(count))','return Data(bytes.prefix(0))','XCTAssertEqual failed')]:
                assert preview.count(old)==1
                binding.write_text(swift_clean[:start]+preview.replace(old,new)+swift_clean[end:]+'\n// '+MARK+'\n');compile_swift()
                swift_test('swift-'+name+'-red',error)
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                binding.write_text(swift_clean);compile_swift();swift_test('swift-'+name+'-restored')
                assert hashlib.sha256(binding.read_bytes()).digest()==hashlib.sha256(snapshots[sources[1]]).digest()
                print('PASS Swift preview '+name+': target red / config green / restored green / SHA256',flush=True)
    finally:
        if any(p.read_bytes()!=old for p,old in snapshots.items()):raise RuntimeError('Production changed during isolated mutations')
    print('SFTP PREVIEW MUTATIONS PASS: 15 Core + 1 unavailable + 4 Swift groups',flush=True)


if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
