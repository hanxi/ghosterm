#!/usr/bin/env python3
"""Engine-free inspection, durable explicit stop and observed termination."""
import hashlib, os, shutil, signal, subprocess, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-control-transport-mutations';MARK='GT_SESSION_CONTROL_TRANSPORT_MUTATION_MARK'
FILES=('Sources/GhostermAppModel/SessionControlTransport.swift','Sources/GhostermAppModel/SessionRecordContext.swift')
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
 def interrupted(sig,frame):raise RuntimeError('control Transport mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer';os.environ['GHOSTERM_NATIVE_TRANSPORT_PACKAGE']=str(package)
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'apple').mkdir(parents=True)
 for name in ('GhostermApp','GhostermCore','GhostermTerminal'):shutil.copytree(ROOT/'apple'/name,WORK/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build','assets'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 def restore():
  for name,b in originals.items():
   (package/name).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[name] for p in (package/name,ROOT/'apple/GhostermApp'/name)):raise RuntimeError('control Transport SHA256 rollback failed/'+name)
 def probe(red=False,unit=False,control=False):
  test='HostListTests.testReadingDoesNotMutateConfig' if control else 'SessionControlTransportTests.testMetadataContextRetriesBriefLockContentionBeforeAdmission'
  args=['xcrun','swift','test','--package-path',package,'--filter',test] if control or unit else [sys.executable,ROOT/'scripts/probe-recoverable-transport.py','--control']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or ('XCTAssert' not in out and ': failed - ' not in out) or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary control Transport assertion red required: '+out[-6000:])
  elif rc or 'with 0 failures' not in out or (not control and not unit and 'PASS native control Transport:' not in out):raise RuntimeError('control Transport baseline/control failed: '+out[-6000:])
 specs=[
  ('brief writer contention fails admission',FILES[1],'let deadline = ProcessInfo.processInfo.systemUptime + 4','let deadline = ProcessInfo.processInfo.systemUptime + 0',True),
  ('pending nonce changed',FILES[0],'options.pendingNonce = nonce','options.pendingNonce = nonce; options.pendingNonce[0] ^= 255',False),
  ('resolved creation identity ignored',FILES[0],'stage == .lookup, info.session > 0','stage == .lookup, false',False),
  ('stop intent not persisted',FILES[0],'context.prepareStop {','context.persistIntent {',False),
  ('ACK fabricated process result',FILES[0],'if row?.hasResult == true {','if lastStop != nil || row?.hasResult == true {',False),
  ('ended source terminated again',FILES[0],'if row?.hasResult == true {','if row?.hasResult == true && stage != .catalog {',False),
  ('observer storage failure hidden',FILES[1],'if let observationError { throw observationError }\n            completion(.success','if let observationError, false { throw observationError }\n            completion(.success',False),
 ]
 try:
  restore();probe();probe(unit=True);probe(control=True)
  for label,name,old,new,unit in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('control Transport mutation anchor drift/'+label)
   try:
    (package/name).write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(red=True,unit=unit);probe(control=True)
    print('PASS native control Transport mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  probe();probe(unit=True);print('PASS 7 native control Transport mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
