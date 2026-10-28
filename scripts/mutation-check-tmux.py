#!/usr/bin/env python3
"""Isolated mutations of the shared tmux seam; real tmux plus offline failures."""
import hashlib, os, pathlib, re, shutil, signal, subprocess, tempfile
ROOT = pathlib.Path(__file__).resolve().parent.parent
MARK = 'GT_MUTATION_MARK'
FILES = ['src/tmux/tmux.c', 'src/tmux/command.c', 'src/ssh.c', 'src/io/pty_posix.c']

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def timed(args, expected=0):
    cmd = ['bash', '-c', '. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require mutation-check-tmux; shift; gt_timeout 180 "$@"', 'tmux-mutation', str(ROOT)] + list(map(str, args))
    result = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, env=dict(os.environ, TMPDIR=str(ROOT/'tmp')))
    if (result.returncode == 0) != (expected == 0):
        print(result.stdout, result.stderr, flush=True)
        raise RuntimeError(f'expected {expected}, actual {result.returncode}: {args}')
    if expected and 'FAIL ' not in result.stderr:
        # A compiler/tool failure is not a failed behavior assertion.
        print(result.stdout, result.stderr, flush=True)
        raise RuntimeError('mutation did not reach a behavior assertion')

def replacement(source, old, new):
    # C-token matching keeps anchors precise while allowing normal formatting.
    tokens = re.findall(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[A-Za-z_]\w*|[0-9]+|==|!=|>=|<=|&&|\|\||\+\+|->|\S', old)
    pattern = r'\s*'.join(re.escape(token) for token in tokens)
    matches = list(re.finditer(pattern, source))
    if len(matches) != 1: raise RuntimeError('mutation source missing/ambiguous: '+repr(old))
    return re.sub(pattern, lambda _: new, source, count=1)

def main():
    (ROOT/'tmp/tmux-analysis').mkdir(parents=True,exist_ok=True)
    originals = {f: (ROOT/'core'/f).read_bytes() for f in FILES}
    checksums = {f: sha(ROOT/'core'/f) for f in FILES}
    if any(MARK.encode() in b for b in originals.values()): raise SystemExit('leftover mutation marker: exit 2')
    if not shutil.which('tmux') and not os.environ.get('TMUX_BINARY'): raise SystemExit('real tmux >=3.2a required')
    def interrupted(signum, frame): raise InterruptedError(signum)
    for sig in [signal.SIGINT, signal.SIGTERM]: signal.signal(sig, interrupted)
    with tempfile.TemporaryDirectory(prefix='tmux-mutations-', dir=ROOT/'tmp') as work:
        folder = pathlib.Path(work); core = folder/'core'; build=folder/'build'
        shutil.copytree(ROOT/'core', core, ignore=shutil.ignore_patterns('build'))
        timed(['cmake','-S',core,'-B',build]); timed(['cmake','--build',build,'-j4'])
        binary=build/'test_tmux'
        def check(mode, red=False):
            if mode == 'command': timed([binary,'command'],1 if red else 0)
            elif mode == 'discovery': timed(['python3',ROOT/'core/tests/scripts/test-tmux-discovery.py',binary],1 if red else 0)
            else: timed(['python3',ROOT/'scripts/test-tmux.py','--binary',binary] + (['--fake'] if mode=='adversarial' else [mode]),1 if red else 0)
        for mode in ['command','discovery','adversarial','names','identity','policy','attach','race','empty','owner-kill']: check(mode)
        mutants = [
            ('version-probe-flag', 'src/tmux/tmux.c', 'run_argv(&context, args, true, &b, &r)', 'run_argv(&context, args, false, &b, &r)', 'discovery', 'command'),
            ('path-newline', 'src/tmux/tmux.c', "\"case $p in *'\\n'*) exit 1;; esac; \"", '""', 'discovery', 'command'),
            ('path-order', 'src/tmux/tmux.c', 'strcpy(out[(*count)++].path, p);', 'strcpy(out[0].path, p); (*count)++;', 'discovery', 'command'),
            ('path-duplicates', 'src/tmux/tmux.c', 'if (!duplicate) {', 'if ((void)duplicate, true) {', 'discovery', 'command'),
            ('path-capacity', 'src/tmux/tmux.c', 'if (*count == cap) { rc = GT_ERR_PROTOCOL; goto done; }', 'if (*count == cap) break;', 'discovery', 'command'),
            ('path-unknown', 'src/tmux/tmux.c', 'out[i].status = ok ? GT_OK : valid ? GT_ERR_UNSUPPORTED : GT_ERR_PROTOCOL;', 'out[i].status = ok ? GT_OK : GT_ERR_UNSUPPORTED;', 'discovery', 'command'),
            ('missing', 'src/tmux/tmux.c', '  if(!r->completed||r->exit_code!=1||r->out_len||!r->err)return false;', '  if(r->completed&&r->exit_code==1)return true;', 'adversarial', 'command'),
            ('duplicates', 'src/tmux/tmux.c', 'if(rows[i].id==r->id||', 'if(false||', 'adversarial', 'command'),
            ('utf8', 'src/tmux/tmux.c', '}else return false;\n    unsigned count=n;', '}else {p++;continue;}\n    unsigned count=n;', 'adversarial', 'command'),
            ('unknown-policy', 'src/tmux/tmux.c', 'r->destroy_unattached=-1;r->exit_unattached=-1;', 'r->destroy_unattached=0;r->exit_unattached=0;', 'adversarial', 'command'),
            ('unknown-result', 'src/tmux/tmux.c', 'r.started?GT_TMUX_UNKNOWN:GT_TMUX_REJECTED', 'GT_TMUX_REJECTED', 'adversarial', 'command'),
            ('dirty-result', 'src/tmux/tmux.c', '*result=r.exit_code?GT_TMUX_REJECTED:GT_TMUX_UNKNOWN', '*result=GT_TMUX_REJECTED', 'adversarial', 'command'),
            ('rejected-result', 'src/tmux/tmux.c', '*result=r.exit_code?GT_TMUX_REJECTED:GT_TMUX_UNKNOWN', '*result=GT_TMUX_UNKNOWN', 'adversarial', 'command'),
            ('discovery-status', 'src/tmux/tmux.c', 'rc = GT_ERR_NOT_FOUND;', 'rc = GT_ERR_UNSUPPORTED;', 'adversarial', 'command'),
            ('version-status', 'src/tmux/tmux.c', 'rc = valid ? GT_ERR_UNSUPPORTED : GT_ERR_PROTOCOL;', 'rc = GT_ERR_UNSUPPORTED;', 'adversarial', 'command'),
            ('version', 'src/tmux/tmux.c', "return major>3||(major==3&&(minor>2||(minor==2&&*suffix>='a')));", 'return major>=3;', 'adversarial', 'command'),
            ('format-injection', 'src/tmux/tmux.c', "if(name[i]=='#')*out++='#';", '', 'names', 'command'),
            ('separator-injection', 'src/tmux/tmux.c', "if(i+1==n&&name[i]==';')*out++='\\\\';", '', 'names', 'command'),
            ('shell-quote', 'src/tmux/command.c', 'memcpy(p,"\'\\\\\'\'",4);p+=4;', 'memcpy(p,"xxxx",4);p+=4;', 'command', 'lifecycle'),
            ('identity', 'src/tmux/tmux.c', 'r->id==target->id&&r->created==target->created&&r->server_pid==target->server_pid&&r->server_started==target->server_started', 'r->id==target->id', 'identity', 'command'),
            ('no-autostart', 'src/tmux/tmux.c', 't->socket,"-N","attach-session"', 't->socket,"attach-session"', 'race', 'command'),
            ('known-empty', 'src/tmux/tmux.c', '*server = GT_TMUX_SERVER_RUNNING;', '*server = GT_TMUX_SERVER_ABSENT;', 'adversarial', 'command'),
            ('local-unknown-exit', 'src/io/pty_posix.c', 'bool e = atomic_load(&t->exited) && atomic_load(&t->exit_code) >= 0;', 'bool e = atomic_load(&t->exited);', 'command', 'lifecycle'),
            ('closed-stdio', 'src/tmux/command.c', 'if (p[i] < 3) {', 'if (false) {', 'command', 'lifecycle'),
            ('cancellation', 'src/tmux/command.c', 'b->cancelled && b->cancelled(b->context)', 'false', 'adversarial', 'command'),
            ('resize', 'src/io/pty_posix.c', 'if (ioctl(t->master_fd, TIOCSWINSZ, &ws) < 0)', 'if ((void)ws, false)', 'attach', 'command'),
        ]
        requested=os.environ.get('GT_TMUX_MUTATIONS')
        if requested: mutants=[case for case in mutants if case[0] in requested.split(',')]
        try:
            for name, file, old, new, target, control in mutants:
                path=core/file; source=originals[file].decode()
                path.write_text(replacement(source,old,new)+'\n/* '+MARK+' */\n')
                # macOS make can compare timestamps at whole-second precision.
                # Invalidate the affected objects so a quick restore/mutation
                # cannot reuse the previous baseline's machine code.
                for obj in build.rglob(path.name + '.o'): obj.unlink()
                (build/'libghosterm_core.a').unlink(missing_ok=True)
                binary.unlink(missing_ok=True)
                timed(['cmake','--build',build,'-j4'])
                print('RUN '+name,flush=True)
                try:
                    check(target,True);check(control)
                except Exception:
                    shutil.copy2(binary, ROOT/'tmp/tmux-analysis/failed-mutation-test')
                    shutil.copy2(path, ROOT/'tmp/tmux-analysis/failed-mutation-source.c')
                    shutil.copy2(core/'tests/test_tmux.c', ROOT/'tmp/tmux-analysis/failed-mutation-test.c')
                    raise
                path.write_bytes(originals[file]); assert sha(path)==checksums[file]
                for obj in build.rglob(path.name + '.o'): obj.unlink()
                (build/'libghosterm_core.a').unlink(missing_ok=True)
                binary.unlink(missing_ok=True)
                timed(['cmake','--build',build,'-j4'])
                print(f'PASS {name}: target red, control green, SHA rollback',flush=True)
            for mode in ['command','adversarial','attach','race']:check(mode)
        finally:
            for file,data in originals.items():
                (core/file).write_bytes(data)
                assert sha(core/file)==checksums[file]
                assert sha(ROOT/'core'/file)==checksums[file], 'source tree changed during mutations'
    print('PASS tmux mutations ('+str(len(mutants))+' groups, isolated, restored)')
if __name__=='__main__': main()
