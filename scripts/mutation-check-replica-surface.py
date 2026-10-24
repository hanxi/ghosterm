#!/usr/bin/env python3
"""Native replica bridge mutations: ordinary XCTest failure and green control."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/replica-bridge-mutations';MARK='GT_REPLICA_BRIDGE_MUTATION_MARK'
NAMES=('Sources/CGhostty/replica.inc','Sources/GhostermTerminal/TerminalSurface.swift')
def run(args):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  out,_=p.communicate(timeout=120);return p.returncode,out
 finally:
  if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
  p.wait()
def main():
 source=ROOT/'apple/GhostermTerminal';package=WORK/'apple/GhostermTerminal'
 originals={n:(source/n).read_bytes() for n in NAMES};hashes={n:hashlib.sha256(v).digest() for n,v in originals.items()}
 if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.swift')) or any(MARK in p.read_text() for p in WORK.rglob('*.inc')):return 2
 def interrupted(sig,frame):raise RuntimeError('native replica bridge interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer'
 os.environ['GHOSTERM_REPLICA_FIXTURES']=str(ROOT/'tmp/replica-surface-fixtures')
 if not (Path(os.environ['GHOSTERM_REPLICA_FIXTURES'])/'plain/initial.gtvc').is_file():raise RuntimeError('run scripts/probe-replica-surface.py first')
 if WORK.exists():shutil.rmtree(WORK)
 package.parent.mkdir(parents=True);shutil.copytree(source,package,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 def restore():
  for n,data in originals.items():
   (package/n).write_bytes(data)
   if hashlib.sha256((package/n).read_bytes()).digest()!=hashes[n] or hashlib.sha256((source/n).read_bytes()).digest()!=hashes[n]:raise RuntimeError('native replica SHA256 rollback failed/'+n)
 def probe(name,red=False):
  rc,out=run(['xcrun','swift','test','--package-path',package,'--filter','ReplicaSurfaceTests'+('.'+name if name else '')])
  (WORK/('last-red.log' if red else 'last-green.log')).write_text(out)
  if red:
   if rc!=1 or 'XCTAssert' not in out or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary XCTest red required: '+out[-8000:])
  elif rc or 'with 0 failures' not in out or 'skipped' in out:raise RuntimeError('native bridge baseline/control failed: '+out[-8000:])
 specs=[
 (NAMES[0],'one-shot flag lost','s->replica->imported=true;','s->replica->imported=false;','testCorruptContainerCannotReusePlaceholder','testKnownZeroAndBudgetAdmission'),
 (NAMES[0],'output short circuited','return replica_identity(s,g) && ghostty_p0_surface_event_output_for','return false && replica_identity(s,g) && ghostty_p0_surface_event_output_for','testHalfUTF8AndImageResources','testKnownZeroAndBudgetAdmission'),
 (NAMES[0],'stamped input omitted','if(s->replica && s->cbs.write_stamped)','if(false && s->replica && s->cbs.write_stamped)','testRealImportInputBarrierAndRelease','testKnownZeroAndBudgetAdmission'),
 (NAMES[0],'scope never retired','if(!r)return;','if(!r || r->generation)return;','testRealImportInputBarrierAndRelease','testHalfUTF8AndImageResources'),
 (NAMES[0],'scheme apply falsely acknowledged','bool ok=ghostty_p0_surface_scheme_apply_for(s->surface,g,sequence,config);','bool ok=true; (void)g; (void)sequence;','testOrderedResizeAndColorBarrier','testKnownZeroAndBudgetAdmission'),
 (NAMES[1],'known zero collapsed','if status == 2 { return nil }','if status == 2 || (status == 0 && sequence == 0) { return nil }','testKnownZeroAndBudgetAdmission','testHalfUTF8AndImageResources'),
 (NAMES[1],'requested budget ignored','p.value.memory_bytes = max(0, bytes);','_ = bytes;','testKnownZeroAndBudgetAdmission','testHalfUTF8AndImageResources'),
 ]
 try:
  probe('')
  for name,label,old,new,target,control in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('native bridge mutation anchor drift/'+label)
   try:
    (package/name).write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(target,True);probe(control)
    print('PASS native replica mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  probe('');print('PASS 7 native replica bridge mutations, 5 real Surface tests and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
