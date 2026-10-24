#!/usr/bin/env python3
"""Kill and relaunch the actual GUI against an explicitly owned Linux fixture."""
import argparse,json,os,runpy,shutil,signal,subprocess,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
fixture=runpy.run_path(str(ROOT/'scripts/probe-native-linux-setup.py'))
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--executable',required=True,type=Path);p.add_argument('--local',action='store_true');args=p.parse_args()
 env=dict(os.environ)
 if not args.local:fixture['fixture_paths'](env)
 work=ROOT/'tmp'/('gui-lifecycle-'+str(os.getpid()));work.mkdir(mode=0o700)
 config=work/'ssh_config';known=work/'known_hosts';status=work/'status.json'
 if not args.local:config.write_text('Host linux\n HostName 127.0.0.1\n Port '+env['GHOSTERM_REPLICA_SSH_PORT']+'\n User '+env['GHOSTERM_REPLICA_SSH_USER']+'\n IdentityFile '+env['GHOSTERM_REPLICA_SSH_KEY']+'\n')
 if args.local:config.write_text('');known.write_text('')
 else:shutil.copyfile(env['GHOSTERM_REPLICA_SSH_KNOWN'],known)
 known.chmod(0o600)
 settings={'config':str(config),'knownHosts':str(known),'records':str(work/'records'),'runtimeRoot':str(work/'runtime'),'installRoot':env.get('GHOSTERM_SESSION_SETUP_ROOT',''),'remoteRuntime':env.get('GHOSTERM_REPLICA_RUNTIME',''),'alias':'linux','status':str(status),'localShell':args.local}
 configuration=work/'probe.json';configuration.write_text(json.dumps(settings));configuration.chmod(0o600)
 child=None;family=None;daemon=None;daemon_log=None
 def wait(state):
  deadline=time.monotonic()+150
  while time.monotonic()<deadline:
   if status.exists():
    value=json.loads(status.read_text())
    if value['state']=='failed':raise RuntimeError('GUI lifecycle failed: '+str(value))
    if value['state']==state:return value
   if child.poll() is not None:raise RuntimeError('GUI exited before '+state)
   family.observe();time.sleep(.02)
  raise RuntimeError('GUI lifecycle deadline expired/'+state)
 def interrupted(sig,frame):raise RuntimeError('GUI probe interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 try:
  if args.local:
   runtime=args.executable.parent.parent/'Resources/sessiond'
   if not (runtime/'catalog.json').is_file():runtime=ROOT/'build/runtime'
   catalog=json.loads((runtime/'catalog.json').read_text());helper=runtime/'macos-arm64'/catalog['local']/'ghosterm-sessiond'
   daemon_log=(work/'daemon.log').open('w')
   daemon=subprocess.Popen([helper,'serve','--runtime-dir',work/'runtime'/catalog['contract'][:16]],stdout=daemon_log,stderr=daemon_log,start_new_session=True)
   endpoint=work/'runtime'/catalog['contract'][:16]/'session.sock';deadline=time.monotonic()+3
   while not endpoint.exists() and daemon.poll() is None and time.monotonic()<deadline:time.sleep(.005)
   if not endpoint.exists() or daemon.poll() is not None:raise RuntimeError('owned local GUI fixture daemon failed to listen')
  with (work/'gui.log').open('w') as log:
   def launch(action):
    nonlocal child,family
    child=subprocess.Popen([args.executable,'--session-lifecycle-probe',configuration,action],env=env,stdout=log,stderr=log,start_new_session=True)
    family=fixture['Family'](child)
   launch('create');original=wait('ready');assert original['pid']>1 and original['session']>0 and original['visible'] and original['cwd']=='/usr'
   child.kill();child.wait(timeout=10);assert child.returncode==-signal.SIGKILL
   family.stop();family.close();family=None
   status.unlink();launch('restore');restored=wait('ready')
   for key in ('pid','session','instance','cwd'):assert restored[key]==original[key],(key,original,restored)
   assert restored['visible']
   report=work/'result.json';report.write_text(json.dumps({'original':original,'restored':restored,'signal':'SIGKILL','executable':str(args.executable)},sort_keys=True,indent=2)+'\n');report.chmod(0o600)
   Path(str(status)+'.stop').touch();wait('ended');child.wait(timeout=10);assert child.returncode==0
   print('PASS real GUI SIGKILL/relaunch: '+('default local daemon' if args.local else 'default Linux installation')+', visible RootView, fresh command response, original Source/PID/cwd and observed explicit stop',flush=True)
 finally:
  try:
   if family:
    family.stop();family.close()
  finally:
   if daemon and daemon.poll() is None:
    os.killpg(daemon.pid,signal.SIGTERM)
    try:daemon.wait(timeout=10)
    except subprocess.TimeoutExpired:os.killpg(daemon.pid,signal.SIGKILL);daemon.wait()
   if daemon_log:daemon_log.close()
   if not args.local:fixture['cleanup'](env)
 return 0
if __name__=='__main__':raise SystemExit(main())
