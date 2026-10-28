#!/usr/bin/env python3
"""Real SSH integration. Setup/cleanup uses system ssh; all assertions use libssh2.

Supply a disposable test account and an absolute remote project tmp directory.
This creates only UUID-owned sockets/configs there, and kills only those servers.
"""
import argparse, os, pathlib, runpy, shlex, signal, subprocess, tempfile, threading, uuid
ROOT=pathlib.Path(__file__).resolve().parent.parent
CONFIG='set -g default-shell /bin/sh\nset -g default-command "exec /bin/sh"\nset -g status off\nset -g assume-paste-time 0\nset -s escape-time 0\nset -g destroy-unattached off\nset -s exit-unattached off\n'

def main():
    def interrupted(signum,frame):raise InterruptedError(signum)
    for sig in [signal.SIGINT,signal.SIGTERM]:signal.signal(sig,interrupted)
    p=argparse.ArgumentParser(description=__doc__)
    env=os.environ
    p.add_argument('--host',default=env.get('GHOSTERM_SSH_HOST'))
    p.add_argument('--port',default=env.get('GHOSTERM_SSH_PORT','22'))
    p.add_argument('--user',default=env.get('GHOSTERM_SSH_USER'))
    p.add_argument('--key',default=env.get('GHOSTERM_SSH_KEY'))
    p.add_argument('--setup-key', help='unrestricted key for fixture setup; defaults to key')
    p.add_argument('--remote-root',default=env.get('GHOSTERM_TMUX_REMOTE_TMP'))
    p.add_argument('--tmux',default='/usr/local/bin/tmux')
    p.add_argument('--binary',type=pathlib.Path,default=ROOT/'core/build/test_tmux')
    p.add_argument('modes',nargs='*',default=['lifecycle','names','identity','policy','attach','race','empty','owner-kill'])
    a=p.parse_args()
    if not all([a.host,a.user,a.key,a.remote_root]) or not a.remote_root.startswith('/'):
        p.error('host, user, key and absolute remote-root required; no SKIP')
    if not pathlib.Path(a.key).is_file():p.error('fixture key unavailable')
    with tempfile.TemporaryDirectory(prefix='tmux-ssh-',dir=ROOT/'tmp') as temp:
        work=pathlib.Path(temp)
        ssh=['ssh','-i',str(pathlib.Path(a.setup_key or a.key).resolve()),'-p',str(a.port),'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','ConnectTimeout=5','-o','StrictHostKeyChecking=accept-new','-o','UserKnownHostsFile='+str(work/'ssh-known-hosts'),a.user+'@'+a.host]
        def remote(script,check=True):
            return subprocess.run(ssh+['/bin/sh'],input=script,text=True,capture_output=True,check=check,timeout=20)
        for mode in a.modes:
            directory=a.remote_root.rstrip('/')+'/tmux-fixture-'+uuid.uuid4().hex
            socket=directory+'/socket';exe=directory+'/tmux';config=directory+'/tmux.conf'
            quote=shlex.quote
            wrapper='#!/bin/sh\nexport TMPDIR='+quote(directory)+'\n'
            if mode=='race':wrapper+='case "$*" in *attach-session*) '+quote(a.tmux)+' -S '+quote(socket)+' -N kill-server;; esac\n'
            command=quote(a.tmux)+' -f '+quote(config)+' "$@"'
            if mode in ['lost-create','lost-terminate']:
                match='*new-session*lost-reply*' if mode=='lost-create' else '*kill-session*'
                wrapper+='case "$*" in '+match+') if [ ! -f '+quote(directory+'/mutation-started')+' ]; then '+command+'; : > '+quote(directory+'/mutation-started')+'; sleep 30; exit; fi;; esac\n'
            wrapper+='exec '+command+'\n'
            setup='umask 077\nmkdir -p '+quote(directory)+'\nprintf %s '+quote(CONFIG+('set -s exit-empty off\n' if mode=='race' else ''))+' > '+quote(config)+'\nprintf %s '+quote(wrapper)+' > '+quote(exe)+'\nchmod 700 '+quote(exe)+'\n'
            proxy=None; watcher=None; faulted=threading.Event();watch_errors=[]
            try:
                remote(setup)
                host,port=a.host,str(a.port)
                if mode in ['reset','blackhole','backpressure','lost-create','lost-terminate','query-timeout','query-cancel']:
                    for marker in ['fault-reset','fault-blackhole','fault-resume','fault-done']:(work/marker).unlink(missing_ok=True)
                    proxy=runpy.run_path(str(ROOT/'scripts/lib/tmux-fault-proxy.py'))['SSHFaultProxy'](a.host,a.port,work)
                    host,port='127.0.0.1',str(proxy.port)
                    if mode.startswith('lost-'):
                        def watch():
                            try:
                                remote('while [ ! -f '+quote(directory+'/mutation-started')+' ]; do sleep 0.02; done\n')
                                proxy.fault('reset');faulted.set()
                            except Exception as error:watch_errors.append(error)
                        watcher=threading.Thread(target=watch,daemon=True);watcher.start()
                subprocess.run([str(a.binary.resolve()),mode,exe,socket,host,port,a.user,str(pathlib.Path(a.key).resolve()),str(work)],check=True,timeout=100,env=dict(env,TMPDIR=str(work)))
                if mode.startswith('lost-'):
                    watcher.join(timeout=1)
                    if not faulted.is_set() or watch_errors:raise RuntimeError('mutation fault was not actually observed: '+repr(watch_errors))
            finally:
                if proxy:proxy.close()
                result=remote(quote(a.tmux)+' -S '+quote(socket)+' -N kill-server\nrm -rf -- '+quote(directory)+'\n',check=False)
                if result.returncode:print(result.stderr)
if __name__=='__main__':main()
