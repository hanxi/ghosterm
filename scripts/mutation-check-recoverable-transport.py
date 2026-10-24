#!/usr/bin/env python3
"""Real native transport: isolated package mutations and same-mutant controls."""
import argparse,hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/recoverable-transport-mutations';MARK='GT_RECOVERABLE_TRANSPORT_MUTATION_MARK'
REL=Path('Sources/GhostermAppModel/RecoverableSurfaceTransport.swift')
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
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--admission',action='store_true');options=parser.parse_args()
 source=ROOT/'apple/GhostermApp'/REL;original=source.read_bytes();digest=hashlib.sha256(original).digest();package=WORK/'apple/GhostermApp';copy=package/REL
 if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
 def interrupted(sig,frame):raise RuntimeError('recoverable transport mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ['DEVELOPER_DIR']='/Applications/Xcode.app/Contents/Developer';os.environ['GHOSTERM_NATIVE_TRANSPORT_PACKAGE']=str(package)
 if WORK.exists():shutil.rmtree(WORK)
 (WORK/'apple').mkdir(parents=True)
 for name in ('GhostermApp','GhostermCore','GhostermTerminal'):shutil.copytree(ROOT/'apple'/name,WORK/'apple'/name,symlinks=True,ignore=shutil.ignore_patterns('.build','.swiftpm'))
 for name in ('core','terminal','build','assets'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 def restore():
  copy.write_bytes(original)
  if hashlib.sha256(copy.read_bytes()).digest()!=digest or hashlib.sha256(source.read_bytes()).digest()!=digest:raise RuntimeError('recoverable transport SHA256 rollback failed')
 def probe(red=False,control=False):
  args=['xcrun','swift','test','--package-path',package,'--filter','HostListTests.testReadingDoesNotMutateConfig'] if control else [sys.executable,ROOT/'scripts/probe-recoverable-transport.py']+(['--admission'] if options.admission else [])
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or 'XCTAssert' not in out or 'Build complete!' not in out or '0 unexpected' not in out or 'unexpected signal' in out or 'Program crashed' in out:raise RuntimeError('ordinary native transport assertion red required: '+out[-8000:])
  elif rc or 'with 0 failures' not in out or (not control and ('PASS durable native admission:' if options.admission else 'PASS real native transport:') not in out):raise RuntimeError('native transport baseline/control failed: '+out[-8000:])
 specs=[
 ('snapshot ignored','guard surface.importReplica(generation: g, sequence: s, bytes: bytes) else','guard true else'),
 ('input never enabled','enabled = surface.setReplicaInput(generation: generation, sequence: applied.sequence, enabled: true)','enabled = true'),
 ('encoded sequence overwritten','session.input(generation: first.0, sequence: first.1, bytes: $0)','session.input(generation: first.0, sequence: 0, bytes: $0)'),
 ('input fragment omitted','stride(from: 0, to: bytes.count, by: 32768)','stride(from: 0, to: bytes.count, by: 65536)'),
 ('output falsely accepted','accepted = surface.applyReplicaOutput(generation: g, sequence: s, bytes: bytes)','accepted = true; _ = bytes'),
 ]
 if options.admission:
  specs=[
   ('admission expiry ignored','DispatchTime.now().uptimeNanoseconds >= admissionDeadline','false'),
   ('durable admission bypassed','needsAdmission = options.create != nil && callbacks.admission != nil','needsAdmission = false'),
   ('CREATE stepped while persistence pending','if admissionPending != nil {','if false && admissionPending != nil {'),
   ('failed persistence continues CREATE','self.reportFailure("无法保存会话记录：\\(error.localizedDescription)"); self.detachOnOwner()','self.reportFailure("无法保存会话记录：\\(error.localizedDescription)"); self.needsAdmission = false'),
  ]
 try:
  restore();probe();probe(control=True)
  for label,old,new in specs:
   text=original.decode()
   if text.count(old)!=1:raise RuntimeError('native transport mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n// '+MARK+'\n');probe(red=True);probe(control=True)
    print('PASS native transport mutation/'+label+': real daemon/native Surface ordinary red, same-mutant control green',flush=True)
   finally:restore()
  probe();print('PASS '+str(len(specs))+' native transport mutations, real PTY recovery and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
