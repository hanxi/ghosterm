#!/usr/bin/env python3
"""Public SSH authentication translation: isolated ordinary assertion failures."""
import hashlib,os,shlex,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/ssh-auth-mutations';SOURCE=ROOT/'core/src/ssh/authenticator.c';MARK='GT_SSH_AUTH_MUTATION_MARK'
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
 original=SOURCE.read_bytes();digest=hashlib.sha256(original).digest();WORK.mkdir(parents=True,exist_ok=True);copy=WORK/'api.c'
 if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
 def interrupted(sig,frame):raise RuntimeError('session ABI mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp')
 if sys.platform=='darwin':
  os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
  rc,sdk=run(['xcrun','--sdk','macosx','--show-sdk-path'])
  if rc:raise RuntimeError('Xcode SDK unavailable')
  os.environ['SDKROOT']=sdk.strip()
 rc,flags=run(['pkg-config','--cflags','--libs','libssh2'])
 if rc:raise RuntimeError('libssh2 development flags required')
 def restore():
  copy.write_bytes(original)
  if hashlib.sha256(copy.read_bytes()).digest()!=digest or SOURCE.read_bytes()!=original:raise RuntimeError('session ABI SHA256 rollback failed')
 def build():
  for source,name in [('test_ssh_auth_bridge.c','bridge'),('test_config.c','control')]:
   rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Icore/include','-Icore/src',ROOT/'core/tests'/source,copy,ROOT/'core/build/libghosterm_core.a',*shlex.split(flags),'-pthread','-o',WORK/name])
   if rc:raise RuntimeError('session ABI compilation failed: '+out)
 def probe(name,red=False):
  rc,out=run([WORK/name,ROOT/'core/tests/fixtures'])
  if red:
   if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+name+': '+out)
  elif rc:raise RuntimeError('session ABI baseline/control failed/'+name+': '+out)
 specs=[
 ('resolved scope lost','.scope = options->scope,','.scope = 0,','bridge'),
 ('resolved address lost','memcpy(c.address, options->address, 16);','memset(c.address,0,16);','bridge'),
 ('authentication budget changed','options->timeout_ms : 60000','options->timeout_ms : 30000','bridge'),
 ('phase falsely reports TCP','(gt_ssh_auth_phase_t)gt__ssh_connector_step(a->connector)','(gt_ssh_auth_phase_t)(gt__ssh_connector_step(a->connector),0)','bridge'),
 ('host unknown folded known','.host = (gt_ssh_auth_host_state_t)i.host,','.host = GT_SSH_AUTH_HOST_KNOWN,','bridge'),
 ('fingerprint omitted','memcpy(out->fingerprint, i.fingerprint, sizeof(out->fingerprint));','memset(out->fingerprint,0,sizeof(out->fingerprint));','bridge'),
 ('second take leaves stale alias','if (out)\n    *out = NULL;\n  return a && out &&','return a && out &&','bridge'),
 ('authenticator owner leaked','gt__ssh_connector_close(a->connector);','(void)a->connector;','bridge'),
 ]
 try:
  restore();build();probe('bridge');probe('control')
  for label,old,new,target in specs:
   text=original.decode()
   if old not in text:raise RuntimeError('session ABI mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe('control');print('PASS session ABI mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  build();probe('bridge');probe('control');print('PASS 8 SSH authentication ABI mutations, translation baselines and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
