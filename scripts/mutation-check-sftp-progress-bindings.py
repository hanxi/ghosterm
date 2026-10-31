#!/usr/bin/env python3
"""Observed SFTP unavailable archive and Swift value/lifetime mapping mutations."""
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
MARK='GT_SFTP_PROGRESS_BINDING_MUTATION'

def main():
    paths=[ROOT/'core/src/sftp.c',ROOT/'apple/GhostermCore/Sources/GhostermCore/GhostermCore.swift']
    originals={p:p.read_bytes() for p in paths}
    if any(MARK.encode() in value for value in originals.values()):raise RuntimeError('Residual progress binding mutation')
    logs=ROOT/'tmp/sftp-progress-binding-mutations';logs.mkdir(parents=True,exist_ok=True)
    env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
    def run(args,label,red=None):
        result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require progress; shift; gt_timeout 240 "$@"','progress',str(ROOT),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True)
        out=result.stdout+result.stderr;(logs/(label+'.log')).write_text(out)
        valid=(result.returncode==1 and red in out) if red else result.returncode==0 and not re.search(r'(?m)^SKIP|Test Case .* skipped',out)
        if not valid:print(out,flush=True);raise RuntimeError(label+': expected '+('assertion red' if red else 'green'))
        return out
    try:
        with tempfile.TemporaryDirectory(prefix='sftp-progress-bindings-',dir=ROOT/'tmp') as folder:
            work=Path(folder);(work/'tmp').mkdir()
            shutil.copytree(ROOT/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
            source=work/'core/src/sftp.c';clean=source.read_text();no=work/'no-lib'
            run(['cmake','-S',work/'core','-B',no,'-DGHOSTERM_WITH_LIBSSH2=OFF'],'no-configure')
            probe=work/'no.c';probe.write_text('''#include "ghosterm.h"
#include <stdio.h>
static int calls;
static void observe(const gt_sftp_progress_t *p,void *c){(void)p;(void)c;calls++;}
int main(void){int token=0;uint64_t count=99;gt_sftp_create_result_t out={GT_SFTP_CREATE_COMPLETE,99};
int r=gt_sftp_receive_observed((void*)&token,"/file",0,1,&count,100,0,0,observe,&token);
int c=gt_sftp_create_observed((void*)&token,"/new",0,0,&out,100,0,0,observe,&token);
int bad=r!=GT_ERR_UNSUPPORTED||count||c!=GT_ERR_UNSUPPORTED||out.state!=GT_SFTP_CREATE_NOT_ATTEMPTED||out.bytes_acked||calls;
if(bad)puts("FAIL: unavailable observed transfer must fail and emit no progress");return bad;}
''')
            def no_build(label):
                run(['cmake','--build',no,'--clean-first','--target','test_config'],label+'-build')
                run(['cc','-std=c11','-I'+str(work/'core/include'),probe,no/'libghosterm_core.a','-lpthread','-o',work/'probe'],label+'-link')
            no_build('no-baseline');run([work/'probe'],'no-baseline')
            for kind in ('receive','create'):
                old='gt__set_error("sftp '+kind+': libssh2 unavailable");\n  return GT_ERR_UNSUPPORTED;'
                assert clean.count(old)==1
                source.write_text(clean.replace(old,old.replace('GT_ERR_UNSUPPORTED','GT_OK'))+'\n/* '+MARK+' */\n')
                no_build('no-'+kind);run([work/'probe'],'no-'+kind+'-red','FAIL: unavailable observed transfer')
                run([no/'test_config',work/'core/tests/fixtures'],'no-'+kind+'-control')
                source.write_text(clean);no_build('no-'+kind+'-restored');run([work/'probe'],'no-'+kind+'-restored')
                assert source.read_bytes()==originals[paths[0]]
                print('PASS unavailable '+kind+': actual archive / target red / config green / restored green / SHA256',flush=True)
            build=work/'core/build';run(['cmake','-S',work/'core','-B',build],'configure')
            run(['cmake','--build',build,'--target','ghosterm_core'],'core-build')
            package=work/'apple/GhostermCore'
            shutil.copytree(ROOT/'apple/GhostermCore',package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
            binding=package/'Sources/GhostermCore/GhostermCore.swift';swift_clean=binding.read_text()
            swift=['xcrun','swift','test','--package-path',package,'--scratch-path',work/'swift-build','--skip-build']
            wrapper=work/'swift';wrapper.write_text('#!/bin/bash\nset -euo pipefail\nexport GHOSTERM_SSH_HOST="$1" GHOSTERM_SSH_PORT="$2" GHOSTERM_SSH_USER="$3" GHOSTERM_SSH_KEY="$4" GHOSTERM_SSH_KNOWN_HOSTS="$5/known"\nexec '+shlex.join(map(str,[*swift,'--filter','GhostermCoreTests.testSFTP(Receive|ExclusiveCreate)EndToEnd']))+'\n');wrapper.chmod(0o700)
            def swift_build(label):run(['xcrun','swift','build','--build-tests','--package-path',package,'--scratch-path',work/'swift-build'],label+'-build')
            def swift_test(label,red=None):
                out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',wrapper],label,red)
                if 'Executed 2 tests' not in out:print(out,flush=True);raise RuntimeError('Swift progress cases did not both execute')
            swift_build('swift-baseline');swift_test('swift-baseline')
            cases=[('receive-kind','case 1: direction = .receive','case 1: direction = .create','receive progress direction and total'),
                   ('create-kind','case 2: direction = .create','case 2: direction = .receive','create progress direction and total'),
                   ('bytes','bytes: value.bytes,','bytes: value.bytes + 1,','progress actual bytes'),
                   ('total','value.total_known ? value.total_bytes : nil','nil','progress direction and total')]
            for label,old,new,message in cases:
                assert swift_clean.count(old)==1
                binding.write_text(swift_clean.replace(old,new)+'\n// '+MARK+'\n');swift_build(label)
                swift_test(label+'-red',message)
                run([*swift,'--filter','GhostermCoreTests.testConfigRoundTripIsByteIdentical'],label+'-control')
                binding.write_text(swift_clean);swift_build(label+'-restored');swift_test(label+'-restored')
                assert binding.read_bytes()==originals[paths[1]]
                print('PASS Swift '+label+': compiled assertion red / config green / restored green / SHA256',flush=True)
    finally:
        if any(path.read_bytes()!=value for path,value in originals.items()):raise RuntimeError('Production sources changed during isolated binding mutations')
    print('SFTP PROGRESS BINDING MUTATIONS PASS: 2 unavailable + 4 Swift',flush=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt(signum)
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
