"""Disposable real tmux server namespace and exec-capable SSH test key."""
from contextlib import contextmanager
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import sys


@contextmanager
def fixture(root, ssh_work, enabled, locale):
    if not enabled:
        yield {}
        return
    binary = shutil.which('tmux')
    if not binary:
        raise RuntimeError('Android tmux tests require real tmux; missing is not a pass')
    binary = str(Path(binary).resolve())
    with tempfile.TemporaryDirectory(prefix='tmx-', dir=root / 'tmp') as directory:
        folder = Path(directory)
        prefix = str(folder / 's')
        if len((prefix + '-12345678').encode()) >= 100:
            raise RuntimeError('tmux fixture Unix socket path is too long')
        home = folder / 'home'
        home.mkdir()
        key = folder / 'clientkey'
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)], check=True, capture_output=True)
        command_dir = folder / 'bin'
        command_dir.mkdir()
        executable = command_dir / 'tmux'
        real = shlex.quote(binary) + ' -f /dev/null'
        executable.write_text('#!'+sys.executable+'\nimport os,sys\nargs=sys.argv[1:]\n' +
            'redirect=os.environ.get("GHOSTERM_TMUX_REDIRECT")\n' +
            'if redirect:\n for i,arg in enumerate(args[:-1]):\n  if arg=="-S":args[i+1]=redirect\n' +
            'if "new-session" in args:\n fd=os.open('+repr(str(folder/'create-attempts'))+',os.O_CREAT|os.O_APPEND|os.O_WRONLY,0o600)\n os.write(fd,b"1\\n")\n os.close(fd)\n' +
            'cmd=['+repr(binary)+',"-f","/dev/null",*args]\n' +
            'if os.environ.get("GHOSTERM_TMUX_DELAY_CREATE") and "new-session" in args:\n import subprocess,time\n result=subprocess.run(cmd)\n time.sleep(8)\n sys.exit(result.returncode)\n' +
            'os.execv('+repr(binary)+',cmd)\n')
        executable.chmod(0o700)
        shell = folder / 'ssh-command'
        shell.write_text(
            '#!/bin/sh\n'
            'export HOME=' + shlex.quote(str(home)) + '\n'
            'export LC_ALL=' + shlex.quote(locale) + '\n'
            'export TMUX_TMPDIR=' + shlex.quote(str(folder)) + '\n'
            'export SHELL=/bin/sh ENV=/dev/null BASH_ENV=/dev/null INPUTRC=/dev/null\n'
            'export PATH=' + shlex.quote(str(command_dir) + ':' + str(Path(binary).parent) + ':/usr/bin:/bin') + '\n'
            'unset TMUX\n'
            'if [ -n "$SSH_ORIGINAL_COMMAND" ]; then exec /bin/sh -c "$SSH_ORIGINAL_COMMAND"; fi\n'
            'exec /bin/sh -i\n')
        shell.chmod(0o700)
        command = shlex.quote(str(shell)).replace('\\', '\\\\').replace('"', '\\"')
        with Path(ssh_work, 'authorized_keys').open('a') as output:
            output.write('no-agent-forwarding,no-port-forwarding,no-X11-forwarding,command="' + command + '" ' + Path(str(key)+'.pub').read_text())
        # A separate exec environment cannot see tmux until its login shell runs.
        # Keep its executable in a quoted/space path to exercise shell data quoting.
        login = folder / "login ' shell"
        login.write_text('#!/bin/sh\nif [ "${1-}" = -l ]; then\nshift\n'
                         'export PATH='+shlex.quote(str(command_dir)+':/usr/bin:/bin')+
                         '\nfi\nexec /bin/sh "$@"\n')
        login.chmod(0o700)
        # A deterministic login PATH keeps the delayed/refusing wrappers first.
        # macOS /bin/sh -l would reorder it through /etc/profile/path_helper.
        default_shell='export SHELL='+shlex.quote(str(login))+' '
        shell.write_text(shell.read_text().replace('export SHELL=/bin/sh ',default_shell))
        (home/'.tmux.conf').write_text('set -g default-shell /bin/sh\nset -g default-command "exec /bin/sh"\n')
        missing_path = folder / 'empty-path'
        missing_path.mkdir()
        def login_key(label, login_shell):
            private = folder / (label+'-key')
            subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(private)],check=True,capture_output=True)
            endpoint = folder / (label+'-endpoint')
            endpoint.write_text(shell.read_text().replace(default_shell,
                'export SHELL='+shlex.quote(str(login_shell))+' ').replace(
                'export PATH='+shlex.quote(str(command_dir)+':'+str(Path(binary).parent)+':/usr/bin:/bin'),
                'export PATH='+shlex.quote(str(missing_path))))
            endpoint.chmod(0o700)
            forced = shlex.quote(str(endpoint)).replace('\\','\\\\').replace('"','\\"')
            with Path(ssh_work,'authorized_keys').open('a') as output:
                output.write('command="'+forced+'" '+Path(str(private)+'.pub').read_text())
            return private.read_text()
        login_data = dict(loginPem=login_key('login',login))
        if sys.platform == 'darwin' and Path('/opt/homebrew/bin/tmux').is_file():
            # Actual macOS zsh + Homebrew, with an isolated HOME/.zprofile.
            (home/'.zprofile').write_text('eval "$(/opt/homebrew/bin/brew shellenv)"\n')
            login_data['homebrewPem'] = login_key('homebrew','/bin/zsh')
        redirected = folder / 's-redirect'
        subprocess.run([binary,'-S',str(redirected),'-f','/dev/null','new-session','-d','-s','wrong-server'],
            check=True,capture_output=True,env=dict(os.environ,HOME=str(home),SHELL='/bin/sh',TMUX=''))
        other_key=folder/'redirect-key'
        subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(other_key)],check=True,capture_output=True)
        other_shell=folder/'ssh-redirect'
        other_shell.write_text(shell.read_text().replace('unset TMUX\n','unset TMUX\nexport GHOSTERM_TMUX_REDIRECT='+shlex.quote(str(redirected))+'\n'))
        other_shell.chmod(0o700)
        forced=shlex.quote(str(other_shell)).replace('\\','\\\\').replace('"','\\"')
        with Path(ssh_work,'authorized_keys').open('a') as output:
            output.write('no-agent-forwarding,no-port-forwarding,no-X11-forwarding,command="'+forced+'" '+Path(str(other_key)+'.pub').read_text())
        delay_key=folder/'delay-key'
        subprocess.run(['ssh-keygen','-q','-t','ed25519','-N','','-f',str(delay_key)],check=True,capture_output=True)
        delay_shell=folder/'ssh-delay'
        delay_shell.write_text(shell.read_text().replace('unset TMUX\n','unset TMUX\nexport GHOSTERM_TMUX_DELAY_CREATE=1\n'))
        delay_shell.chmod(0o700)
        forced_delay=shlex.quote(str(delay_shell)).replace('\\','\\\\').replace('"','\\"')
        with Path(ssh_work,'authorized_keys').open('a') as output:
            output.write('no-agent-forwarding,no-port-forwarding,no-X11-forwarding,command="'+forced_delay+'" '+Path(str(delay_key)+'.pub').read_text())
        paths = {}
        for label, body in {
            'unsupported': "printf 'tmux 1.0\\n'\n",
            'unknown': "printf 'unparseable-version\\n'\n",
            'slow': 'sleep 10\nexec ' + real + ' "$@"\n',
            'rejected': 'for arg in "$@"; do if [ "$arg" = new-session ]; then printf "CREATE_REFUSED\\n" >&2; exit 73; fi; done\nexec ' + real + ' "$@"\n',
            'uncertain': 'for arg in "$@"; do if [ "$arg" = new-session ]; then ' + real + ' "$@"; result=$?; sleep 8; exit "$result"; fi; done\nexec ' + real + ' "$@"\n',
        }.items():
            path = folder / label
            path.write_text('#!/bin/sh\n' + body)
            path.chmod(0o700)
            paths[label] = str(path)
        try:
            yield dict(tmux=dict(path=str(executable), socketPrefix=prefix, pem=key.read_text(),createLog=str(folder/"create-attempts"),redirectPem=other_key.read_text(),delayPem=delay_key.read_text(),defaultSocket=str(folder/("tmux-"+str(os.getuid()))/"default"), **login_data, **paths))
        finally:
            for socket in [*folder.glob('s-*'),*folder.glob('tmux-*/default')]:
                result = subprocess.run([binary, '-S', str(socket), 'kill-server'], capture_output=True)
                check = subprocess.run([binary, '-S', str(socket), '-N', 'display-message', '-p', '#{pid}'], capture_output=True)
                if check.returncode == 0:
                    raise RuntimeError('Isolated tmux server survived cleanup: ' + str(socket))
                if result.returncode not in (0, 1):
                    raise RuntimeError('Isolated tmux cleanup failed')
