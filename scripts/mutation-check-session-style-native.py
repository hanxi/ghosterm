#!/usr/bin/env python3
"""Native ordered config: actual applied state, reload isolation and input barrier."""
import hashlib,os,runpy,shutil,signal,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-style-native-mutations';MARK='GT_SESSION_STYLE_NATIVE_MUTATION_MARK'
FILES=('apple/GhostermApp/Sources/GhostermAppModel/RecoverableSurfaceTransport.swift','apple/GhostermTerminal/Sources/CGhostty/shim.c','apple/GhostermApp/Sources/GhostermAppModel/RecoverablePaneSession.swift')
Family=runpy.run_path(str(ROOT/'scripts/probe-native-linux-setup.py'))['Family']
def run(args):
 p=subprocess.Popen([str(a) for a in args],start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);family=Family(p);deadline=time.monotonic()+180
 try:
  while True:
   if time.monotonic()>=deadline:raise RuntimeError('native style deadline expired')
   family.observe()
   try:out,_=p.communicate(timeout=.2);return p.returncode,out
   except subprocess.TimeoutExpired:pass
 finally:
  try:family.stop()
  finally:family.close()
def main():
 originals={n:(ROOT/n).read_bytes() for n in FILES};digests={n:hashlib.sha256(b).digest() for n,b in originals.items()}
 if any(MARK.encode() in b or ((WORK/n).exists() and MARK in (WORK/n).read_text()) for n,b in originals.items()):return 2
 def interrupted(sig,frame):raise RuntimeError('native style mutation interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ.update(TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',GHOSTERM_NATIVE_TRANSPORT_PACKAGE=str(WORK/'apple/GhostermApp'))
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'apple').mkdir(parents=True)
 for name in ('GhostermApp','GhostermCore','GhostermTerminal'):shutil.copytree(ROOT/'apple'/name,WORK/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build','assets'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 def restore():
  for n,b in originals.items():
   (WORK/n).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[n] for p in (ROOT/n,WORK/n)):raise RuntimeError('native style SHA256 rollback failed/'+n)
 def probe(red=False,control=False):
  args=['xcrun','swift','test','--package-path',WORK/'apple/GhostermApp','--filter','HostListTests.testReadingDoesNotMutateConfig'] if control else [sys.executable,ROOT/'scripts/probe-recoverable-transport.py','--pane']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and ': failed - ' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary native style assertion red required: '+out[-5000:])
  elif rc or 'with 0 failures' not in out or (not control and 'PASS durable native pane:' not in out):raise RuntimeError('native style baseline/control failed: '+out[-5000:])
 specs=[
 ('input enabled before ordered style applied',FILES[0],'if !enabled && styleReady {','if !enabled {'),
 # Reproduce the forbidden target state of an App-wide untagged update, without
 # replacing the validated SDK or relying on a crash as mutation evidence.
 ('replica receives unsequenced global reload',FILES[1],'  ghostty_app_update_config(a->app, fresh);','  ghostty_app_update_config(a->app, fresh);\n  for(size_t i=0;i<a->surface_count;i++)if(a->surfaces[i] && a->surfaces[i]->replica)ghostty_surface_update_config(a->surfaces[i]->surface,fresh);'),
 ('LIVE input revoked during local config barrier',FILES[0],'if sentStyle == style { styleSequence = s }','if sentStyle == style { styleSequence = s }; _ = surface.setReplicaInput(generation: g, sequence: 0, enabled: false)'),
 ('configuration observer disconnected',FILES[2],'self.transport.requestStyle(style)','_ = style'),
 ]
 try:
  restore();probe();probe(control=True)
  for label,name,old,new in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('native style mutation anchor drift/'+label)
   try:
    (WORK/name).write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(True);probe(control=True);print('PASS native style mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  probe();print('PASS 4 native style mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
