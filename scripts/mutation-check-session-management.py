#!/usr/bin/env python3
"""Shared management coordinator: ordinary assertion red and independent green."""
import hashlib,os,shlex,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-management-mutations';SOURCE=ROOT/'core/src/ipc/session-client.c';MARK='GT_SESSION_MANAGEMENT_MUTATION_MARK'
MODES=('pending','admission','catalog','catalog-network','stop','stop-unknown','validation')
def run(args):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
 try:out,_=p.communicate(timeout=30);return p.returncode,out
 finally:
  if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
  p.wait()
def main():
 WORK.mkdir(exist_ok=True);copy=WORK/'session-client.c';original=SOURCE.read_bytes();digest=hashlib.sha256(original).digest()
 if MARK.encode() in original or (copy.exists() and MARK in copy.read_text()):return 2
 def interrupted(sig,frame):raise RuntimeError('session management mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 os.environ['TMPDIR']=str(ROOT/'tmp')
 def restore():
  copy.write_bytes(original)
  if hashlib.sha256(copy.read_bytes()).digest()!=digest or SOURCE.read_bytes()!=original:raise RuntimeError('session management SHA256 rollback failed')
 def build():
  rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Icore/src','-Icore/src/ipc','-Icore/include',copy,ROOT/'core/tests/test_session_management.c',ROOT/'core/build/libghosterm_client_protocol.a','-o',WORK/'test'])
  if rc:raise RuntimeError('management mutation compilation failed: '+out)
 def probe(mode,red=False):
  rc,out=run([WORK/'test',mode]);(WORK/('last-red.log' if red else 'last-green.log')).write_text(out)
  if red:
   if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+mode+': '+out)
  elif rc:raise RuntimeError('management baseline/control failed/'+mode+': '+out)
 specs=[
 ('pending nonce creates again','c->create_admitted?GT_MSG_QUERY_CREATE:GT_MSG_CREATE','GT_MSG_CREATE','pending','catalog'),
 ('admission not observable before wire','if(c->request==GT_MSG_CREATE)c->create_admitted=true;','if(c->request==GT_MSG_CREATE)c->create_admitted=false;','admission','catalog'),
 ('unknown catalog folded empty','if(!c->catalog_known)return GT_ERR_NOT_FOUND;','if(!c->catalog_known)return GT_OK;','catalog','stop'),
 ('known empty catalog folded unknown','if(!c->catalog_known)return GT_ERR_NOT_FOUND;','if(!c->catalog_known || !c->catalog_count)return GT_ERR_NOT_FOUND;','catalog','stop'),
 ('catalog data corrupted','memcpy(c->catalog,r.rows,r.count*sizeof(*c->catalog));','memset(c->catalog,0,r.count*sizeof(*c->catalog));','catalog','stop'),
 ('catalog stale after disconnect','c->catalog_known=false;c->catalog_count=0;','c->catalog_count=0;','catalog-network','stop'),
 ('refresh starts with stale clock','if(!tick(c))return GT_CLIENT_UNAVAILABLE;','if(false && !tick(c))return GT_CLIENT_UNAVAILABLE;','catalog','stop'),
 ('catalog steady stage lost','c->stage=CATALOG;c->info.phase=GT_SESSION_CATALOG;','c->stage=CONNECT;c->info.phase=GT_SESSION_CATALOG;','catalog','stop'),
 ('stop cleanup falsely confirmed','c->info.stop.state=GT_CLIENT_STOP_UNKNOWN;','c->info.stop.state=GT_CLIENT_STOP_KNOWN;','stop-unknown','catalog'),
 ('control unknown falsely confirmed','r->delivery==GT_CONTROL_CLIENT_REPLY?GT_CLIENT_STOP_KNOWN:GT_CLIENT_STOP_UNKNOWN','GT_CLIENT_STOP_KNOWN','stop-unknown','catalog'),
 ('ambiguous stop automatically repeated','c->request=c->config.catalog_only?GT_MSG_LIST:','c->request=c->info.stop.state==GT_CLIENT_STOP_UNKNOWN?GT_MSG_TERMINATE:c->config.catalog_only?GT_MSG_LIST:','stop-unknown','catalog'),
 ('pending stop leaves input open','s->state==GT_CLIENT_STOP_PENDING || s->state==GT_CLIENT_STOP_UNKNOWN','s->state==GT_CLIENT_STOP_UNKNOWN','stop','catalog'),
 ('stop acknowledgment invents process exit','*out=c->info;out->has_admitted_create','*out=c->info;if(c->info.stop.state==GT_CLIENT_STOP_KNOWN && c->info.stop.status==GT_CONTROL_OK)out->phase=GT_SESSION_ENDED;out->has_admitted_create','stop','pending'),
 ('catalog can combine create mode','(config->catalog_only?1:0)!=1','0!=1','validation','stop'),
 ('pending instance not required','(config->session || nonzero(config->pending_nonce,16))','config->session','validation','catalog'),
 ]
 try:
  restore();build()
  for mode in MODES:probe(mode)
  for label,old,new,target,control in specs:
   text=original.decode()
   if old not in text:raise RuntimeError('management mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe(control)
    print('PASS session management mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  build()
  for mode in MODES:probe(mode)
  print('PASS 15 management mutations, 7 protocol baselines and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
