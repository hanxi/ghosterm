#!/usr/bin/env python3
"""Deferred SSH auth/adoption guards, real sshd, ordinary FAIL and SHA rollback."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-deferred-auth-mutations';MARK='GT_DEFERRED_AUTH_MUTATION_MARK'
FILES=('core/src/ssh.c','core/src/ipc/ssh-session-provider.c')
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
 def interrupted(sig,frame):raise RuntimeError('deferred auth mutation interrupted '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 if WORK.exists():shutil.rmtree(WORK)
 WORK.mkdir();(WORK/'tmp').mkdir();os.environ['TMPDIR']=str(ROOT/'tmp')
 shutil.copytree(ROOT/'core',WORK/'core',ignore=shutil.ignore_patterns('build','.build'))
 for name in ('terminal','daemon','scripts'):(WORK/name).symlink_to(ROOT/name,target_is_directory=True)
 build=WORK/'build'
 def checked(args):
  rc,out=run(args)
  if rc:raise RuntimeError('deferred auth setup/build failed: '+out[-5000:])
 def restore():
  for n,b in originals.items():
   (WORK/n).write_bytes(b)
   if any(hashlib.sha256(p.read_bytes()).digest()!=digests[n] for p in (ROOT/n,WORK/n)):raise RuntimeError('deferred auth SHA256 rollback failed/'+n)
 def rebuild():checked(['cmake','--build',build,'--clean-first','--target','test_session_deferred_auth','test_session_setup_bridge','-j','4'])
 def probe(red=False,control=False):
  args=[build/'test_session_setup_bridge'] if control else ['bash',WORK/'core/tests/scripts/session_deferred_auth_e2e.sh',build/'test_session_deferred_auth']
  rc,out=run(args);(WORK/('last-control.log' if control else 'last-red.log' if red else 'last-baseline.log')).write_text(out)
  if red:
   if rc!=1 or 'FAIL deferred SSH:' not in out:raise RuntimeError('ordinary deferred auth FAIL/exit1 required: '+out[-3000:])
  elif rc or ('PASS setup ABI' if control else 'PASS deferred SSH:') not in out:raise RuntimeError('deferred auth baseline/control failed: '+out[-4000:])
 specs=[
 ('deferred empty-material open rejected',FILES[0],'if(!config->defer_credentials && !(*owned)->pem && !(*owned)->password)','if(!(*owned)->pem && !(*owned)->password)'),
 ('foreign username material admitted',FILES[0],'strcmp(target->username,material->username)','false'),
 ('unsupported method silently accepted',FILES[0],'if((material->pem && !(target->auth_methods&GT_SSH_AUTH_PUBLICKEY)) ||\n     (!material->pem && !(target->auth_methods&GT_SSH_AUTH_PASSWORD)))','if(false)'),
 ('candidate retry resets deadline',FILES[0],'if(!connector_tick(c))return c->info.error;\n  gt_ssh_t *target','uint64_t reset;if(c->config.clock(c->config.clock_context,&reset))c->started=reset;\n  if(!connector_tick(c))return c->info.error;\n  gt_ssh_t *target'),
 ('bad key drops established connection',FILES[0],'if(c->config.defer_credentials && error==GT_ERR_AUTH)','if(false && c->config.defer_credentials && error==GT_ERR_AUTH)'),
 ('credentials phase mistaken for TAKEN on cancel',FILES[0],'if(!c || (c->info.phase>=GT_SSH_CONNECT_TAKEN && c->info.phase!=GT_SSH_CONNECT_CREDENTIALS))return;','if(!c || c->info.phase>=GT_SSH_CONNECT_TAKEN)return;'),
 ('stale adoption request admitted',FILES[1],'request!=p->info.request','false'),
 ('adoption ignores total deadline',FILES[1],'if(!tick(p))return p->info.error;','if(false && !tick(p))return p->info.error;'),
 ]
 try:
  restore();checked(['cmake','-S',WORK/'core','-B',build,'-DGHOSTERM_WITH_LIBSSH2=ON','-DCMAKE_BUILD_TYPE=Debug']);rebuild();probe();probe(control=True)
  for label,name,old,new in specs:
   text=originals[name].decode()
   if text.count(old)!=1:raise RuntimeError('deferred auth mutation anchor drift/'+label)
   try:
    (WORK/name).write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');rebuild();probe(True);probe(control=True);print('PASS deferred auth mutation/'+label+': ordinary red and same-mutant green',flush=True)
   finally:restore()
  rebuild();probe();probe(control=True);print('PASS 8 deferred auth/adoption mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
