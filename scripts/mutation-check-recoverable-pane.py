#!/usr/bin/env python3
"""Durable native pane lifecycle and actual production RootView replacement."""
import hashlib, os, shutil, signal, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/recoverable-pane-mutations';MARK='GT_RECOVERABLE_PANE_MUTATION_MARK'
FILES=('Sources/GhostermApp/RootView.swift','Sources/GhostermAppModel/TerminalTab.swift','Sources/GhostermAppModel/RecoverablePaneSession.swift')
def run(args):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  out,_=p.communicate(timeout=180);return p.returncode,out
 finally:
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=15)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def main():
 originals={name:(ROOT/'apple/GhostermApp'/name).read_bytes() for name in FILES};digests={n:hashlib.sha256(b).digest() for n,b in originals.items()};package=WORK/'apple/GhostermApp'
 if any(MARK.encode() in b or ((package/n).exists() and MARK in (package/n).read_text()) for n,b in originals.items()):return 2
 def interrupted(sig,frame):raise RuntimeError('recoverable pane mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer';os.environ['GHOSTERM_NATIVE_TRANSPORT_PACKAGE']=str(package)
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'apple').mkdir(parents=True)
 for name in ('GhostermApp','GhostermCore','GhostermTerminal'):shutil.copytree(ROOT/'apple'/name,WORK/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build','assets'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 def restore():
  for name,b in originals.items():
   (package/name).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[name] for p in (package/name,ROOT/'apple/GhostermApp'/name)):raise RuntimeError('pane SHA256 rollback failed/'+name)
 def probe(red=False,unit=False,control=False):
  test='HostListTests.testReadingDoesNotMutateConfig' if control else 'RecoverablePaneTests.testRecoveryStatusPreservesUnknownAndRequiresCloseConfirmation'
  args=['xcrun','swift','test','--package-path',package,'--filter',test] if control or unit else [sys.executable,ROOT/'scripts/probe-recoverable-transport.py','--pane']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or 'XCTAssert' not in out or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary pane assertion red required: '+out[-6000:])
  elif rc or 'with 0 failures' not in out or (not control and not unit and 'PASS durable native pane:' not in out):raise RuntimeError('pane baseline/control failed: '+out[-6000:])
 specs=[
  ('native view retains old Surface',FILES[0],'.id(ObjectIdentifier(surface))','.id("unchanged-native-surface")',False),
  ('admitted CREATE replay allowed',FILES[2],'!record.admitted, record.session == 0, record.instance.allSatisfy({ $0 == 0 })','true',False),
  ('record identity never published',FILES[2],'self.pane?.acceptRecoverableRecord(record)','_ = record',False),
  ('unknown exit fabricated zero',FILES[1],'case .exitedUnknown: return "已结束（退出码未知）"','case .exitedUnknown: return "已退出（0）"',True),
  ('recovering silently considered idle',FILES[1],'case .recovering: return true','case .recovering: return false',True),
 ]
 try:
  restore();probe();probe(unit=True);probe(control=True)
  for label,name,old,new,unit in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('pane mutation anchor drift/'+label)
   try:
    (package/name).write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(red=True,unit=unit);probe(control=True)
    print('PASS native pane mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  probe();probe(unit=True);print('PASS 5 durable native pane mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
