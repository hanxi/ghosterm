#!/usr/bin/env python3
"""Actual Mach-O dependency mutation: absolute install ID must not be shipped."""
import hashlib,os,runpy,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-runtime-builder-mutation';MARK='GT_RUNTIME_BUILDER_MUTATION_MARK'
def run(args):
 p=subprocess.Popen([str(x) for x in args],env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer'),start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:out,_=p.communicate(timeout=180);return p.returncode,out
 finally:
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=10)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def main():
 source=ROOT/'scripts/build-sessiond-mac.py';original=source.read_bytes();digest=hashlib.sha256(original).digest();copy=WORK/'builder.py'
 if MARK.encode() in original or copy.exists() and MARK in copy.read_text():return 2
 def interrupted(sig,frame):raise RuntimeError('runtime builder mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 if WORK.exists():shutil.rmtree(WORK)
 WORK.mkdir();engine=WORK/'absolute-engine.dylib';shutil.copyfile(ROOT/'tmp/worker-surface/out/lib/libghostty-vt.dylib',engine)
 for args in (['install_name_tool','-id',engine,engine],['codesign','--force','--sign','-',engine]):
  rc,out=run(args)
  if rc:raise RuntimeError(out)
 driver=WORK/'driver.py';driver.write_text('import runpy,sys\nfrom pathlib import Path\nd=runpy.run_path(sys.argv[1]);d["main"].__globals__["ROOT"]=Path(sys.argv[2]);sys.argv=[sys.argv[1],*sys.argv[3:]];sys.exit(d["main"]())\n')
 def restore():
  copy.write_bytes(original)
  if any(hashlib.sha256(p.read_bytes()).digest()!=digest for p in (source,copy)):raise RuntimeError('runtime builder SHA256 rollback failed')
 gate=WORK/'gate.py';gate.write_text('import subprocess,sys\np=subprocess.run(sys.argv[1:],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)\nassert p.returncode==1 and "helper dependency is not self contained:" in p.stdout, "absolute dependency was incorrectly published"\nprint("PASS bad absolute dependency rejected")\n')
 def check_bad(red=False):
  command=[sys.executable,gate,sys.executable,driver,copy,ROOT,'--engine',engine,'--out',WORK/'out-bad']
  rc,out=run(command);(WORK/('red.log' if red else 'bad.log')).write_text(out)
  if red:
   if rc!=1 or 'AssertionError: absolute dependency was incorrectly published' not in out:raise RuntimeError('ordinary dependency gate FAIL/exit 1 required: '+out[-3000:])
  elif rc or 'PASS bad absolute dependency rejected' not in out:raise RuntimeError('real absolute dependency was not rejected by its guard: '+out[-3000:])
 def control():
  rc,out=run([sys.executable,driver,copy,ROOT,'--out',WORK/'out-good']);(WORK/'control.log').write_text(out)
  if rc or 'PASS self-contained macOS session helpers:' not in out:raise RuntimeError('same-mutant valid payload control failed: '+out[-3000:])
 try:
  restore();check_bad();control()
  old="if not (dep.startswith('/usr/lib/') or dep == '@rpath/libghostty-vt.dylib'):"
  if original.decode().count(old)!=1:raise RuntimeError('runtime dependency mutation anchor drift')
  try:
   copy.write_text(original.decode().replace(old,'if False:')+'\n# '+MARK+'\n');check_bad(True);control()
  finally:restore()
  check_bad();control();print('PASS real Mach-O runtime dependency mutation, same-mutant green and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
