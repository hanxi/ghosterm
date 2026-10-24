#!/usr/bin/env python3
"""Native Linux setup, credential scopes and prebuilt installation outcome."""
import argparse, hashlib, os, shutil, signal, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-setup-transport-mutations';MARK='GT_SESSION_SETUP_TRANSPORT_MUTATION_MARK'
FILES=('Sources/GhostermAppModel/SessionSetupTransport.swift','Sources/GhostermAppModel/SessionAuthenticationFlow.swift')
def run(args):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  out,_=p.communicate(timeout=180);return p.returncode,out
 finally:
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=45)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("--only",type=int);options=parser.parse_args()
 originals={name:(ROOT/'apple/GhostermApp'/name).read_bytes() for name in FILES};digests={n:hashlib.sha256(b).digest() for n,b in originals.items()};package=WORK/'apple/GhostermApp'
 if any(MARK.encode() in b or ((package/n).exists() and MARK in (package/n).read_text()) for n,b in originals.items()):return 2
 def interrupted(sig,frame):raise RuntimeError('setup Transport mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer';os.environ['GHOSTERM_NATIVE_SETUP_PACKAGE']=str(package)
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'apple').mkdir(parents=True)
 for name in ('GhostermApp','GhostermCore','GhostermTerminal'):shutil.copytree(ROOT/'apple'/name,WORK/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build','assets'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 def restore():
  for name,b in originals.items():
   (package/name).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[name] for p in (package/name,ROOT/'apple/GhostermApp'/name)):raise RuntimeError('setup Transport SHA256 rollback failed/'+name)
 def probe(red=False,unit=False,control=False):
  test='HostListTests.testReadingDoesNotMutateConfig' if control else 'SessionSetupTransportTests.testCancellationBeforeStartNeverRequestsCredentials'
  args=['xcrun','swift','test','--package-path',package,'--filter',test] if control or unit else [sys.executable,ROOT/'scripts/probe-native-linux-setup.py']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and ': failed - ' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary native setup assertion red required: '+out[-6000:])
  elif rc or 'with 0 failures' not in out or (not control and not unit and 'PASS Mac-to-Linux native setup:' not in out):raise RuntimeError('native setup baseline/control failed: '+out[-6000:])
 specs=[
  ('cancel before start ignored',FILES[0],'        cancellation.cancel()\n        owner.async { [self] in if !installing { finish(.failure(Failure.cancelled(installation))) } }','        _ = self',True),
  ('duplicate credential replaces accepted material',FILES[1],'self.request == id, self.prompting, !scope.isCancelled else { return }\n                        // Consume once','self.request == id else { return }\n                        // Consume once',False),
  ('duplicate hostkey acceptance reaches Core',FILES[1],'self.request == id, self.prompting, !scope.isCancelled else { return }\n                        self.prompting = false; scope.cancel()\n                        if decision','self.request == id else { return }\n                        self.prompting = false; scope.cancel()\n                        if decision',False),
  ('bundle contract not checked',FILES[0],'header[5] == Substring(contract)','true',False),
  ('architecture bundle selection lost',FILES[0],'bundles[linux.arch]','bundles["unsupported-architecture"]',False),
  ('failed install reported as success',FILES[0],'guard result.status == 0 else','guard true else',False),
  ('successful credential index fabricated',FILES[0],'acceptedAttempt: attempt','acceptedAttempt: 0',False),
 ]
 if options.only is not None:specs=[specs[options.only]]
 try:
  restore();probe();probe(unit=True);probe(control=True)
  for label,name,old,new,unit in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('native setup mutation anchor drift/'+label)
   try:
    (package/name).write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(red=True,unit=unit);probe(control=True)
    print('PASS native setup Transport mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  probe();probe(unit=True);print('PASS '+str(len(specs))+' native setup Transport mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
