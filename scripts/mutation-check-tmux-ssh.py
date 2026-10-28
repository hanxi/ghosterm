#!/usr/bin/env python3
"""SSH seam mutations. Pass test-tmux-ssh.py connection/setup options (no modes)."""
import hashlib, os, pathlib, runpy, shutil, signal, subprocess, sys, tempfile
ROOT=pathlib.Path(__file__).resolve().parent.parent
MARK='GT_TMUX_SSH_MUTATION_MARK'
FILES=['src/ssh.c','src/tmux/tmux.c','src/tmux/command.c']

def run(args, limit=180):
    p=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require tmux-ssh; shift; gt_timeout "$1" "${@:2}"','tmux-ssh',str(ROOT),str(limit)]+list(map(str,args)),cwd=ROOT,capture_output=True,text=True,env=dict(os.environ,TMPDIR=str(ROOT/'tmp')))
    return p.returncode,p.stdout+p.stderr

def main():
    original={f:(ROOT/'core'/f).read_bytes() for f in FILES};sha={f:hashlib.sha256(b).digest() for f,b in original.items()}
    if any(MARK.encode() in b for b in original.values()):raise SystemExit(2)
    def interrupt(sig,frame):raise InterruptedError(sig)
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupt)
    replace=runpy.run_path(str(ROOT/'scripts/mutation-check-tmux.py'))['replacement']
    options=sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix='tmux-ssh-mutations-',dir=ROOT/'tmp') as temp:
        folder=pathlib.Path(temp);core=folder/'core';build=folder/'build'
        shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build'))
        def command(args):
            code,out=run(args)
            if code:print(out,flush=True);raise RuntimeError('SSH mutation build/tool failed')
        command(['cmake','-S',core,'-B',build]);command(['cmake','--build',build,'-j4'])
        binary=build/'test_tmux'
        def check(modes,red=False):
            code,out=run(['python3',ROOT/'scripts/test-tmux-ssh.py','--binary',binary]+options+modes,20 if red=='timeout' else 180)
            valid=(code==124 and 'BEGIN tmux query-timeout' in out) if red=='timeout' else (code==1 and 'FAIL ' in out) if red else code==0 and all('PASS tmux '+m in out for m in modes)
            if not valid:print(out,flush=True);raise RuntimeError('SSH mutation expected '+('red' if red else 'green'))
        check(['lifecycle','attach','reset','blackhole','backpressure','lost-create','lost-terminate','query-timeout','query-cancel'])
        mutants=[
            ('PTY','src/ssh.c','if (pty) {','if (false) {',['attach']),
            ('exit-unknown','src/ssh.c','(void)exit_code; return false;','(void)exit_code; return true;',['reset']),
            ('result-unknown','src/tmux/tmux.c','r.started ? GT_TMUX_UNKNOWN : GT_TMUX_REJECTED','GT_TMUX_CONFIRMED',['lost-terminate']),
            ('cancellation','src/tmux/command.c','b->cancelled && b->cancelled(b->context)','false',['query-cancel']),
            ('deadline','src/tmux/command.c','gt__command_now() >= b->deadline','false',['query-timeout']),
            ('queued-tail','src/ssh.c','return ssh_wtail_append(sh, data + off, len - off);','return GT_OK;',['backpressure']),
        ]
        try:
            for name,file,old,new,target in mutants:
                path=core/file;path.write_text(replace(original[file].decode(),old,new)+'\n/* '+MARK+' */\n')
                for obj in build.rglob(path.name+'.o'):obj.unlink()
                (build/'libghosterm_core.a').unlink(missing_ok=True);binary.unlink(missing_ok=True)
                command(['cmake','--build',build,'-j4']);check(target,red='timeout' if name=='deadline' else True);check(['lifecycle'])
                path.write_bytes(original[file]);assert hashlib.sha256(path.read_bytes()).digest()==sha[file]
                for obj in build.rglob(path.name+'.o'):obj.unlink()
                (build/'libghosterm_core.a').unlink(missing_ok=True);binary.unlink(missing_ok=True)
                command(['cmake','--build',build,'-j4']);check(target)
                print('PASS tmux SSH '+name+': target red, control green, SHA rollback',flush=True)
        finally:
            for file,b in original.items():
                (core/file).write_bytes(b)
                assert hashlib.sha256((ROOT/'core'/file).read_bytes()).digest()==sha[file]
    print('PASS tmux SSH mutations (6 groups)')
if __name__=='__main__':main()
