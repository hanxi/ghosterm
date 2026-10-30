#!/usr/bin/env python3
"""Public SSH abort state, unavailable branch and Swift binding with independent controls."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import signal
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SSH_ABORT_MUTATION'

def main():
    sources=[ROOT/'core/src/ssh.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    snapshots={p:p.read_bytes() for p in sources}
    if any(MARK.encode() in s for s in snapshots.values()):raise RuntimeError('Residual abort mutation')
    logs=ROOT/'tmp/ssh-abort-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None):
        result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require abort; shift; gt_timeout 180 "$@"','abort',str(ROOT),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True)
        out=result.stdout+result.stderr;(logs/(label+'.log')).write_text(out)
        okay=(result.returncode==1 and red in out) if red else result.returncode==0 and 'SKIP' not in out and ' skipped ' not in out
        if not okay:print(out,flush=True);raise RuntimeError(label+': expected '+('specific red' if red else 'green'))
        return out
    def same(path,original):
        assert hashlib.sha256(path.read_bytes()).digest()==hashlib.sha256(original).digest()
    try:
        with tempfile.TemporaryDirectory(prefix='ssh-abort-',dir=ROOT/'tmp') as directory:
            work=Path(directory);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/ssh.c';clean=source.read_text();build=work/'core/build'
            run(['cmake','-S',work/'core','-B',build],'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_sftp_preview','test_config'],'build')
            def check(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',build/'test_sftp_preview'],label,red)
                if not red and 'SFTP PREVIEW PASS' not in out:raise RuntimeError('Real abort test did not run')
            compile_core();check('baseline')
            start=clean.index('int gt_ssh_abort(');end=clean.index('void gt__ssh_abort(',start)
            body=clean[start:end];old='ssh->connected = false;';assert body.count(old)==1
            source.write_text(clean[:start]+body.replace(old,'ssh->connected = true;')+clean[end:]+'\n/* '+MARK+' */\n')
            compile_core();check('state-red','FAIL: preview owner abort invalidates connection')
            run([build/'test_config',work/'core/tests/fixtures'],'state-control')
            source.write_text(clean);compile_core();check('state-restored');same(source,snapshots[sources[0]])
            print('PASS SSH abort state: compiled red / config green / restored green / SHA256',flush=True)
            unavailable=work/'unavailable'
            run(['cmake','-S',work/'core','-B',unavailable,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'unavailable-configure')
            probe=work/'unavailable.c';probe.write_text('#include "ghosterm.h"\n#include <stdio.h>\nint main(void){int token=0;int bad=gt_ssh_abort((void *)&token)!=GT_ERR_UNSUPPORTED;if(bad)puts("FAIL: abort unavailable");return bad;}\n')
            def no_library():
                run(['cmake','--build',unavailable,'--clean-first','--target','test_api'],'unavailable-build')
                run(['cc','-std=c11','-Wall','-Wextra','-Werror','-I'+str(work/'core/include'),probe,unavailable/'libghosterm_core.a','-lpthread','-o',work/'no-library'],'unavailable-link')
            no_library();run([work/'no-library'],'unavailable-baseline')
            old='int gt_ssh_abort(gt_ssh_t *ssh) { return ssh ? GT_ERR_UNSUPPORTED : GT_ERR_INVALID_ARG; }';assert clean.count(old)==1
            source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n')
            no_library();run([work/'no-library'],'unavailable-red','FAIL: abort unavailable');run([unavailable/'test_api'],'unavailable-control')
            source.write_text(clean);no_library();run([work/'no-library'],'unavailable-restored');same(source,snapshots[sources[0]])
            print('PASS SSH abort unavailable: real archive / target red / API green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore';shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            fixture=work/'swift-fixture';fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known_hosts"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSSHAbortStopsOwnedSFTP']))+'\n');fixture.chmod(0o700)
            def compile_swift():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def check_swift(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',fixture],label,red)
                if 'Executed 1 test' not in out:raise RuntimeError('Swift abort test did not run')
            compile_swift();check_swift('swift-baseline')
            old='try check(gt_ssh_abort(handle))';assert swift_clean.count(old)==1
            binding.write_text(swift_clean.replace(old,'try check(0)')+'\n// '+MARK+'\n');compile_swift();check_swift('swift-red','XCTAssertThrowsError failed')
            run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-control')
            binding.write_text(swift_clean);compile_swift();check_swift('swift-restored');same(binding,snapshots[sources[1]])
            print('PASS Swift SSH abort: target red / config green / restored green / SHA256',flush=True)
    finally:
        if any(p.read_bytes()!=old for p,old in snapshots.items()):raise RuntimeError('Production changed during isolated abort validation')
    print('SSH ABORT MUTATIONS PASS: 1 Core + 1 unavailable + 1 Swift groups',flush=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
