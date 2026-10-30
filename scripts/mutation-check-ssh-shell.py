#!/usr/bin/env python3
"""Controlled shell ABI and Swift binding: isolated copies, actual SSH, no skips."""
import hashlib, os, re, shlex, shutil, signal, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SSH_SHELL_MUTATION'

def main():
    paths=[ROOT/'core/src/transport.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    originals={p:p.read_bytes() for p in paths}
    hashes={p:hashlib.sha256(data).digest() for p,data in originals.items()}
    if any(MARK.encode() in data for data in originals.values()):raise RuntimeError('Residual shell mutation')
    logs=ROOT/'tmp/ssh-shell-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(command,label,red=False,reason='FAIL:'):
        result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require shell; shift; gt_timeout 300 "$@"','shell',str(ROOT),*map(str,command)],cwd=ROOT,env=env,capture_output=True,text=True)
        output=result.stdout+result.stderr;(logs/(label+'.log')).write_text(output)
        good=(result.returncode==1 and reason in output) if red else (result.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',output))
        if not good:print(output,flush=True);raise RuntimeError(label+' expected '+('assertion red' if red else 'green'))
        return output
    try:
        with tempfile.TemporaryDirectory(prefix='ssh-shell-',dir=ROOT/'tmp') as directory:
            work=Path(directory);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            build=work/'core/build';source=work/'core/src/transport.c'
            run(['cmake','-S',work/'core','-B',build], 'configure')
            def compile_core():run(['cmake','--build',build,'--clean-first','--target','test_api','test_config'],'build')
            def control(label):run([build/'test_config',work/'core/tests/fixtures'],label)
            compile_core();run([build/'test_api'],'baseline');control('control')
            old='if (out_status) *out_status = GT_ERR_INVALID_ARG;'
            if source.read_text().count(old)!=1:raise RuntimeError('Ambiguous ABI anchor')
            source.write_text(source.read_text().replace(old,'if (out_status) *out_status = GT_OK;')+'\n/* '+MARK+' */\n')
            compile_core();run([build/'test_api'],'null-status',True,'FAIL: controlled shell rejects NULL with status');control('null-control')
            source.write_bytes(originals[paths[0]]);compile_core();run([build/'test_api'],'null-restored')
            print('PASS C shell status: compiled red / config green / restored green / SHA256',flush=True)
            package=work/'apple/GhostermCore'
            shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift'
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            wrapper=work/'swift-shell-fixture'
            wrapper.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known_hosts"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSSHControlledEndToEnd']))+'\n')
            wrapper.chmod(0o700)
            def compile_swift():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
            def target(label,red=False):
                output=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,'XCTAssertThrowsError failed' if 'size' not in label else 'XCTAssertTrue failed')
                if 'Executed 1 test' not in output:raise RuntimeError('Swift shell test did not execute')
            compile_swift();target('swift-baseline')
            cases=[
                ('cancel','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled','return Unmanaged<SSHCancellation>.fromOpaque(pointer).takeUnretainedValue().isCancelled && false'),
                ('budget','gt_transport_open_ssh_shell_controlled(session.handle, &size, timeoutMilliseconds,','gt_transport_open_ssh_shell_controlled(session.handle, &size, 5000,'),
                ('size','var size = gt_shell_opts_t(); size.cols = cols; size.rows = rows\n        var status = GT_OK\n        let context', 'var size = gt_shell_opts_t(); size.cols = 80; size.rows = rows\n        var status = GT_OK\n        let context'),
            ]
            for name,old,new in cases:
                clean=originals[paths[1]].decode()
                if clean.count(old)!=1:raise RuntimeError('Ambiguous Swift anchor '+name)
                binding.write_text(clean.replace(old,new)+'\n// '+MARK+'\n')
                compile_swift();target('swift-'+name,True)
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
                binding.write_bytes(originals[paths[1]])
                if hashlib.sha256(binding.read_bytes()).digest()!=hashes[paths[1]]:raise RuntimeError('Swift restore SHA')
                compile_swift();target('swift-'+name+'-restored')
                print('PASS Swift shell '+name+': compiled assertion red / config green / restored green / SHA256',flush=True)
            if hashlib.sha256(source.read_bytes()).digest()!=hashes[paths[0]]:raise RuntimeError('C restore SHA')
    finally:
        if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Production changed during isolated shell mutations')
    print('SSH SHELL MUTATIONS PASS: 1 C + 3 Swift groups',flush=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
