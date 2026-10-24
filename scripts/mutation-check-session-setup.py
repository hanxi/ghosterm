#!/usr/bin/env python3
"""Public session ABI translation: isolated ordinary assertion failures."""
import hashlib,os,shlex,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-setup-mutations';SOURCE=ROOT/'core/src/session/setup-api.c';MARK='GT_SESSION_SETUP_MUTATION_MARK'
def run(args,timeout=30):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
 try:out,_=p.communicate(timeout=timeout);return p.returncode,out
 finally:
  try:os.killpg(p.pid,signal.SIGKILL)
  except ProcessLookupError:pass
  p.wait()
def main():
 original=SOURCE.read_bytes();digest=hashlib.sha256(original).digest();WORK.mkdir(parents=True,exist_ok=True);copy=WORK/'api.c'
 if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
 def interrupted(sig,frame):raise RuntimeError('session ABI mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp')
 rc,flags=run(['pkg-config','--cflags','--libs','libssh2'])
 if rc:raise RuntimeError('libssh2 development flags required')
 def restore():
  copy.write_bytes(original)
  if hashlib.sha256(copy.read_bytes()).digest()!=digest or SOURCE.read_bytes()!=original:raise RuntimeError('session ABI SHA256 rollback failed')
 def build():
  for source,name in [('test_session_setup_bridge.c','bridge'),('test_proxy_status.c','control')]:
   rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Icore/include','-Icore/src',ROOT/'core/tests'/source,copy,ROOT/'core/build/libghosterm_core.a',*shlex.split(flags),'-pthread','-o',WORK/name])
   if rc:raise RuntimeError('session ABI compilation failed: '+out)
 def probe(name,red=False):
  rc,out=run([WORK/name,ROOT])
  if red:
   if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+name+': '+out)
  elif rc:raise RuntimeError('session ABI baseline/control failed/'+name+': '+out)
 specs=[
 ('resolved scope lost','.scope = options->scope,','.scope = 0,','bridge'),
 ('resolved address lost','memcpy(c.address, options->address, 16);','memset(c.address,0,16);','bridge'),
 ('authentication budget changed','options->timeout_ms : 60000','options->timeout_ms : 30000','bridge'),
 ('phase falsely reports TCP','(gt_session_auth_phase_t)gt__ssh_connector_step(a->connector)','(gt_session_auth_phase_t)(gt__ssh_connector_step(a->connector),0)','bridge'),
 ('host unknown folded known','.host = (gt_session_host_state_t)i.host,','.host = GT_SESSION_HOST_KNOWN,','bridge'),
 ('fingerprint omitted','memcpy(out->fingerprint, i.fingerprint, sizeof(out->fingerprint));','memset(out->fingerprint,0,sizeof(out->fingerprint));','bridge'),
 ('second take leaves stale alias','if (out)\n    *out = NULL;\n  return a && out &&','return a && out &&','bridge'),
 ('authenticator owner leaked','gt__ssh_connector_close(a->connector);','(void)a->connector;','bridge'),
 ('failed probe metadata published','if (e == GT_OK) {','if (true) {','bridge'),
 ('cancellation discarded','c->options->cancelled(c->options->context);','(c->options->cancelled(c->options->context),false);','bridge'),
 ('progress file identity changed','file, bytes, total);','file+1, bytes, total);','bridge'),
 ('publication unknown hidden','out->publication_unknown = r.publication_unknown;','out->publication_unknown = false;','bridge'),
 ('catalog rejection ignored','if (e != GT_OK)\n    return e;','if (false && e != GT_OK)\n    return e;','bridge'),
 ('install error hidden','memcpy(out->proxy, r.proxy, sizeof(out->proxy));\n  return e;','memcpy(out->proxy, r.proxy, sizeof(out->proxy));\n  return GT_OK;','bridge'),
 ]
 try:
  restore();build();probe('bridge');probe('control')
  for label,old,new,target in specs:
   text=original.decode()
   if old not in text:raise RuntimeError('session ABI mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe('control');print('PASS session ABI mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  build();probe('bridge');probe('control');print('PASS 14 setup ABI mutations, translation baselines and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
