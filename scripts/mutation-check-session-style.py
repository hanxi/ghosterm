#!/usr/bin/env python3
"""Ordered style codecs and Source defaults: ordinary assertion reds, isolated tree."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-style-mutations';MARK='GT_SESSION_STYLE_MUTATION_MARK'
FILES=('core/src/ipc/user-wire.c','core/src/ipc/data-wire.c','daemon/src/vt-state.c')
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
 def interrupted(sig,frame):raise RuntimeError('ordered style mutation interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 if WORK.exists():shutil.rmtree(WORK)
 WORK.mkdir();os.environ['TMPDIR']=str(ROOT/'tmp')
 for name in ('core','daemon','terminal'):shutil.copytree(ROOT/name,WORK/name,ignore=shutil.ignore_patterns('build','.build','__pycache__'))
 for name in ('patches','vendor','scripts'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 build=WORK/'build'
 def checked(args):
  rc,out=run(args)
  if rc:raise RuntimeError('style mutation setup/build failed: '+out[-5000:])
 def restore():
  for n,b in originals.items():
   (WORK/n).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[n] for p in (ROOT/n,WORK/n)):raise RuntimeError('ordered style SHA256 rollback failed/'+n)
 def rebuild():checked(['cmake','--build',build,'--clean-first','--target','test_session_style_wire','test_daemon_vt','-j','4'])
 def probe(case,red=False):
  args=[build/'core/test_session_style_wire',case] if case in ('user','data') else [build/'test_daemon_vt',case,WORK]
  rc,out=run(args);(WORK/('last-red.log' if red else 'last-green.log')).write_text(out)
  if red:
   if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary style FAIL/exit1 required: '+out)
  elif rc or 'PASS ' not in out:raise RuntimeError('style baseline/control failed: '+out)
 specs=[
 ('USER palette omitted',FILES[0],'memcpy(p+13,r->style.palette,768);','memset(p+13,0,768);','user','data'),
 ('DATA palette omitted',FILES[1],'memcpy(p+37,m->style.palette,768);','memset(p+37,0,768);','data','user'),
 ('Source default background dropped',FILES[2],'GhosttyColorRgb bg={colors.background[0],colors.background[1],colors.background[2]};','GhosttyColorRgb bg={0,0,0};','style','authority'),
 ('Source palette update dropped',FILES[2],'if(rc==GHOSTTY_SUCCESS)rc=ghostty_terminal_set(s->terminal,GHOSTTY_TERMINAL_OPT_COLOR_PALETTE,palette);','if(rc==GHOSTTY_SUCCESS)rc=GHOSTTY_SUCCESS; (void)palette;','style','authority'),
 ('frozen style replaced by factory defaults',FILES[2],'v->light_colors=s->light_colors;v->dark_colors=s->dark_colors;','v->light_colors=f->config.light;v->dark_colors=f->config.dark;','style','authority'),
 ]
 try:
  restore();checked(['cmake','-S',WORK/'daemon','-B',build,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_VT_INCLUDE_DIR='+str(ROOT/'build/GhosttyKit.xcframework/macos-arm64/Headers'),'-DGHOSTERM_VT_LIBRARY='+str(ROOT/'tmp/worker-surface/out/lib/libghostty-vt.dylib')]);rebuild()
  for case in ('user','data','style','authority'):probe(case)
  for label,name,old,new,target,control in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('style mutation anchor drift/'+label)
   try:
    (WORK/name).write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');rebuild();probe(target,True);probe(control);print('PASS style mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  rebuild()
  for case in ('user','data','style','authority'):probe(case)
  print('PASS 5 ordered style mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
