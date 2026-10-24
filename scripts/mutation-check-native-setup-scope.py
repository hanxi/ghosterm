#!/usr/bin/env python3
"""Protect the directories the SSH fixture is allowed to remove."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/native-setup-scope-mutations';MARK='GT_NATIVE_SETUP_SCOPE_MUTATION_MARK'
NAMES=('scripts/probe-native-linux-setup.py','scripts/test-native-setup-scope.py')
def main():
 originals={n:(ROOT/n).read_bytes() for n in NAMES}
 if any(MARK.encode() in b or ((WORK/n).exists() and MARK in (WORK/n).read_text()) for n,b in originals.items()):return 2
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,lambda sig,frame:(_ for _ in ()).throw(RuntimeError('scope mutation interrupted '+str(sig))))
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'scripts').mkdir(parents=True)
 def restore():
  for n,b in originals.items():
   (WORK/n).write_bytes(b)
   assert all(hashlib.sha256(p.read_bytes()).digest()==hashlib.sha256(b).digest() for p in (ROOT/n,WORK/n))
 def run(red=False,control=False):
  args=['python3',str(WORK/NAMES[1])]+(['ScopeTests.test_legacy_siblings'] if control else [])
  p=subprocess.Popen(args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
  try:out,_=p.communicate(timeout=10)
  finally:
   if p.poll() is None:os.killpg(p.pid,signal.SIGKILL);p.wait()
  (WORK/('last-red.log' if red else 'last-control.log' if control else 'last-baseline.log')).write_text(out)
  if red:
   if p.returncode!=1 or 'FAILED (failures=' not in out:raise RuntimeError('ordinary scope assertion/exit1 required: '+out)
  elif p.returncode or 'OK' not in out:raise RuntimeError('scope baseline/control failed: '+out)
 specs=(
  ('foreign runtime accepted','runtime.relative_to(scope)','runtime.relative_to(runtime.parent)'),
  ('parent traversal accepted'," and '..' not in p.parts",''),
  ('unowned install name accepted',"not all(p.name.startswith('ghosterm-test-') for p in (scope,install))",'False'),
  ('foreign install parent accepted','scope.parent!=install.parent','False'),
  ('unbounded runtime subtree accepted','len(relative.parts)>4','len(relative.parts)>100'),
 )
 try:
  restore();run();run(control=True)
  for label,old,new in specs:
   source=originals[NAMES[0]].decode();assert source.count(old)>=1
   try:
    (WORK/NAMES[0]).write_text(source.replace(old,new,1)+'\n# '+MARK+'\n');run(True);run(control=True)
    print('PASS fixture scope mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  run();print('PASS 5 fixture scope mutations and SHA256 rollback')
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
