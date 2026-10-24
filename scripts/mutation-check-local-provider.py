#!/usr/bin/env python3
"""Local proxy ownership, isolation and typed errors: ordinary red + green control."""
import hashlib,os,shlex,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];SOURCE=ROOT/'core/src/ipc/local-session-provider.c';WORK=ROOT/'tmp/local-provider-mutations';MARK='GT_LOCAL_PROVIDER_MUTATION_MARK'
def run(args,timeout=25):
    p=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
    try:return communicate(p,timeout)
    finally:
        try:os.killpg(p.pid,signal.SIGKILL)
        except ProcessLookupError:pass
        p.wait()
def communicate(p,timeout):
    out,_=p.communicate(timeout=timeout);return p.returncode,out
def main():
    WORK.mkdir(parents=True,exist_ok=True);copy=WORK/'provider.c';original=SOURCE.read_bytes();digest=hashlib.sha256(original).digest()
    if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
    def interrupted(sig,frame):raise RuntimeError('local provider mutation interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    os.environ['TMPDIR']=str(ROOT/'tmp')
    def restore():
        copy.write_bytes(original)
        if hashlib.sha256(copy.read_bytes()).digest()!=digest or SOURCE.read_bytes()!=original:raise RuntimeError('local provider SHA256 rollback failed')
    def build():
        rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Icore/src','-Icore/src/ipc','-Icore/include',ROOT/'core/tests/test_local_provider.c',copy,ROOT/'core/build/libghosterm_core.a','-pthread','-o',WORK/'probe'])
        if rc:raise RuntimeError('compile failed: '+out)
    def probe(case,red=False):
        rc,out=run([WORK/'probe',case,ROOT/'core/CMakeLists.txt'])
        if red:
            if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+case+': '+out)
        elif rc:raise RuntimeError('local provider baseline/control failed/'+case+': '+out)
    specs=[
      ('stderr not drained','ssize_t n = read(c->diagnostic, bytes, sizeof(bytes));','ssize_t n = 0; (void)bytes;','binary'),
      ('roles collapsed','slot ? "data" : "management"','slot ? "management" : "management"','binary'),
      ('proxy argv not owned','p->config.proxy = p->proxy;','p->config.proxy = p->proxy; strcpy(p->proxy,"/ghosterm-fixture-no-such-helper");','binary'),
      ('typed rejection ignored','gt__proxy_exit_status(WEXITSTATUS(status), &error)','false && gt__proxy_exit_status(WEXITSTATUS(status), &error)','status'),
      ('clock fault accepted','if(p->clock_fault', 'if(false && p->clock_fault','clock'),
      ('proxy escalation omitted','(void)kill(c->pid, SIGKILL);','(void)0;','cancel'),
      ('child reaping omitted','pid_t result = waitpid(c->pid, &status, WNOHANG);','pid_t result = 0; (void)status;','binary'),
      ('missing helper hidden','rc == ENOENT   ? GT_ERR_NOT_FOUND','rc == ENOENT   ? GT_ERR_IO','missing'),
      ('unsafe relative runtime accepted',"!s || s[0] != '/' || strlen(s) > 1024","!s || strlen(s) > 1024",'validation'),
      ('allocation errors disguised','if (!p)\n    return GT_ERR_IO;','if (!p)\n    return GT_OK;','validation'),
    ]
    # Match the actual formatted guard, rather than adding a second code path.
    specs[4]=('clock fault accepted','p->clock_fault || !p->config.clock(p->config.clock_context, &now) ||','p->clock_fault || (p->config.clock(p->config.clock_context, &now), false) ||','clock')
    isolation=('posix_spawnattr_setflags(&attributes, POSIX_SPAWN_CLOEXEC_DEFAULT)','posix_spawnattr_setflags(&attributes, 0)') if sys.platform=='darwin' else ('posix_spawn_file_actions_addclosefrom_np(&actions, 3)','posix_spawn_file_actions_addclose(&actions, 999999)')
    specs.append(('unrelated descriptor inherited',*isolation,'fd'))
    try:
        restore();build()
        for case in ('validation','binary','fd','status','missing','cancel','restart','clock'):probe(case)
        for label,old,new,target in specs:
            text=original.decode()
            if old not in text:raise RuntimeError('mutation anchor drift/'+label)
            try:
                copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True)
                probe('missing' if target=='validation' else 'validation')
                print('PASS local provider mutation/'+label+': ordinary red, same-mutant control green',flush=True)
            finally:restore()
        build()
        for case in ('validation','binary','fd','status','missing','cancel','restart','clock'):probe(case)
        print('PASS 11 local provider mutations, 8 baselines and SHA256 rollback',flush=True)
    finally:restore()
    return 0
if __name__=='__main__':sys.exit(main())
