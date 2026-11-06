#!/usr/bin/env python3
"""Compiled managed-route mutations; normal assertions against real jump chains.

Only isolated Core copies are edited. Each mutant must compile, fail its normal
C assertion, pass an independent configuration control, then restore the full
route suite and source digest. Crash/timeout/sanitizer failure is not evidence.
"""
from pathlib import Path
import sys
import hashlib
import json
import os
import shutil
import signal
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_PLUGIN_ROUTE_MUTATION'
INPUTS=['core/src/extension/route.c','core/src/extension/route.h',
        'core/src/extension/backends/ssh_exec.c','core/src/extension/backends/ssh_exec.h',
        'core/src/ssh.c','core/src/ssh/connector.h','core/src/ssh/wire.h','core/src/internal.h',
        'core/src/extension/stream.c','core/src/extension/stream.h',
        'core/src/extension/frame.c','core/src/extension/frame.h',
        'core/tests/test_bridge_route.c','core/tests/test_bridge_ssh.c','core/CMakeLists.txt',
        'scripts/test-plugin-route.py','scripts/test-plugin-ssh.py','scripts/test-plugin-ssh-stream.py',
        'scripts/mutation-check-plugin-route.py']

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-route-mutations-',dir=ROOT/'tmp'))
    logs=work/'logs';logs.mkdir()
    hashes={name:digest(ROOT/name) for name in INPUTS}
    source=work/'source';build=work/'build'
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1',
         'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    commands=[]
    def run(label,args,timeout=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-route-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-route-mutations',str(ROOT),str(timeout),*map(str,args)],
                         cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10)
        output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output)
        commands.append(dict(label=label,returncode=p.returncode))
        if p.returncode:raise RuntimeError(label+': '+output)
        return output
    def rebuild(label):
        objects=[('ghosterm_core', 'src/ssh.c'), ('ghosterm_core', 'src/extension/route.c'), ('ghosterm_core', 'src/extension/backends/ssh_exec.c'), ('test_bridge_route', 'tests/test_bridge_route.c')];binaries=['test_bridge_route','test_config']
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_route','test_config','-j','2'])
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+name)
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in output:raise RuntimeError('Missing actual link: '+label+' '+marker)
    def suite(label,mode=None,red=False):
        args=['python3',ROOT/'scripts/test-plugin-route.py','--binary',build/'test_bridge_route']
        if mode:args+=['--case',mode]
        if red:args+=['--expect-assertion']
        output=run(label,args)
        if red and ('ASSERTION RED '+mode not in output or any(x in output for x in ('AddressSanitizer','runtime error:'))):
            raise RuntimeError(label+': missing normal C assertion red')
    def control(label):run(label+'-control',[build/'test_config',source/'tests/fixtures'])
    try:
        for name in ('core/src/extension/route.c','core/src/extension/backends/ssh_exec.c','core/src/ssh.c'):
            if MARK.encode() in (ROOT/name).read_bytes():print('Residual mutation: '+name,file=sys.stderr);raise SystemExit(2)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug',
                         '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');control('baseline')
        cases=[
            ('inner-hostkey','ssh.c','kh_result_t result=kh_check(s->kh_path,s->host,s->port,name,c->host_key);',
             'kh_result_t result=kh_check(s->kh_path,s->host,s->port,name,c->host_key); if(s->wire)result=KH_MATCH;','wrong-pin3'),
            ('unknown-hostkey','ssh.c','kh_result_t result=kh_check(s->kh_path,s->host,s->port,name,c->host_key);',
             'kh_result_t result=kh_check(s->kh_path,s->host,s->port,name,c->host_key); if(s->wire && result==KH_UNKNOWN)result=KH_MATCH;','unknown-reject'),
            ('native-inner-dial','ssh.c','c->config=*config;c->stage=CONNECT_STREAM;',
             'c->config=*config;c->stage=CONNECT_SOCKET;','echo3'),
            ('inner-authentication','ssh.c','gt_status_t error=connector_error(rc);',
             'gt_status_t error=s->wire?GT_ERR_IO:connector_error(rc);','wrong-auth3'),
            ('forward-channel','extension/backends/ssh_exec.c','if(s->forwarding)return libssh2_channel_open_ex(s->session,"direct-tcpip",12,',
             'if(s->forwarding)return libssh2_channel_open_ex(s->session,"session",7,','echo2'),
            ('forward-destination','extension/backends/ssh_exec.c','uint32_t values[4]={(uint32_t)length,target_port,9,0};',
             'uint32_t values[4]={(uint32_t)length,(uint32_t)target_port+1,9,0};','echo3'),
            ('route-deadline','extension/route.c','if(now-r->started>=r->options.total_ms)',
             'if(false)','deadline3'),
            ('route-wake','extension/route.c','while(write(r->wake[1],&byte,1)<0 && errno==EINTR){}',
             '(void)r;(void)byte;','cancel-read3'),
            ('owner-stop','extension/backends/ssh_exec.c','(s->owner_stop && atomic_load_explicit(s->owner_stop,memory_order_acquire));',
             'false;','cancel-read3'),
            ('hop-snapshot','extension/route.c','gt__ssh_connector_info(r->auth,&r->info.authentication[h]);',
             'gt__ssh_connector_info(r->auth,&r->info.authentication[0]);','echo3'),
            ('alias-ownership','extension/route.c','if(owned[j]==owned[i])return GT_ERR_INVALID_ARG;',
             '(void)j;','allocation'),
            ('route-release','extension/route.c','gt_allocator_t a=r->allocator;gt__plugin_memory_free(&a,r,sizeof(*r));',
             'gt_allocator_t a=r->allocator;(void)a;','allocation'),
            ('parent-release','ssh.c','if(gt__bridge_stream_destroy(ssh->wire)!=GT_OK)return GT_ERR_IO;',
             '/* leaked parent stream owner */','echo3'),
        ]
        results=[]
        for label,name,old,new,mode in cases:
            path=source/'src'/name;original=path.read_bytes();text=original.decode()
            if text.count(old)!=1:raise RuntimeError('Ambiguous mutation anchor: '+label)
            path.write_text(text.replace(old,new+' /* '+MARK+' */'))
            rebuild(label);suite(label+'-target',mode,True);control(label)
            path.write_bytes(original)
            if digest(path)!=hashlib.sha256(original).hexdigest():raise RuntimeError('Restore mismatch')
            rebuild(label+'-restored');suite(label+'-restored')
            results.append(label)
            print(label+': compiled C assertion red / same library config green / restored 18 route cases green / SHA256',flush=True)
        if any(digest(ROOT/name)!=sha for name,sha in hashes.items()):raise RuntimeError('Worktree changed during isolated mutation run')
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,inputs_sha256=hashes,
            commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        print('PLUGIN ROUTE MUTATIONS PASS: '+str(len(results))+' groups; logs: '+str(logs),flush=True)
    finally:
        shutil.rmtree(source,ignore_errors=True);shutil.rmtree(build,ignore_errors=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt('signal '+str(signum))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
