#!/usr/bin/env python3
"""Retained SSH/streaming-exec gates, isolated and linked against each mutant."""
import hashlib, os, pathlib, runpy, shutil, signal, subprocess, sys, tempfile
ROOT=pathlib.Path(__file__).resolve().parent.parent
MARK='GT_SSH_CORE_MUTATION_MARK'

def run(args):
    p=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require ssh-core; shift; gt_timeout 240 "$@"','ssh-core',str(ROOT)]+list(map(str,args)),cwd=ROOT,capture_output=True,text=True,env=dict(os.environ,TMPDIR=str(ROOT/'tmp')))
    return p.returncode,p.stdout+p.stderr

def main():
    group=sys.argv[1] if len(sys.argv)==2 else ''
    if group not in ['ssh','exec','sftp','credentials']:raise SystemExit('group must be ssh, exec, sftp or credentials')
    filename='sftp.c' if group=='sftp' else 'ssh.c'
    source=ROOT/'core/src'/filename;original=source.read_bytes();digest=hashlib.sha256(original).digest()
    if MARK.encode() in original or b'GT_MUTATION_MARK' in original:raise SystemExit(2)
    def interrupt(sig,frame):raise InterruptedError(sig)
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupt)
    replace=runpy.run_path(str(ROOT/'scripts/mutation-check-tmux.py'))['replacement']
    cases={
      'credentials':[
        ('key-retry','if(s->pem && rc==LIBSSH2_ERROR_FILE)error=GT_ERR_AUTH;',
         'if(false && s->pem && rc==LIBSSH2_ERROR_FILE)error=GT_ERR_AUTH;'),
      ],
      'ssh':[
        ('zero-poll','if (timeout_ms == 0) libssh2_session_set_blocking(sh->sess, 0);',
         'if (timeout_ms == 0) libssh2_session_set_timeout(sh->sess, 0);'),
        ('UNKNOWN','} else { decision = GT_HOSTKEY_UNKNOWN; }','} else { decision = GT_HOSTKEY_ACCEPT; }'),
        ('MISMATCH','if (kh == KH_MISMATCH) { decision = GT_HOSTKEY_REJECT;','if (kh == KH_MISMATCH) { decision = GT_HOSTKEY_ACCEPT;'),
        ('TOFU','int saved = kh_append(ssh->kh_path, ssh->host, port, ktype, b64, &kh);','int saved = GT_OK;'),
        ('resize','if (libssh2_channel_request_pty_size(sh->chan, cols ? cols : 80, rows ? rows : 24) != 0) {','(void)cols; (void)rows; if (false) {'),
      ],
      'sftp':[('F1', '  out->size = out->has_size ? a->filesize : 0;', '  out->size = 0; /* GT_MUTATION_MARK */'), ('F2', '    strcpy(e->full_path + off, name);', '    strcpy(e->full_path + off, "x"); /* GT_MUTATION_MARK */'), ('F3', '    if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { rc = GT_JOB_FAILED; break; }', '    if (fwrite("", 1, 0, f) != (size_t)n) { rc = GT_JOB_FAILED; break; } /* GT_MUTATION_MARK */'), ('F4', 'static void emit_job_done(uint64_t job_id, int result, const char *detail) {', 'static void emit_job_done(uint64_t job_id, int result, const char *detail) {\n  result = GT_JOB_OK; /* GT_MUTATION_MARK: 三态折叠 */'), ('F5', '    int r = remove_all_rec(sftp, ents[i].full_path, depth + 1);', '    int r = gt_sftp_remove(sftp, ents[i].full_path); /* GT_MUTATION_MARK */'), ('F6', '  a.permissions = (unsigned long)(mode & 07777);', '  a.permissions = (unsigned long)((mode & 0) | 0644); /* GT_MUTATION_MARK */'), ('F7', '    if (e->stat.is_dir && (max_depth < 0 || depth < max_depth)) {', '    if (e->stat.is_dir) { (void)max_depth; /* GT_MUTATION_MARK */'), ('F8', '    if (atomic_load(&j->paused)) { usleep(20 * 1000); continue; }', '    if (atomic_load(&j->paused) && false) { usleep(20 * 1000); continue; } /* GT_MUTATION_MARK */')],
      'exec':[
        ('timeout','libssh2_session_set_timeout(e->sess, previous_timeout);','(void)previous_timeout;'),
        ('stderr','ssize_t b = libssh2_channel_read_stderr(e->chan, (char *)buf, cap);','ssize_t b = libssh2_channel_read(e->chan, (char *)buf, cap);'),
        ('exit','int rc = libssh2_channel_get_exit_status(e->chan);','int rc = 0;'),
      ]}
    with tempfile.TemporaryDirectory(prefix='ssh-core-mutations-',dir=ROOT/'tmp') as work:
        folder=pathlib.Path(work);core=folder/'core';build=folder/'build';path=core/'src'/filename
        shutil.copytree(ROOT/'core',core,ignore=shutil.ignore_patterns('build'))
        def command(args):
            code,out=run(args)
            if code:print(out,flush=True);raise RuntimeError('SSH mutation build/tool failed')
        command(['cmake','-S',core,'-B',build]);command(['cmake','--build',build,'-j4'])
        def check(red=False,control=False,expected=None):
            target='test_ssh_connector' if group=='credentials' else 'test_ssh'
            args=[build/'test_config',ROOT/'core/tests/fixtures'] if control else ['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',build/target]
            code,out=run(args)
            marker='FAIL ' if group=='credentials' else 'FAIL:'
            valid=(code!=0 and (expected or marker) in out) if red else code==0 and 'SKIP' not in out
            if not valid:print(out,flush=True);raise RuntimeError(group+' expected '+('red' if red else 'green'))
        check();check(control=True)
        def rebuild():
            for obj in build.rglob(filename+'.o'):obj.unlink()
            for file in ['libghosterm_core.a','test_ssh','test_ssh_connector','test_config']:(build/file).unlink(missing_ok=True)
            command(['cmake','--build',build,'-j4'])
        try:
            for name,old,new in cases[group]:
                text=original.decode()
                if group=='sftp' and name=='F8':
                    assert text.count(old)==2; mutated=text.replace(old,new)
                else: mutated=replace(text,old,new)
                path.write_text(mutated+'\n/* '+MARK+' */\n');rebuild()
                check(red=True,expected='FAIL: exec read restores shared SFTP timeout' if group=='exec' and name=='timeout' else None);check(control=True)
                path.write_bytes(original);assert hashlib.sha256(path.read_bytes()).digest()==digest;rebuild();check()
                print('PASS '+group+' '+name+': target red, control green, SHA rollback',flush=True)
        finally:
            path.write_bytes(original);assert hashlib.sha256(source.read_bytes()).digest()==digest
    print('PASS retained '+group+' mutations ('+str(len(cases[group]))+' groups)')
if __name__=='__main__':main()
