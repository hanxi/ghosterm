#!/usr/bin/env python3
"""Real SSH integration. Setup/cleanup uses system ssh; all assertions use libssh2.

Supply a disposable test account and an absolute remote project tmp directory.
This creates only UUID-owned sockets/configs there, and kills only those servers.
"""
import argparse, os, pathlib, shlex, subprocess, tempfile, uuid
ROOT=pathlib.Path(__file__).resolve().parent.parent
CONFIG='set -g default-shell /bin/sh\nset -g default-command "exec /bin/sh"\nset -g status off\nset -g assume-paste-time 0\nset -s escape-time 0\nset -g destroy-unattached off\nset -s exit-unattached off\n'

def main():
    p=argparse.ArgumentParser(description=__doc__)
    env=os.environ
    p.add_argument('--host',default=env.get('GHOSTERM_SSH_HOST'))
    p.add_argument('--port',default=env.get('GHOSTERM_SSH_PORT','22'))
    p.add_argument('--user',default=env.get('GHOSTERM_SSH_USER'))
    p.add_argument('--key',default=env.get('GHOSTERM_SSH_KEY'))
    p.add_argument('--remote-root',default=env.get('GHOSTERM_TMUX_REMOTE_TMP'))
    p.add_argument('--tmux',default='/usr/local/bin/tmux')
    p.add_argument('--binary',type=pathlib.Path,default=ROOT/'core/build/test_tmux')
    p.add_argument('modes',nargs='*',default=['lifecycle','names','identity','policy','attach','race'])
    a=p.parse_args()
    if not all([a.host,a.user,a.key,a.remote_root]) or not a.remote_root.startswith('/'):
        p.error('host, user, key and absolute remote-root required; no SKIP')
    if not pathlib.Path(a.key).is_file():p.error('fixture key unavailable')
    with tempfile.TemporaryDirectory(prefix='tmux-ssh-',dir=ROOT/'tmp') as temp:
        work=pathlib.Path(temp)
        ssh=['ssh','-i',str(pathlib.Path(a.key).resolve()),'-p',str(a.port),'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','ConnectTimeout=5','-o','StrictHostKeyChecking=accept-new','-o','UserKnownHostsFile='+str(work/'ssh-known-hosts'),a.user+'@'+a.host]
        def remote(script,check=True):
            return subprocess.run(ssh+['/bin/sh'],input=script,text=True,capture_output=True,check=check,timeout=20)
        for mode in a.modes:
            directory=a.remote_root.rstrip('/')+'/tmux-fixture-'+uuid.uuid4().hex
            socket=directory+'/socket';exe=directory+'/tmux';config=directory+'/tmux.conf'
            quote=shlex.quote
            wrapper='#!/bin/sh\nexport TMPDIR='+quote(directory)+'\n'
            if mode=='race':wrapper+='case "$*" in *attach-session*) '+quote(a.tmux)+' -S '+quote(socket)+' -N kill-server;; esac\n'
            wrapper+='exec '+quote(a.tmux)+' -f '+quote(config)+' "$@"\n'
            setup='umask 077\nmkdir -p '+quote(directory)+'\nprintf %s '+quote(CONFIG+('set -s exit-empty off\n' if mode=='race' else ''))+' > '+quote(config)+'\nprintf %s '+quote(wrapper)+' > '+quote(exe)+'\nchmod 700 '+quote(exe)+'\n'
            try:
                remote(setup)
                subprocess.run([str(a.binary.resolve()),mode,exe,socket,a.host,str(a.port),a.user,str(pathlib.Path(a.key).resolve()),str(work)],check=True,timeout=100,env=dict(env,TMPDIR=str(work)))
            finally:
                result=remote(quote(a.tmux)+' -S '+quote(socket)+' -N kill-server\nrm -rf -- '+quote(directory)+'\n',check=False)
                if result.returncode:print(result.stderr)
if __name__=='__main__':main()
