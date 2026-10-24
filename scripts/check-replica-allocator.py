#!/usr/bin/env python3
"""Allocator-only GUI object must contain no headless VT runtime references."""
import argparse,hashlib,os,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/replica-allocator';MARK='GT_REPLICA_ALLOCATOR_MUTATION_MARK'
def run(args,check=True):
 p=subprocess.Popen([str(x) for x in args],start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
 try:
  out,err=p.communicate(timeout=30);result=subprocess.CompletedProcess(args,p.returncode,out,err)
  if check:result.check_returncode()
  return result
 finally:
  if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
  p.wait()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--mutations',action='store_true');parser.add_argument('--object',type=Path);args=parser.parse_args()
 if args.object:
  symbols=run(['nm',args.object]).stdout
  if any(' U _ghostty_' in line or ' U ghostty_' in line for line in symbols.splitlines()):
   print('FAIL allocator object imports headless VT runtime');return 1
  print('PASS allocator object imports no headless VT runtime');return 0
 paths=[ROOT/'terminal/session-state'/n for n in ('budget-posix.c','budget-posix.h')];originals=[p.read_bytes() for p in paths];hashes=[hashlib.sha256(b).digest() for b in originals]
 WORK.mkdir(exist_ok=True)
 copies=[WORK/p.name for p in paths]
 if any(MARK.encode() in b for b in originals) or any(p.exists() and MARK in p.read_text() for p in copies):return 2
 def interrupted(sig,frame):raise RuntimeError('allocator-only check interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp')
 def restore():
  for p,q,b,digest in zip(paths,copies,originals,hashes):
   q.write_bytes(b)
   if hashlib.sha256(p.read_bytes()).digest()!=digest or hashlib.sha256(q.read_bytes()).digest()!=digest:raise RuntimeError('allocator-only SHA256 rollback failed')
 def compile_object():
  run(['cc','-std=c11','-D_DEFAULT_SOURCE','-DGHOSTERM_P0_ALLOCATOR_ONLY','-Wall','-Wextra','-Werror','-I'+str(ROOT/'build/GhosttyKit.xcframework/macos-arm64/Headers'),'-c',copies[0],'-o',WORK/'allocator.o'])
  return run(['nm',WORK/'allocator.o']).stdout
 def isolated(symbols):return not any(' U _ghostty_' in line or ' U ghostty_' in line for line in symbols.splitlines())
 try:
  restore();base=compile_object();assert isolated(base),'FAIL allocator object imports headless VT runtime'
  assert 'p0_budget_allocator' in base,'FAIL allocator provider disappeared'
  if args.mutations:
   text=originals[0].decode();old='#ifndef GHOSTERM_P0_ALLOCATOR_ONLY';assert text.count(old)==1
   copies[0].write_text(text.replace(old,'#include <ghostty/vt.h>\n#if 1')+'\n// '+MARK+'\n')
   variant=compile_object() # A compiler error never counts as assertion evidence.
   red=run([sys.executable,__file__,'--object',WORK/'allocator.o'],check=False)
   assert red.returncode==1 and 'FAIL allocator object imports' in red.stdout,'FAIL runtime-leak mutation escaped the isolation assertion'
   assert 'p0_budget_allocator' in variant,'FAIL same-mutant allocator provider control'
   print('PASS allocator mutation: runtime-reference assertion red; allocator provider control green')
   restore();assert isolated(compile_object()),'FAIL allocator rollback'
  print('PASS allocator-only GUI object: no headless VT imports; SHA256 rollback')
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
