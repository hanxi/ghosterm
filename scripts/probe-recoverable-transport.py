#!/usr/bin/env python3
"""Real local daemon -> shared coordinator -> native Metal Surface integration."""
import argparse,os,re,signal,subprocess,sys,tempfile,shutil,runpy,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def stop_group(p):
 if p.poll() is not None:return
 os.killpg(p.pid,signal.SIGTERM)
 try:p.wait(timeout=10)
 except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def run(args):
 p=subprocess.Popen([str(x) for x in args],start_new_session=True)
 try:
  if p.wait(timeout=120):raise RuntimeError('native transport build command failed: '+str(args[0]))
 finally:stop_group(p)
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--management',action='store_true');parser.add_argument('--admission',action='store_true');parser.add_argument('--pane',action='store_true');parser.add_argument('--control',action='store_true');args=parser.parse_args()
 def interrupted(sig,frame):raise RuntimeError('native transport probe interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
 build=ROOT/'tmp/native-replica-runtime';headers=ROOT/'build/GhosttyKit.xcframework/macos-arm64/Headers'
 run(['cmake','-S',ROOT/'daemon','-B',build,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_VT_INCLUDE_DIR='+str(headers),'-DGHOSTERM_VT_LIBRARY='+str(ROOT/'tmp/worker-surface/out/lib/libghostty-vt.dylib')])
 run(['cmake','--build',build,'--target','ghosterm-sessiond','ghosterm-session-proxy','-j','4'])
 contract=re.search(r'GT_SESSION_CONTRACT_HEX "([0-9a-f]{64})"',(build/'generated/session-contract.h').read_text()).group(1)
 runtime=Path(tempfile.mkdtemp(prefix='native-transport-',dir=ROOT/'tmp'))
 log=(runtime/'daemon.log').open('w');daemon=subprocess.Popen([build/'ghosterm-sessiond','serve','--runtime-dir',runtime],stdout=log,stderr=log,start_new_session=True)
 test=None;family=None
 try:
  env=dict(os.environ,GHOSTERM_REPLICA_RUNTIME=str(runtime),GHOSTERM_REPLICA_HELPERS=str(build),GHOSTERM_REPLICA_CONTRACT=contract)
  # An isolated mutation package consumes the same production helpers/contract.
  package=Path(env.get('GHOSTERM_NATIVE_MANAGEMENT_PACKAGE',ROOT/'apple/GhostermCore')) if args.management else Path(env.get('GHOSTERM_NATIVE_TRANSPORT_PACKAGE',ROOT/'apple/GhostermApp'))
  name='SessionManagementTests.testRealLocalDaemonManagement' if args.management else 'SessionControlTransportTests.testRealKnownAndPendingCreationCanBeInspectedAndExplicitlyTerminated' if args.control else 'RecoverablePaneTests.testRealPaneResumesFromDurableRecordAndRemountsSurface' if args.pane else 'RecoverableSurfaceTransportTests.testDurableAdmissionPrecedesRealCreateAndFailureNeverFlushes' if args.admission else 'RecoverableSurfaceTransportTests.testRealLocalDetachResumePreservesProcessAndReplacesSurface'
  marker='PASS native session management:' if args.management else 'PASS native control Transport:' if args.control else 'PASS durable native pane:' if args.pane else 'PASS durable native admission:' if args.admission else 'PASS real native transport:'
  test=subprocess.Popen(['xcrun','swift','test','--package-path',package,'--filter',name],env=env,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  print('native probe owned daemon='+str(daemon.pid)+' swift='+str(test.pid),flush=True)
  family=runpy.run_path(str(ROOT/'scripts/probe-native-linux-setup.py'))['Family'](test)
  deadline=time.monotonic()+180
  while True:
   if time.monotonic()>=deadline:raise RuntimeError('native transport probe deadline expired')
   family.observe()
   try:output,_=test.communicate(timeout=.2);break
   except subprocess.TimeoutExpired:pass
  print(output,end='')
  if test.returncode or marker not in output or 'Executed 1 test, with 0 failures' not in output:raise RuntimeError('real native transport integration failed')
 finally:
  try:
   if family:family.stop()
   elif test is not None:stop_group(test)
  finally:
   if family:family.close()
   stop_group(daemon);log.close()
   if daemon.returncode not in (0,-signal.SIGTERM):print((runtime/'daemon.log').read_text(),file=sys.stderr)
   shutil.rmtree(runtime)
 return 0
if __name__=='__main__':sys.exit(main())
