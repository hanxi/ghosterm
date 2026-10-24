#!/usr/bin/env python3
"""Execute relocated macOS helpers and verify the shipped runtime payload."""
import argparse,json,os,runpy,shutil,signal,subprocess,sys,tempfile,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--app',type=Path);args=p.parse_args()
 os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer');os.environ['TMPDIR']=str(ROOT/'tmp')
 if args.app:
  index=json.loads((args.app/'Contents/Resources/sessiond/catalog.json').read_text());source=args.app/'Contents/Resources/sessiond/macos-arm64'/index['local']
 else:
  index=json.loads((ROOT/'build/runtime/catalog.json').read_text());source=ROOT/'build/runtime/macos-arm64'/index['local']
 work=Path(tempfile.mkdtemp(prefix='relocated-session-runtime-',dir=ROOT/'tmp'));daemon=None;test=None;family=None
 def interrupted(sig,frame):raise RuntimeError('runtime package probe interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 try:
  helpers=work/'payload';shutil.copytree(source,helpers);metadata=json.loads((helpers/'runtime.json').read_text())
  if metadata['contract']!=index['contract']:raise RuntimeError('runtime catalog/helper contract mismatch')
  notice=json.loads((helpers/'SOURCE-INFO.json').read_text())
  if notice.get('license')!='MIT' or not notice.get('source_commit'):raise RuntimeError('runtime notice missing project license/commit')
  for name in ('ghosterm-sessiond','ghosterm-session-proxy','lib/libghostty-vt.dylib'):
   result=subprocess.run(['otool','-L',helpers/name],check=True,text=True,capture_output=True)
   for line in result.stdout.splitlines()[1:]:
    dep=line.strip().split(' (')[0]
    if not (dep.startswith('/usr/lib/') or dep=='@rpath/libghostty-vt.dylib'):raise RuntimeError('runtime still depends on build host path')
   subprocess.run(['codesign','--verify','--strict',helpers/name],check=True,capture_output=True)
  runtime=work/'run';runtime.mkdir(mode=0o700)
  log=(work/'daemon.log').open('w');daemon=subprocess.Popen([helpers/'ghosterm-sessiond','serve','--runtime-dir',runtime],stdout=log,stderr=log,start_new_session=True)
  env=dict(os.environ,GHOSTERM_REPLICA_RUNTIME=str(runtime),GHOSTERM_REPLICA_HELPERS=str(helpers),GHOSTERM_REPLICA_CONTRACT=index['contract'])
  test=subprocess.Popen(['xcrun','swift','test','--package-path',ROOT/'apple/GhostermApp','--filter','RecoverableSurfaceTransportTests.testRealLocalDetachResumePreservesProcessAndReplacesSurface'],env=env,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  family=runpy.run_path(str(ROOT/'scripts/probe-native-linux-setup.py'))['Family'](test)
  deadline=time.monotonic()+180
  while True:
   if time.monotonic()>=deadline:raise RuntimeError('relocated native package deadline expired')
   family.observe()
   try:output,_=test.communicate(timeout=.2);break
   except subprocess.TimeoutExpired:pass
  print(output,end='')
  if test.returncode or 'PASS real native transport:' not in output:raise RuntimeError('relocated helper native integration failed')
  print('PASS relocated native package: signed helpers, self-contained VT, license notice, real PTY/PID/resume',flush=True)
 finally:
  try:
   if family:family.stop()
  finally:
   if family:family.close()
   if daemon and daemon.poll() is None:
    os.killpg(daemon.pid,signal.SIGTERM)
    try:daemon.wait(timeout=10)
    except subprocess.TimeoutExpired:os.killpg(daemon.pid,signal.SIGKILL);daemon.wait()
   if daemon:log.close()
   shutil.rmtree(work)
 return 0
if __name__=='__main__':sys.exit(main())
