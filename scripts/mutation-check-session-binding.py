#!/usr/bin/env python3
"""Native Swift session binding: isolated SPM package and XCTest red/green."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];SOURCE=ROOT/'apple/GhostermCore';WORK=ROOT/'tmp/session-binding-mutations';MARK='GT_SESSION_BINDING_MUTATION_MARK';REL=Path('Sources/GhostermCore/RecoverableSession.swift')
def run(args,timeout=150):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
 try:out,_=p.communicate(timeout=timeout);return p.returncode,out
 finally:
  if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
  p.wait()
def main():
 original=(SOURCE/REL).read_bytes();digest=hashlib.sha256(original).digest();copy=WORK/'GhostermCore'/REL
 if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
 def interrupted(sig,frame):raise RuntimeError('Swift session binding mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer'
 package=WORK/'GhostermCore'
 if package.exists():shutil.rmtree(package)
 shutil.copytree(SOURCE,package,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 p=package/'Package.swift';s=p.read_text().replace('let coreBuild = repoRoot.appendingPathComponent("core/build").path','let coreBuild = "'+str(ROOT/'core/build')+'"');p.write_text(s)
 def restore():
  copy.write_bytes(original)
  if hashlib.sha256(copy.read_bytes()).digest()!=digest or (SOURCE/REL).read_bytes()!=original:raise RuntimeError('Swift binding SHA256 rollback failed')
 def probe(name,red=False):
  rc,out=run(['xcrun','swift','test','--package-path',package,'--filter',name])
  (WORK/('last-'+('red' if red else 'green')+'.log')).write_text(out)
  if red:
   if rc!=1 or 'XCTAssert' not in out or 'failed' not in out or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary XCTest assertion/exit 1 required: '+out[-7000:])
  elif rc:raise RuntimeError('Swift session baseline/control failed: '+out[-7000:])
 specs=[
 ('cancel discarded','public func cancel() { checkOwner(); gt_session_cancel(raw) }','public func cancel() { checkOwner(); _ = raw }','testLifecycleAndInputLock'),
 ('close falsely succeeds in callback','guard gt_session_close(raw) else { return false }','guard gt_session_close(raw) else { return true }','testAsyncAuthenticationAdmissionAndReentry'),
 ('attempt metadata changed','authentication?.requested(id, attempt)','authentication?.requested(id, attempt + 1)','testAsyncAuthenticationAdmissionAndReentry'),
 ('stale credential admission hidden','guard result == GT_OK else','guard result != GT_OK else','testAsyncAuthenticationAdmissionAndReentry'),
 ('NUL metadata silently truncated','!texts.contains(where: { $0.utf8.contains(0) })','!texts.contains(where: { $0.utf8.contains(255) })','testRejectsInvalidMetadata'),
 ('nonce identity length lost','options.create?.nonce.count ?? 16 == 16','options.create?.nonce.count ?? 16 >= 1','testRejectsInvalidMetadata'),
 ]
 try:
  restore();probe('RecoverableSessionTests');probe('GhostermCoreTests.testVersionAndError')
  for label,old,new,target in specs:
   text=original.decode()
   if text.count(old)!=1:raise RuntimeError('Swift session mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe('RecoverableSessionTests.'+target,True);probe('GhostermCoreTests.testVersionAndError');print('PASS Swift session mutation/'+label+': ordinary XCTest red, same-mutant control green',flush=True)
   finally:restore()
  probe('RecoverableSessionTests');print('PASS 6 Swift session mutations, 5 native baselines and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
