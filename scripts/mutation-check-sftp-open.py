#!/usr/bin/env python3
"""Isolated controlled-SFTP mutations: real SSH/protocol peer, independent config, SHA restore."""
import hashlib,os,re,shlex,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SFTP_OPEN_MUTATION'

def main():
    production=[ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    originals={p:p.read_bytes() for p in production}; hashes={p:hashlib.sha256(b).digest() for p,b in originals.items()}
    if any(MARK.encode() in b for b in originals.values()):raise RuntimeError('Residual SFTP mutation')
    logs=ROOT/'tmp/sftp-open-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=False,extra=None):
        result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require sftp; shift; gt_timeout 300 "$@"','sftp',str(ROOT),*map(str,args)],cwd=ROOT,env=dict(env,**(extra or {})),capture_output=True,text=True)
        output=result.stdout+result.stderr;(logs/(label+'.log')).write_text(output)
        ok=(result.returncode==1 and ('FAIL:' in output or 'XCTAssertThrowsError failed' in output)) if red else result.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',output)
        if not ok:print(output,flush=True);raise RuntimeError(label+': expected '+('assertion red' if red else 'green'))
        return output
    try:
      with tempfile.TemporaryDirectory(prefix='sftp-open-',dir=ROOT/'tmp') as directory:
        work=Path(directory);(work/'tmp').mkdir()
        shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
        build=work/'core/build';source=work/'core/src/sftp.c';clean=source.read_bytes()
        run(['cmake','-S',work/'core','-B',build],'configure')
        def compile_c():run(['cmake','--build',build,'--clean-first','--target','test_sftp_open','test_api','test_config'],'build')
        wrapper=work/'protocol'
        wrapper.write_text('#!/bin/bash\nexec '+shlex.join([str(ROOT/'build/ssh-channel-toolchain/bin/python'),str(ROOT/'scripts/test-sftp-open.py')])+' "$@"\n');wrapper.chmod(0o700)
        def protocol(label,case='',red=False):
            result=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red,dict(GHOSTERM_SFTP_OPEN_BIN=str(build/'test_sftp_open'),SFTP_OPEN_CASE=case))
            if not red and 'SFTP OPEN PROTOCOL PASS' not in result:raise RuntimeError('Protocol suite did not run')
        def control(label):run([build/'test_config',work/'core/tests/fixtures'],label)
        compile_c();protocol('baseline');run([build/'test_api'],'api-baseline');control('baseline-control')
        cases=[
          ('null','if (status) *status = result;\n  if (!ssh || !timeout_ms)', 'if (status) *status = GT_OK;\n  if (!ssh || !timeout_ms)',None),
          ('zero','if (!ssh || !timeout_ms) {','if (!ssh) {',None),
          ('cancel','if (cancelled && cancelled(context)) return GT_ERR_CANCELLED;','if (false && cancelled && cancelled(context)) return GT_ERR_CANCELLED;','version-cancel'),
          ('budget','const int64_t deadline = mono_ms() + timeout_ms;','const int64_t deadline = mono_ms() + 5000;','version-timeout'),
          ('abort','gt__ssh_abort(ssh);','(void)ssh;','version-timeout'),
          ('io','if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {\n      result = GT_ERR_IO;', 'if (libssh2_session_last_errno(session) != LIBSSH2_ERROR_EAGAIN) {\n      result = GT_ERR_UNSUPPORTED;','subsystem-deny'),
          ('success','if (status) *status = GT_OK;\n    return out;', 'if (status) *status = GT_ERR_IO;\n    return out;','normal'),
        ]
        for name,old,new,case in cases:
          text=clean.decode();start=text.index('static gt_status_t open_status(');end=text.index('static void attrs_to_stat(',start)
          opener=text[start:end];assert opener.count(old)==1,(name,opener.count(old))
          source.write_text(text[:start]+opener.replace(old,new)+text[end:]+'\n/* '+MARK+' */\n');compile_c()
          if case is None:run([build/'test_api'],name+'-red',True)
          else:protocol(name+'-red',case,True)
          control(name+'-control');source.write_bytes(clean);compile_c()
          if case is None:run([build/'test_api'],name+'-restored')
          else:protocol(name+'-restored',case)
          assert hashlib.sha256(source.read_bytes()).digest()==hashes[production[0]]
          print('PASS SFTP '+name+': compiled target red / config green / restored green / SHA256',flush=True)
        package=work/'apple/GhostermCore'
        shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
        binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_bytes()
        swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
        swift_fixture=work/'swift-fixture';swift_fixture.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known_hosts"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTPControlledEndToEnd']))+'\n');swift_fixture.chmod(0o700)
        def compile_swift():run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],'swift-build')
        def swift_test(label,red=False):
            output=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',swift_fixture],label,red)
            if 'Executed 1 test' not in output:raise RuntimeError('Swift SFTP test did not execute')
        compile_swift();swift_test('swift-baseline')
        for name,old,new in [
          ('cancel','return request.isCancelled','return request.isCancelled && false'),
          ('budget','gt_ssh_sftp_controlled(handle, timeoutMilliseconds,','gt_ssh_sftp_controlled(handle, 3000,')]:
            text=swift_clean.decode();assert text.count(old)==1
            binding.write_text(text.replace(old,new)+'\n// '+MARK+'\n');compile_swift();swift_test('swift-'+name+'-red',True)
            run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],'swift-'+name+'-control')
            binding.write_bytes(swift_clean);compile_swift();swift_test('swift-'+name+'-restored')
            assert hashlib.sha256(binding.read_bytes()).digest()==hashes[production[1]]
            print('PASS Swift SFTP '+name+': target red / config green / restored green / SHA256',flush=True)
        # A real archive compiled without libssh2, not a mocked return value.
        unavailable=work/'unavailable';run(['cmake','-S',work/'core','-B',unavailable,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'unavailable-configure')
        probe=work/'unavailable.c';probe.write_text('''#include "ghosterm.h"
#include <stdio.h>
int main(void) {
 gt_ssh_opts_t o={.host="localhost",.username="fixture"};gt_ssh_t *s=gt_ssh_new(&o);
 if(!s)return 2;
 gt_status_t status=GT_OK;gt_sftp_t *f=gt_ssh_sftp_controlled(s,100,0,0,&status);
 int failed=f!=NULL || status!=GT_ERR_UNSUPPORTED;gt_sftp_close(f);gt_ssh_close(s);
 if(failed)puts("FAIL: unavailable SFTP status");return failed;
}
''')
        def no_library():
            run(['cmake','--build',unavailable,'--clean-first','--target','test_api'],'unavailable-build')
            run(['cc','-std=c11','-I'+str(work/'core/include'),probe,unavailable/'libghosterm_core.a','-lpthread','-o',work/'unavailable-probe'],'unavailable-link')
        no_library();run([work/'unavailable-probe'],'unavailable-baseline')
        old='if (status) *status = (!ssh || !timeout_ms) ? GT_ERR_INVALID_ARG : GT_ERR_UNSUPPORTED;'
        assert clean.decode().count(old)==1
        source.write_text(clean.decode().replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n')
        no_library();run([work/'unavailable-probe'],'unavailable-red',True);run([unavailable/'test_api'],'unavailable-control')
        source.write_bytes(clean);no_library();run([work/'unavailable-probe'],'unavailable-restored')
        assert hashlib.sha256(source.read_bytes()).digest()==hashes[production[0]]
        print('PASS SFTP unavailable: real archive / target red / API green / restored green / SHA256',flush=True)
    finally:
      if any(hashlib.sha256(p.read_bytes()).digest()!=h for p,h in hashes.items()):raise RuntimeError('Production changed during isolated SFTP mutations')
    print('SFTP OPEN MUTATIONS PASS: 7 C + 2 Swift + 1 unavailable groups',flush=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
