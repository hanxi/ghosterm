#!/usr/bin/env python3
"""Native Swift session binding: isolated SPM package and XCTest red/green."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];SOURCE=ROOT/'apple/GhostermCore';WORK=ROOT/'tmp/session-native-mutations';MARK='GT_SESSION_NATIVE_MUTATION_MARK';REL=Path('Sources/GhostermCore/SessionSetup.swift')
def run(args,timeout=150):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
 try:out,_=p.communicate(timeout=timeout);return p.returncode,out
 finally:
  try:os.killpg(p.pid,signal.SIGKILL)
  except ProcessLookupError:pass
  p.wait()
def main():
 names=('SessionSetup.swift','SessionAddressResolver.swift'); originals={n:(SOURCE/'Sources/GhostermCore'/n).read_bytes() for n in names}; original=originals['SessionSetup.swift'];digest=hashlib.sha256(original).digest();copy=WORK/'GhostermCore'/REL
 if any(MARK.encode() in b for b in originals.values()) or any(MARK in p.read_text() for p in WORK.rglob('*.swift')):return 2
 def interrupted(sig,frame):raise RuntimeError('Swift session binding mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer'
 package=WORK/'GhostermCore'
 if package.exists():shutil.rmtree(package)
 shutil.copytree(SOURCE,package,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 p=package/'Package.swift';s=p.read_text().replace('let coreBuild = repoRoot.appendingPathComponent("core/build").path','let coreBuild = "'+str(ROOT/'core/build')+'"');p.write_text(s)
 driver=WORK/'native-driver.sh'
 driver.write_text('''#!/usr/bin/env bash
set -euo pipefail
export GHOSTERM_SETUP_HOST="$1" GHOSTERM_SETUP_PORT="$2" GHOSTERM_SETUP_USER="$3" GHOSTERM_SETUP_KEY="$4" GHOSTERM_SETUP_WORK="$5"
xcrun swift test --package-path "$GHOSTERM_NATIVE_PACKAGE" --filter SessionSetupTests.testRealSSHSetup
''');driver.chmod(0o700);os.environ['GHOSTERM_NATIVE_PACKAGE']=str(package)
 def restore():
  for n,data in originals.items():
   p=package/'Sources/GhostermCore'/n;p.write_bytes(data)
   if hashlib.sha256(p.read_bytes()).digest()!=hashlib.sha256(data).digest() or (SOURCE/'Sources/GhostermCore'/n).read_bytes()!=data:raise RuntimeError('Swift native SHA256 rollback failed/'+n)

 def probe(name,red=False):
  rc,out=run(['bash',ROOT/'core/tests/scripts/ssh_e2e.sh',driver] if name=='real' else ['xcrun','swift','test','--package-path',package,'--filter',name])
  (WORK/('last-'+('red' if red else 'green')+'.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and 'error: -[' not in out) or 'failed' not in out or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary XCTest assertion/exit 1 required: '+out[-7000:])
  elif rc or (name=='real' and 'PASS native async resolver/authenticator/take/Linux capability/cancellation' not in out):raise RuntimeError('Swift native baseline/control failed: '+out[-7000:])
 specs=[
 ('SessionSetup.swift','setup take dropped','guard gt_session_authenticator_take(raw, &ssh), let ssh else','guard false && gt_session_authenticator_take(raw, &ssh), let ssh else','real'),
 ('SessionSetup.swift','host identity changed','host: Host(rawValue: info.host.rawValue) ?? .unchecked','host: .known','real'),
 ('SessionSetup.swift','setup cancellation discarded','{ p in Unmanaged<Callbacks>.fromOpaque(p!).takeUnretainedValue().cancellation.isCancelled }','{ p in _ = p; return false }','real'),
 ('SessionSetup.swift','busy close falsely successful','guard !busy else { return false }','guard !busy else { return true }','real'),
 ('SessionSetup.swift','installer failure hidden','Installation(status: Int32(error.rawValue), reused:','Installation(status: 0, reused:','real'),
 ('SessionAddressResolver.swift','IPv4 byte order reversed','withUnsafeBytes(of: ipv4) { Array($0) }','withUnsafeBytes(of: ipv4) { Array($0.reversed()) }','SessionAddressResolverTests.testIPv4BinaryAddress'),
 ('SessionAddressResolver.swift','IPv6 scope omitted','}, scope: scope)))','}, scope: 0)))','SessionAddressResolverTests.testScopedIPv6'),
 ('SessionAddressResolver.swift','resolver cancel discarded','public func cancel() { checkOwner(); finish(nil) }','public func cancel() { checkOwner(); _ = finished }','SessionAddressResolverTests.testCancelRevokesCompletionAndReleasesOwner'),
 ('SessionAddressResolver.swift','resolver owner leaked','completion = nil; keepAlive = nil','completion = nil; keepAlive = self','SessionAddressResolverTests.testCancelRevokesCompletionAndReleasesOwner'),
 ('SessionAddressResolver.swift','zero DNS budget accepted','timeout > 0, timeout <= 60000','timeout >= 0, timeout <= 60000','SessionAddressResolverTests.testInvalidHostAndDeadlineParameters'),
 ('SessionAddressResolver.swift','DNS budget cap removed','timeout > 0, timeout <= 60000','timeout > 0','SessionAddressResolverTests.testInvalidHostAndDeadlineParameters'),
 ]
 try:
  restore();probe('SessionAddressResolverTests');probe('SessionSetupTests.testAuthenticatorOwnsFreshCredentialsAndCloses');probe('real');probe('GhostermCoreTests.testVersionAndError')
  for name,label,old,new,target in specs:
   text=originals[name].decode();copy=package/'Sources/GhostermCore'/name
   if text.count(old)!=1:raise RuntimeError('Swift session mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(target,True);probe('GhostermCoreTests.testVersionAndError');print('PASS Swift session mutation/'+label+': ordinary XCTest red, same-mutant control green',flush=True)
   finally:restore()
  probe('SessionAddressResolverTests');probe('real');print('PASS 11 native setup/resolver mutations, real sshd, native DNS and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
