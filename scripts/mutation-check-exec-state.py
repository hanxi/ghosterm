#!/usr/bin/env python3
"""C/helper and real patched-dependency exit-state mutants; isolated, offline.
Normal C assertions only, independent config green and restored full cases.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import runpy
import shutil
import signal
import subprocess
import tarfile
import tempfile
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_EXEC_STATE_MUTATION'
INPUTS=['core/src/ssh.c','core/include/ghosterm/ssh.h','core/tests/test_exec_boundary.c','core/tests/test_bridge_ssh.c',
        'core/CMakeLists.txt','core/cmake/verify-ssh2.py','scripts/build-android-native.py',
        'scripts/test-plugin-ssh-stream.py','scripts/mutation-check-exec-state.py']

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--case',action='append');args=parser.parse_args()
    meta=runpy.run_path(str(ROOT/'core/cmake/verify-ssh2.py'))
    patches=[ROOT/'patches/libssh2/1.11.1'/name for name in meta['PATCH_NAMES']]
    hashes={name:sha(ROOT/name) for name in INPUTS}
    hashes.update({str(path.relative_to(ROOT)):sha(path) for path in patches})
    archive=ROOT/'tmp/host-ssh2/libssh2-1.11.1.tar.gz'
    if sha(archive)!=meta['SOURCE_SHA']:raise RuntimeError('Pinned offline source mismatch')
    original=(ROOT/'core/src/ssh.c').read_bytes()
    if MARK.encode() in original:raise RuntimeError('Residual exec mutation')
    work=Path(tempfile.mkdtemp(prefix='exec-state-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir()
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    steps=[];accepted=[]
    def run(label,command,expected=0,assertion=False):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require exec-state; shift; gt_timeout 180 "$@"'
        result=subprocess.run(['bash','-c',shell,'exec-state',str(ROOT),*map(str,command)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=190)
        output=result.stdout+result.stderr;(logs/(label+'.log')).write_text(output)
        steps.append(dict(label=label,returncode=result.returncode))
        if result.returncode!=expected or (assertion and 'FAIL ' not in output):raise RuntimeError(label+': '+output)
        if assertion and any(x in output for x in ('AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:')):raise RuntimeError(label+': sanitizer is not assertion red')
        return output
    try:
        core=work/'core';shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build','build-*'))
        stage=work/'dependency';stage.mkdir()
        with tarfile.open(archive) as tar:tar.extractall(stage,filter='data')
        dep=stage/'libssh2-1.11.1';dbuild=work/'dependency-build'
        for patch in patches:run('patch-'+patch.stem,['patch','--batch','--forward','-d',dep,'-p1','-i',patch])
        crypto=subprocess.check_output(['pkg-config','--variable=prefix','libcrypto'],text=True).strip()
        flags='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
        run('dependency-configure',['cmake','-S',dep,'-B',dbuild,'-DBUILD_SHARED_LIBS=OFF','-DBUILD_STATIC_LIBS=ON',
             '-DBUILD_TESTING=OFF','-DBUILD_EXAMPLES=OFF','-DENABLE_ZLIB_COMPRESSION=OFF','-DOPENSSL_ROOT_DIR='+crypto,'-DCMAKE_C_FLAGS='+flags])
        def dependency_build(label):run(label+'-dependency-build',['cmake','--build',dbuild,'--clean-first','-j','2'])
        dependency_build('baseline')
        wrapper=work/'wrapper';wrapper.mkdir()
        (wrapper/'CMakeLists.txt').write_text(f'''cmake_minimum_required(VERSION 3.19)
project(exec_fixture C)
find_package(OpenSSL REQUIRED)
add_library(fixture_ssh2 STATIC IMPORTED GLOBAL)
set_target_properties(fixture_ssh2 PROPERTIES IMPORTED_LOCATION [[{dbuild}/src/libssh2.a]] INTERFACE_INCLUDE_DIRECTORIES [[{dep}/include]] INTERFACE_LINK_LIBRARIES OpenSSL::Crypto)
set(GHOSTERM_SSH2_TARGET fixture_ssh2 CACHE STRING "Test-only explicit dependency")
add_subdirectory([[{core}]] core)
''')
        build=work/'build'
        run('core-configure',['cmake','-S',wrapper,'-B',build,'-DCMAKE_BUILD_TYPE=Debug','-DOPENSSL_ROOT_DIR='+crypto,'-DCMAKE_C_FLAGS='+flags])
        def rebuild(label):run(label+'-core-build',['cmake','--build',build,'--target','test_exec_boundary','test_bridge_ssh','test_config','-j','2'])
        def control(label):run(label+'-control',[build/'core/test_config',core/'tests/fixtures'])
        def network(label,mode=None,red=False):
            command=['python3',ROOT/'scripts/test-plugin-ssh-stream.py','--binary',build/'core/test_bridge_ssh','--legacy']
            if mode:command+=['--case',mode]
            if red:command+=['--expect-assertion']
            output=run(label,command)
            if red and 'ASSERTION RED '+mode not in output:raise RuntimeError(label+': missing real C assertion')
        def restored(label):
            run(label+'-boundary',[build/'core/test_exec_boundary']);network(label+'-legacy')
        rebuild('baseline');control('baseline');restored('baseline')
        core_cases=[
          ('zero-eof','bool both_eof = n < 0 && libssh2_channel_eof(e->chan);','bool both_eof = n < 0 && (a == 0 || libssh2_channel_eof(e->chan));','idle',False),
          ('confirmed-eof','bool both_eof = n < 0 && libssh2_channel_eof(e->chan);','bool both_eof = n < 0 && !hard && libssh2_channel_eof(e->chan);','eof',False),
          ('reset-eof','bool both_eof = n < 0 && libssh2_channel_eof(e->chan);','bool both_eof = n < 0 && (hard || libssh2_channel_eof(e->chan));','reset',False),
          ('stderr','ssize_t b = libssh2_channel_read_stderr(e->chan, (char *)buf, cap);','ssize_t b = libssh2_channel_read(e->chan, (char *)buf, cap);','tail',False),
          ('zero-blocking','libssh2_session_set_blocking(e->sess, 0);','/* blocking read on zero budget */','idle',False),
          ('restore-blocking','libssh2_session_set_blocking(e->sess, previous_blocking);','(void)previous_blocking;','idle',False),
          ('restore-timeout','libssh2_session_set_timeout(e->sess, previous_timeout);','(void)previous_timeout;','idle',False),
          ('unknown-output','    if (rc != 1) {\n        return false;\n','    if (rc != 1) {\n        if (exit_code) {\n            *exit_code = 0;\n        }\n        return false;\n','status',False),
          ('query-close','int rc = libssh2_ghosterm_channel_exit_status(e->chan, &value);','libssh2_channel_close(e->chan); int rc = libssh2_ghosterm_channel_exit_status(e->chan, &value);','status',False),
          ('late-state','    int64_t start = exec_mono_ms();\n',
           '    if (libssh2_channel_eof(e->chan)) {\n        return GT_EXEC_READ_EOF;\n    }\n    int64_t start = exec_mono_ms();\n','legacy-late',True),
        ]
        names={case[0] for case in core_cases}|{'presence','malformed','duplicate','range','absent-zero','stub-output'}
        if args.case and any(x not in names for x in args.case):raise RuntimeError('Unknown mutation case')
        def selected(name):return not args.case or name in args.case
        path=core/'src/ssh.c'
        for label,old,new,mode,real in core_cases:
            if not selected(label):continue
            text=original.decode()
            if text.count(old)!=1:raise RuntimeError('Ambiguous core anchor: '+label)
            path.write_text(text.replace(old,new+' /* '+MARK+' */'));rebuild(label)
            if real:network(label+'-target',mode,True)
            else:run(label+'-target',[build/'core/test_exec_boundary',mode],1,True)
            control(label);path.write_bytes(original);rebuild(label+'-restored');restored(label+'-restored')
            if sha(path)!=hashlib.sha256(original).hexdigest():raise RuntimeError('Core restore mismatch')
            accepted.append(label);print(label+': compiled assertion red / same library config green / restored boundary + 10 protocol cases green / SHA256',flush=True)
        dep_cases=[
          ('presence','src/packet.c','channelp->ghosterm_exit_status_received = 1;','channelp->ghosterm_exit_status_received = 0;','legacy-zero'),
          ('malformed','src/packet.c','if(channelp && (strlen("exit-status") + 14) <= datalen &&',
           'if(channelp && (strlen("exit-status") + 14) > datalen) { channelp->ghosterm_exit_status = 0; channelp->ghosterm_exit_status_received = 1; }\n                    if(channelp && (strlen("exit-status") + 14) <= datalen &&','legacy-malformed'),
          ('duplicate','src/packet.c','&&\n                       !channelp->ghosterm_exit_status_received', '', 'legacy-duplicate'),
          ('range','src/channel.c','if(channel->ghosterm_exit_status > INT_MAX)\n        return -1;',
           'if(channel->ghosterm_exit_status > INT_MAX) { if(status) *status = 0; return 1; }','legacy-range'),
          ('absent-zero','src/channel.c','if(!channel || !channel->ghosterm_exit_status_received)\n        return 0;',
           'if(!channel || !channel->ghosterm_exit_status_received) { if(status) *status = 0; return 1; }','legacy-missing'),
        ]
        for label,name,old,new,mode in dep_cases:
            if not selected(label):continue
            path=dep/name;before=path.read_bytes();text=before.decode()
            if text.count(old)!=1:raise RuntimeError('Ambiguous dependency anchor: '+label)
            path.write_text(text.replace(old,new+' /* '+MARK+' */'))
            dependency_build(label);rebuild(label);network(label+'-target',mode,True);control(label)
            path.write_bytes(before);dependency_build(label+'-restored');rebuild(label+'-restored');restored(label+'-restored')
            if sha(path)!=hashlib.sha256(before).hexdigest():raise RuntimeError('Dependency restore mismatch')
            accepted.append(label);print(label+': rebuilt patched dependency assertion red / same library config green / restored 10 protocol cases green / SHA256',flush=True)
        if selected('stub-output'):
            off=work/'off';run('off-configure',['cmake','-S',core,'-B',off,'-DGHOSTERM_WITH_LIBSSH2=OFF'])
            def off_build(label):run(label,['cmake','--build',off,'--target','test_bridge_ssh','test_config','-j','2'])
            off_build('off-baseline-build');run('off-baseline',[off/'test_bridge_ssh','unsupported'])
            path=core/'src/ssh.c';text=original.decode();old='    (void)code;\n    return false;\n'
            if text.count(old)!=1:raise RuntimeError('Ambiguous stub anchor')
            path.write_text(text.replace(old,'    if (code) {\n        *code = 0;\n    }\n    return false; /* GT_EXEC_STATE_MUTATION */\n'))
            off_build('off-mutant-build');run('off-target',[off/'test_bridge_ssh','unsupported'],1,True)
            run('off-control',[off/'test_config',core/'tests/fixtures']);path.write_bytes(original)
            off_build('off-restored-build');run('off-restored',[off/'test_bridge_ssh','unsupported'])
            accepted.append('stub-output');print('stub-output: actual unsupported archive assertion red / config green / restored green / SHA256',flush=True)
        for name,digest in hashes.items():
            if sha(ROOT/name)!=digest:raise RuntimeError('Worktree changed during mutation: '+name)
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=accepted,inputs_sha256=hashes,source_sha256=meta['SOURCE_SHA'],steps=steps,leak_sanitizer=False),indent=2)+'\n')
        print('EXEC STATE MUTATIONS PASS: '+str(len(accepted))+' groups; logs: '+str(logs),flush=True)
    finally:
        for name in ('core','dependency','dependency-build','build','wrapper','off'):shutil.rmtree(work/name,ignore_errors=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt('signal '+str(signum))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
