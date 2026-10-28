#!/usr/bin/env python3
"""Platform credential selection: method gating, cancellation and accepted-only caching."""
import hashlib,os,runpy,shutil,signal,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/ssh-credential-mutations';MARK='GT_SSH_CREDENTIAL_MUTATION_MARK'
FILES=('apple/GhostermApp/Sources/GhostermAppModel/SSHCredentialSelector.swift',)
def run(args,timeout=180):
 wrapper='. "$1"; shift; gt_timeout_require auth-mutation; gt_timeout "$@"'
 p=subprocess.Popen(['bash','-c',wrapper,'auth-mutation',str(ROOT/'scripts/lib/gt-timeout.sh'),str(timeout),*[str(a) for a in args]],cwd=ROOT,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:
  out,_=p.communicate();return p.returncode,out
 finally:
  if p.poll() is None:
   try:os.killpg(p.pid,signal.SIGTERM)
   except ProcessLookupError:pass
   p.wait()
def main():
 originals={n:(ROOT/n).read_bytes() for n in FILES};digests={n:hashlib.sha256(b).digest() for n,b in originals.items()}
 if any(MARK.encode() in b or ((WORK/n).exists() and MARK in (WORK/n).read_text()) for n,b in originals.items()):return 2
 def interrupted(sig,frame):raise RuntimeError('auth scope mutation interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ.update(TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer')
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
  args=['xcrun','swift','test','--package-path',package,'--filter','HostListTests.testReadingDoesNotMutateConfig' if control else 'SSHCredentialSelectorTests']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and ': failed - ' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary shared auth XCTest red required: '+out[-5000:])
  elif rc or 'with 0 failures' not in out:raise RuntimeError('shared auth baseline/control failed: '+out[-5000:])
 specs=[
 ('method discovery bypassed','guard let methods = offered, methods.0 == id, methods.1 != 0 else','guard let methods = offered ?? (id, 1, 0) as (UInt64, Int32, Int32)? else'),
 ('late credential storage result survives cancel','pending === request && !request.cancellation.isCancelled','true'),
 ('acceptance belongs to wrong request','guard let value = selection, value.id == id else','guard let value = selection else'),
 ('rejected secret not removed','try storage.remove(rejected, target)','_ = rejected; _ = target; _ = storage'),
 ('server password mask ignored','if SSHAuthMethods(rawValue: methods.1).contains(.publicKey),','if true,'),
 ('unknown format silently treated as unencrypted','if requirement == .unknown {','if false {'),
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
  probe();print('PASS 6 platform credential mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
