#!/usr/bin/env python3
"""Compiled SSH-provider mutations in isolated copies, real SSH assertions.
Every red is C exit 1, same-library config green, restored full SSH suite green.
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
MARK='GT_PLUGIN_SSH_STREAM_MUTATION'
INPUTS=['core/src/extension/backends/ssh_exec.c','core/src/extension/backends/ssh_exec.h',
        'core/src/ssh.c','core/src/internal.h','core/src/extension/stream.c','core/src/extension/stream.h',
        'core/src/extension/frame.c','core/src/extension/frame.h','core/tests/test_bridge_ssh.c',
        'core/CMakeLists.txt','scripts/test-plugin-ssh-stream.py','scripts/mutation-check-plugin-ssh-stream.py']

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    work=Path(tempfile.mkdtemp(prefix='plugin-ssh-stream-mutations-',dir=ROOT/'tmp'))
    logs=work/'logs';logs.mkdir()
    hashes={name:digest(ROOT/name) for name in INPUTS}
    source=work/'source';build=work/'build'
    env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1',
         'UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
    commands=[]
    def run(label,args,timeout=180):
        shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-ssh-mutations; shift; budget="$1"; shift; gt_timeout "$budget" "$@"'
        p=subprocess.run(['bash','-c',shell,'plugin-ssh-mutations',str(ROOT),str(timeout),*map(str,args)],
                         cwd=ROOT,env=env,capture_output=True,text=True,timeout=timeout+10)
        output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output)
        commands.append(dict(label=label,returncode=p.returncode))
        if p.returncode:raise RuntimeError(label+': '+output)
        return output
    def rebuild(label):
        objects=[('ghosterm_core', 'src/ssh.c'), ('ghosterm_core', 'src/extension/backends/ssh_exec.c'), ('test_bridge_ssh', 'tests/test_bridge_ssh.c')];binaries=['test_bridge_ssh','test_config']
        for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
        for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
        run(label+'-build',['cmake','--build',build,'--target','test_bridge_ssh','test_config','-j','2'])
        output=(logs/(label+'-build.log')).read_text()
        for owner,name in objects:
            if 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' not in output:raise RuntimeError('Missing actual compilation: '+label+' '+name)
        for marker in ['Linking C static library libghosterm_core.a',*['Linking C executable '+name for name in binaries]]:
            if marker not in output:raise RuntimeError('Missing actual link: '+label+' '+marker)
    def suite(label,mode=None,red=False):
        args=['python3',ROOT/'scripts/test-plugin-ssh-stream.py','--binary',build/'test_bridge_ssh']
        if mode:args+=['--case',mode]
        if red:args+=['--expect-assertion']
        output=run(label,args)
        if red and ('ASSERTION RED '+mode not in output or any(x in output for x in ('AddressSanitizer','runtime error:'))):
            raise RuntimeError(label+': missing normal C assertion red')
    def control(label):run(label+'-control',[build/'test_config',source/'tests/fixtures'])
    try:
        for name in ('core/src/extension/backends/ssh_exec.c','core/src/ssh.c'):
            if MARK.encode() in (ROOT/name).read_bytes():print('Residual mutation: '+name,file=sys.stderr);raise SystemExit(2)
        shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
        run('configure',['cmake','-S',source,'-B',build,'-DCMAKE_BUILD_TYPE=Debug',
                         '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
        rebuild('baseline');suite('baseline');control('baseline')
        cases=[
            ('channel-failure-unknown','extension/backends/ssh_exec.c','if (rc == LIBSSH2_ERROR_CHANNEL_FAILURE) return GT_ERR_IO;',
             'if (rc == LIBSSH2_ERROR_CHANNEL_FAILURE) return GT_ERR_UNSUPPORTED;','channel-deny'),
            ('pty','extension/backends/ssh_exec.c','rc = libssh2_channel_exec(s->channel, s->command);',
             'libssh2_session_set_blocking(s->session, 1); (void)libssh2_channel_request_pty(s->channel, "xterm"); libssh2_session_set_blocking(s->session, 0); rc = libssh2_channel_exec(s->channel, s->command);','echo'),
            ('idle-eof','extension/backends/ssh_exec.c','if (s->rx_used == s->rx_size && libssh2_channel_eof(s->channel)) s->info.remote_eof = true;',
             'if (s->rx_used == s->rx_size) s->info.remote_eof = true;','cancel'),
            ('stderr-merge','extension/backends/ssh_exec.c','rc = libssh2_channel_exec(s->channel, s->command);',
             '(void)libssh2_channel_handle_extended_data2(s->channel, LIBSSH2_CHANNEL_EXTENDED_DATA_MERGE); rc = libssh2_channel_exec(s->channel, s->command);','diagnostic'),
            ('stderr-budget','extension/backends/ssh_exec.c','if (s->info.stderr_bytes > s->stderr_limit)',
             'if (false)','flood'),
            ('diagnostic-truncated','extension/backends/ssh_exec.c','s->info.diagnostic_truncated = s->info.stderr_bytes > s->info.diagnostic_size;',
             's->info.diagnostic_truncated = false;','diagnostic'),
            ('cancel-wake','extension/backends/ssh_exec.c','while (write(s->wake[1], &byte, 1) < 0 && errno == EINTR) {}',
             '(void)s; (void)byte;','cancel'),
            ('reset-eof','extension/backends/ssh_exec.c','if (s->wire_closed) { fail(s, GT_ERR_IO); return io(GT_BRIDGE_ERROR, 0, GT_ERR_IO); }',
             'if (s->wire_closed) { s->info.remote_eof = true; continue; }','reset'),
            ('unknown-exit-zero','extension/backends/ssh_exec.c','*out = ((const ssh_stream_t *)base)->info;',
             '*out = ((const ssh_stream_t *)base)->info; out->exit_status_known = true; out->exit_status = 0;','missing-exit'),
            ('admission-deadline','extension/backends/ssh_exec.c','if (now >= s->admission_end)',
             'if (false)','admission-deadline'),
            ('abort-before-free','extension/backends/ssh_exec.c','gt__ssh_abort(s->ssh); /* dedicated socket only, before library cleanup */',
             '/* skipped dedicated socket abort */','cancel'),
            ('allocator-release','extension/backends/ssh_exec.c','a.release(a.context, s, sizeof(*s));\n}',
             '(void)a; /* leaked owner */\n}','allocation'),
            ('reuse-terminal','ssh.c','ssh->channel_started || ssh->bridge_owned)return GT_ERR_INVALID_ARG;',
             'ssh->bridge_owned)return GT_ERR_INVALID_ARG;','isolation'),
            ('alias-exec','ssh.c','if (!ssh || !ssh->connected || !ssh->session || ssh->bridge_owned) {\n    gt__set_error("ssh_exec: 会话未连接");',
             'if (!ssh || !ssh->connected || !ssh->session) {\n    gt__set_error("ssh_exec: 会话未连接");','allocation'),
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
            print(label+': compiled C assertion red / same library config green / restored 16 SSH cases green / SHA256',flush=True)
        if any(digest(ROOT/name)!=sha for name,sha in hashes.items()):raise RuntimeError('Worktree changed during isolated mutation run')
        (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,inputs_sha256=hashes,
            commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
        print('PLUGIN SSH STREAM MUTATIONS PASS: '+str(len(results))+' groups; logs: '+str(logs),flush=True)
    finally:
        shutil.rmtree(source,ignore_errors=True);shutil.rmtree(build,ignore_errors=True)

if __name__=='__main__':
    def interrupted(signum,frame):raise KeyboardInterrupt('signal '+str(signum))
    signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
    main()
