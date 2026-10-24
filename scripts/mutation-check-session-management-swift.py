#!/usr/bin/env python3
"""Native management binding, real daemon metadata, isolated XCTest mutations."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-management-swift-mutations';SOURCE=ROOT/'apple/GhostermCore';REL=Path('Sources/GhostermCore/RecoverableSession.swift');MARK='GT_MANAGEMENT_SWIFT_MUTATION_MARK'
def run(args):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:out,_=p.communicate(timeout=150);return p.returncode,out
 finally:
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=15)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def main():
 package=WORK/'GhostermCore';copy=package/REL;original=(SOURCE/REL).read_bytes();digest=hashlib.sha256(original).digest()
 if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
 def interrupted(sig,frame):raise RuntimeError('native management mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer';os.environ['GHOSTERM_NATIVE_MANAGEMENT_PACKAGE']=str(package)
 if package.exists():shutil.rmtree(package)
 shutil.copytree(SOURCE,package,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 p=package/'Package.swift';p.write_text(p.read_text().replace('let coreBuild = repoRoot.appendingPathComponent("core/build").path','let coreBuild = "'+str(ROOT/'core/build')+'"'))
 def restore():
  copy.write_bytes(original)
  if hashlib.sha256(copy.read_bytes()).digest()!=digest or (SOURCE/REL).read_bytes()!=original:raise RuntimeError('native management SHA256 rollback failed')
 def probe(name,red=False):
  rc,out=run([sys.executable,ROOT/'scripts/probe-recoverable-transport.py','--management'] if name=='real' else ['xcrun','swift','test','--package-path',package,'--filter',name])
  (WORK/('last-red.log' if red else 'last-green.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and 'error: -[' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary management XCTest assertion red required: '+out[-7000:])
  elif rc or (name=='real' and 'PASS native session management:' not in out):raise RuntimeError('native management baseline/control failed: '+out[-7000:])
 specs=[
 ('catalog flag lost','config.catalog_only = options.catalogOnly','config.catalog_only = false','SessionManagementTests.testCatalogNeedsNoSurfaceAndPreservesUnknownList'),
 ('pending identity lost','$0.copyBytes(from: options.pendingNonce)','$0.copyBytes(from: Array(repeating: UInt8(0), count: 16))','SessionManagementTests.testPendingLookupRequiresOriginalInstanceAndRejectsMixedModes'),
 ('unknown list invented empty','if result == GT_ERR_NOT_FOUND { return nil }','if result == GT_ERR_NOT_FOUND { return [] }','SessionManagementTests.testCatalogNeedsNoSurfaceAndPreservesUnknownList'),
 ('create admission hidden','hasAdmittedCreate: i.has_admitted_create','hasAdmittedCreate: false','SessionManagementTests.testPendingLookupRequiresOriginalInstanceAndRejectsMixedModes'),
 ('terminate target replaced','gt_session_terminate(raw, session)','gt_session_terminate(raw, 0)','SessionManagementTests.testCatalogNeedsNoSurfaceAndPreservesUnknownList'),
 ('unknown stop folded known','StopState(rawValue: i.stop.state.rawValue) ?? .unknown','(i.stop.state == GT_SESSION_STOP_UNKNOWN ? .known : (StopState(rawValue: i.stop.state.rawValue) ?? .unknown))','real'),
 ('stop error concealed','error: Int32(i.stop.error.rawValue)','error: 0','real'),
 ('stop target metadata lost','session: i.stop.session, request:','session: 0, request:','real'),
 ('row result invented known','hasResult: $0.has_result','hasResult: true','real'),
 ('row unknown exit invented known','hasExitCode: $0.has_exit_code','hasExitCode: true','real'),
 ]
 try:
  restore();probe('SessionManagementTests.testCatalogNeedsNoSurfaceAndPreservesUnknownList');probe('SessionManagementTests.testPendingLookupRequiresOriginalInstanceAndRejectsMixedModes');probe('real');probe('GhostermCoreTests.testVersionAndError')
  for label,old,new,target in specs:
   text=original.decode()
   if text.count(old)!=1:raise RuntimeError('native management mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(target,True);probe('GhostermCoreTests.testVersionAndError')
    print('PASS native management mutation/'+label+': ordinary XCTest red and same-mutant control green',flush=True)
   finally:restore()
  probe('real');print('PASS 10 native management mutations, real daemon catalog/explicit stop and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
