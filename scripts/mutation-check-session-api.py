#!/usr/bin/env python3
"""Public session ABI translation: isolated ordinary assertion failures."""
import hashlib,os,shlex,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-api-mutations';SOURCE=ROOT/'core/src/session/client-api.c';MARK='GT_SESSION_API_MUTATION_MARK'
def run(args,timeout=30):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,start_new_session=True)
 try:out,_=p.communicate(timeout=timeout);return p.returncode,out
 finally:
  if p.poll() is None:os.killpg(p.pid,signal.SIGKILL)
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
  for source,name in [('test_session_api_bridge.c','bridge'),('test_session_api.c','unit'),('test_proxy_status.c','control')]:
   rc,out=run([*shlex.split(os.environ.get('CC','cc')),'-std=c11','-Wall','-Wextra','-Werror','-Icore/include','-Icore/src',ROOT/'core/tests'/source,copy,ROOT/'core/build/libghosterm_core.a',*shlex.split(flags),'-pthread','-o',WORK/name])
   if rc:raise RuntimeError('session ABI compilation failed: '+out)
 def probe(name,red=False):
  rc,out=run([WORK/name,ROOT])
  if red:
   if rc!=1 or 'FAIL ' not in out:raise RuntimeError('ordinary FAIL/exit 1 required/'+name+': '+out)
  elif rc:raise RuntimeError('session ABI baseline/control failed/'+name+': '+out)
 specs=[
 ('step reentry accepted','if (!s || s->busy)\n    return GT_SESSION_STATE_FAILED;','if (!s)\n    return GT_SESSION_STATE_FAILED;','bridge'),
 ('close reentry frees owner','if (s->busy)\n    return false;','if (false && s->busy)\n    return false;','bridge'),
 ('input reentry accepted','if (!s || s->busy || !out)','if (!s || !out)','bridge'),
 ('info reentry accepted','if (!s || !out || s->busy)','if (!s || !out)','bridge'),
 ('snapshot readonly lost','format, bytes, n, readonly, error','format, bytes, n, !readonly, error','bridge'),
 ('binary event shortened','e.length = source->length;','e.length = 0;','bridge'),
 ('resize pixel dimensions lost','size.px_width,\n                                 size.px_height','0,\n                                 0','bridge'),
 ('color inverted','e.dark = c.dark != 0;','e.dark = c.dark == 0;','bridge'),
 ('known zero sequence folded','{a.known, a.generation, a.sequence}','{a.known && a.sequence!=0, a.generation, a.sequence}','bridge'),
 ('receipt sequence restamped','source->stamp.applied_sequence},','0},','bridge'),
 ('unknown result invented successful','.delivery = (gt_session_delivery_t)source->delivery,','.delivery = GT_SESSION_OPERATION_OK,','bridge'),
 ('unknown exit invented known','out->has_exit_code = r->has_exit_code;','out->has_exit_code = true;','bridge'),
 ('unknown exit value exposed','out->exit_code = r->has_exit_code ? r->exit_code : 0;','out->exit_code = r->exit_code;','bridge'),
 ('instance identity lost','memcpy(out->instance, i.instance, 16);','memset(out->instance,0,16);','bridge'),
 ('submit sequence lost','stamp.applied_sequence};','0};','bridge'),
 ('resize routed as input','.kind = GT_WORKER_RESIZE,','.kind = GT_WORKER_INPUT,','bridge'),
 ('color routed as resize','gt_user_request_t r = {.kind = GT_WORKER_COLOR_SCHEME,','gt_user_request_t r = {.kind = GT_WORKER_RESIZE,','bridge'),
 ('local authentication silently successful','if (!s->ssh)\n    return GT_ERR_UNSUPPORTED;','if (!s->ssh)\n    return GT_OK;','unit'),
 ('duplicate cancel','if (!s || s->busy || s->cancelled)','if (!s || s->busy)','bridge'),
 ('catalog flag lost','.catalog_only = o->catalog_only,','.catalog_only = false,','unit'),
 ('pending nonce lost','memcpy(c.pending_nonce, o->pending_nonce, 16);','memset(c.pending_nonce,0,16);','unit'),
 ('catalog reentry accepted','!s || s->busy || !count','!s || !count','bridge'),
 ('refresh reentry accepted','gt_session_submit_t gt_session_catalog_refresh(gt_session_t *s) {\n  if (!s || s->busy)','gt_session_submit_t gt_session_catalog_refresh(gt_session_t *s) {\n  if (!s)','bridge'),
 ('terminate reentry accepted','gt_session_submit_t gt_session_terminate(gt_session_t *s, uint64_t target) {\n  if (!s || s->busy)','gt_session_submit_t gt_session_terminate(gt_session_t *s, uint64_t target) {\n  if (!s)','bridge'),
 ('small catalog falsely successful','if (capacity < n)\n    return GT_ERR_INVALID_ARG;','if (capacity < n)\n    return GT_OK;','bridge'),
 ('row result invented known','.has_result = r->has_result};','.has_result = true};','bridge'),
 ('unknown row exit exposed','rows[i].exit_code = v->has_exit_code ? v->exit_code : 0;','rows[i].exit_code = v->exit_code;','bridge'),
 ('row phase discarded','.phase = (uint32_t)r->phase,','.phase = 0,','bridge'),
 ('creation admission hidden','.has_admitted_create = i.has_admitted_create,','.has_admitted_create = false,','bridge'),
 ('stop unknown folded known','(gt_session_stop_state_t)i.stop.state','GT_SESSION_STOP_KNOWN','bridge'),
 ('stop input lock omitted',' || i.input_locked,',',','bridge'),
 ]
 try:
  restore();build();probe('bridge');probe('unit');probe('control')
  for label,old,new,target in specs:
   text=original.decode()
   if old not in text:raise RuntimeError('session ABI mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n/* '+MARK+' */\n');build();probe(target,True);probe('control');print('PASS session ABI mutation/'+label+': ordinary red, same-mutant control green',flush=True)
   finally:restore()
  build();probe('bridge');probe('unit');probe('control');print('PASS '+str(len(specs))+' session ABI mutations, translation/real-provider baselines and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
