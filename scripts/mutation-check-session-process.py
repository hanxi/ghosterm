#!/usr/bin/env python3
"""Source process metadata: real runtime, bounded observation, codec and client."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-process-mutations';MARK='GT_SESSION_PROCESS_MUTATION_MARK'
FILES=('core/src/session/worker.c','core/src/session/creation.c','core/src/session/process-meta.h','core/src/ipc/data-router.c','core/src/ipc/attachment-client.c')
def run(args):
 p=subprocess.Popen([str(a) for a in args],start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:out,_=p.communicate(timeout=120);return p.returncode,out
 finally:
  if p.poll() is None:
   os.killpg(p.pid,signal.SIGTERM)
   try:p.wait(timeout=10)
   except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
def main():
 originals={n:(ROOT/n).read_bytes() for n in FILES};digests={n:hashlib.sha256(b).digest() for n,b in originals.items()}
 if any(MARK.encode() in b or ((WORK/n).exists() and MARK in (WORK/n).read_text()) for n,b in originals.items()):return 2
 def interrupted(sig,frame):raise RuntimeError('process metadata mutation interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 if WORK.exists():shutil.rmtree(WORK)
 WORK.mkdir();(WORK/'tmp').mkdir();os.environ['TMPDIR']=str(ROOT/'tmp')
 shutil.copytree(ROOT/'core',WORK/'core',ignore=shutil.ignore_patterns('build','.build'))
 for name in ('terminal','daemon','scripts'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 build=WORK/'build'
 def checked(args):
  rc,out=run(args)
  if rc:raise RuntimeError('process metadata build failed: '+out[-5000:])
 def restore():
  for n,b in originals.items():
   (WORK/n).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[n] for p in (ROOT/n,WORK/n)):raise RuntimeError('process metadata SHA256 rollback failed/'+n)
 def rebuild():checked(['cmake','--build',build,'--clean-first','--target','test_process_meta','test_session_runtime','test_attachment_client','test_data_wire','-j','4'])
 def probe(target,red=False):
  args={'owner':[build/'test_process_meta','owner'],'codec':[build/'test_process_meta','codec'],'runtime':[build/'test_session_runtime','route-flow',ROOT/'tmp'],'client':[build/'test_attachment_client','process'],'control':[build/'test_data_wire','vectors']}[target]
  rc,out=run(args);(WORK/('last-red.log' if red else 'last-'+target+'.log')).write_text(out)
  if red:
   if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary metadata FAIL/exit1 required: '+out[-4000:])
  elif rc or 'PASS ' not in out:raise RuntimeError('metadata baseline/control failed: '+out[-4000:])
 specs=[
 ('Source observation dropped',FILES[0],'if(!w->config.process)return;','if(true)return;','owner'),
 ('unknown child collapsed to idle',FILES[0],'if((unsigned)value.child>GT_TRANSPORT_CHILD_JOB)value.child=GT_TRANSPORT_CHILD_UNKNOWN;','if(value.child==GT_TRANSPORT_CHILD_UNKNOWN || (unsigned)value.child>GT_TRANSPORT_CHILD_JOB)value.child=GT_TRANSPORT_CHILD_IDLE;','owner'),
 ('kernel query throttle removed',FILES[0],'now-*previous<250','now-*previous<1','owner'),
 ('unchanged metadata floods callback',FILES[0],'if(!*observed || !gt__process_equal(last,&value))','if(true)','owner'),
 ('unavailable cwd retains partial path',FILES[0],'value.has_cwd=false;memset(value.cwd,0,sizeof(value.cwd));','value.has_cwd=false;','owner'),
 ('creation loses metadata forwarding',FILES[1],'wrapped.process=j->original.process?process_observed:NULL;','wrapped.process=j->original.process?NULL:process_observed;','runtime'),
 ('surrogate UTF8 accepted',FILES[2],'if(cp<minimum || cp>0x10ffff || (cp>=0xd800 && cp<=0xdfff))return false;','if(cp<minimum || cp>0x10ffff)return false;','codec'),
 ('router never publishes process state',FILES[3],'if(gt__runtime_process(p->lease,&process) &&','if(false && gt__runtime_process(p->lease,&process) &&','runtime'),
 ('client loses observation availability',FILES[4],'c->info.process=m.process;c->info.has_process=true;','c->info.process=m.process;c->info.has_process=false;','client'),
 ('cwd absence preserves stale value',FILES[4],'c->info.process=m.process;c->info.has_process=true;','if(m.process.has_cwd)c->info.process=m.process;c->info.has_process=true;','client'),
 ('old generation metadata accepted',FILES[4],'m.generation!=c->config.generation','false','client'),
 ]
 try:
  restore();checked(['cmake','-S',WORK/'core','-B',build,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DCMAKE_BUILD_TYPE=Debug']);rebuild()
  for target in ('codec','owner','runtime','client','control'):probe(target)
  for label,name,old,new,target in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('process mutation anchor drift/'+label)
   try:
    (WORK/name).write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');rebuild();probe(target,True);probe('control');print('PASS process mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  rebuild()
  for target in ('codec','owner','runtime','client','control'):probe(target)
  print('PASS 11 process metadata mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
