#!/usr/bin/env python3
"""Product session lifecycle: native local resume, cancellation and explicit stop."""
import hashlib,os,runpy,shutil,signal,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-coordinator-mutations';MARK='GT_SESSION_COORDINATOR_MUTATION_MARK'
FILES=('apple/GhostermApp/Sources/GhostermAppModel/NativeSessionCoordinator.swift',)
Family=runpy.run_path(str(ROOT/'scripts/probe-native-linux-setup.py'))['Family']
def run(args):
 p=subprocess.Popen([str(a) for a in args],start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);family=Family(p);deadline=time.monotonic()+180
 try:
  while True:
   if time.monotonic()>=deadline:raise RuntimeError('auth scope native deadline expired')
   family.observe()
   try:out,_=p.communicate(timeout=.2);return p.returncode,out
   except subprocess.TimeoutExpired:pass
 finally:
  try:family.stop()
  finally:family.close()
def main():
 originals={n:(ROOT/n).read_bytes() for n in FILES};digests={n:hashlib.sha256(b).digest() for n,b in originals.items()}
 if any(MARK.encode() in b or ((WORK/n).exists() and MARK in (WORK/n).read_text()) for n,b in originals.items()):return 2
 def interrupted(sig,frame):raise RuntimeError('auth scope mutation interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ.update(TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',GHOSTERM_NATIVE_COORDINATOR='1',GHOSTERM_COORDINATOR_TEST_ROOT=str(ROOT))
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'apple').mkdir(parents=True)
 for name in ('GhostermApp','GhostermCore','GhostermTerminal'):shutil.copytree(ROOT/'apple'/name,WORK/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build','assets'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 package=WORK/'apple/GhostermApp';os.environ['GHOSTERM_AUTH_PACKAGE']=str(package)
 def restore():
  for n,b in originals.items():
   (WORK/n).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[n] for p in (ROOT/n,WORK/n)):raise RuntimeError('auth scope SHA256 rollback failed/'+n)
 def probe(red=False,control=False):
  args=['xcrun','swift','test','--package-path',package,'--filter','HostListTests.testReadingDoesNotMutateConfig' if control else 'NativeSessionCoordinatorTests']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and ': failed - ' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary shared auth XCTest red required: '+out[-5000:])
  elif rc or 'with 0 failures' not in out:raise RuntimeError('shared auth baseline/control failed: '+out[-5000:])
 specs=[
 ('stopped preparation not revoked','pane.onStop = { [weak self, weak pane] in if let pane { self?.cancelPreparation(pane) } }','pane.onStop = {}'),
 ('transient record lock rejected','catch SessionRecordStore.Error.busy {','catch SessionRecordStore.Error.busy { throw SessionRecordStore.Error.busy;'),
 ('cancelled preparation still publishes','preparing.removeValue(forKey: pane.id); installers.removeValue(forKey: pane.id)?.cancel()','_ = preparing[pane.id]; installers.removeValue(forKey: pane.id)?.cancel()'),
 ('local cwd ignored','executable: shell ?? "/bin/zsh", cwd: cwd','executable: shell ?? "/bin/zsh", cwd: "/"'),
 ('resume ID lost','options.session = record.session','options.session = 0'),
 ('resume instance lost','options.instance = record.instance','options.instance = Array(repeating: 0, count: 16)'),
 ('explicit close only detaches','self.control(record, operation: .terminate)','self.refresh()'),
 ('closed owner drops pending stop','pane.detachThenRecord { [self] result in\n            DispatchQueue.main.async { [self] in\n                switch result {','pane.detachThenRecord { [weak self] result in\n            DispatchQueue.main.async { [weak self] in\n                guard let self else { return }; switch result {'),
 ]
 specs=[(label,FILES[0],old,new) for label,old,new in specs]

 try:
  restore();probe();probe(control=True)
  for label,name,old,new in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('auth scope mutation anchor drift/'+label)
   try:
    (WORK/name).write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(True);probe(control=True);print('PASS shared auth mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  probe();print('PASS 8 product coordinator mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
