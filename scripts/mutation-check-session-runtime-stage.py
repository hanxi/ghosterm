#!/usr/bin/env python3
"""Runtime staging rejects damaged/foreign inventories and immutable collisions."""
import hashlib,os,shutil,signal,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];WORK=ROOT/'tmp/session-runtime-stage-mutations';MARK='GT_RUNTIME_STAGE_MUTATION_MARK'
def main():
 source=ROOT/'scripts/stage-session-runtime.py';original=source.read_bytes();digest=hashlib.sha256(original).digest();copy=WORK/'scripts/stage-session-runtime.py'
 if MARK.encode() in original or copy.exists() and MARK in copy.read_text():return 2
 def interrupted(sig,frame):raise RuntimeError('runtime staging mutation interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 if WORK.exists():shutil.rmtree(WORK)
 copy.parent.mkdir(parents=True);(WORK/'tmp').mkdir()
 env=dict(os.environ,TMPDIR=str(ROOT/'tmp'),GHOSTERM_RUNTIME_STAGE_SCRIPT=str(copy))
 def restore():
  copy.write_bytes(original)
  if any(hashlib.sha256(p.read_bytes()).digest()!=digest for p in (source,copy)):raise RuntimeError('runtime staging SHA256 rollback failed')
 def probe(test='',red=False):
  command=[sys.executable,ROOT/'scripts/probe-session-runtime-stage.py']+(['RuntimeStageTests.'+test] if test else [])
  p=subprocess.Popen(command,env=env,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  try:out,_=p.communicate(timeout=20)
  finally:
   if p.poll() is None:
    os.killpg(p.pid,signal.SIGTERM)
    try:p.wait(timeout=5)
    except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
  (WORK/('last-red.log' if red else 'last-green.log')).write_text(out)
  if red:
   if p.returncode!=1 or 'AssertionError' not in out or 'FAILED (failures=' not in out:raise RuntimeError('ordinary runtime staging assertion red required: '+out)
  elif p.returncode or '\nOK\n' not in out:raise RuntimeError('runtime staging baseline/control failed: '+out)
 specs=[
  ('native manifest truncated','set(value.get(\'files\',{}))!=set(MAC_FILES)','not set(value.get(\'files\',{})).issubset(set(MAC_FILES))','test_native_manifest_digest_and_extra_file_rejected'),
  ('native checksum ignored','digest(path/name)!=sha','False','test_native_tamper_and_unsafe_file'),
  ('Linux contract ignored','header[5]!=contract','False','test_linux_contract_and_duplicate_inventory'),
  ('Linux inventory duplicated','entry[0] in seen','False','test_linux_contract_and_duplicate_inventory'),
  ('Linux checksum ignored','digest(p)!=entry[2]','False','test_linux_checksum_and_size'),
  ('existing release accepted after tamper','if digest(source/name)!=digest(target/name):','if False:','test_existing_release_never_overwritten'),
  ('unsafe release accepted',"not token(value.get('release'))",'False','test_release_token_and_destination_symlink'),
 ]
 try:
  restore();probe()
  for label,old,new,test in specs:
   text=original.decode()
   if text.count(old)!=1:raise RuntimeError('runtime staging mutation anchor drift/'+label)
   try:
    copy.write_text(text.replace(old,new)+'\n# '+MARK+'\n');probe(test,True);probe('test_matching_inventory')
    print('PASS runtime staging mutation/'+label+': ordinary red, same-mutant matching inventory green',flush=True)
   finally:restore()
  probe();print('PASS 7 runtime staging mutations and SHA256 rollback',flush=True)
 finally:restore()
 return 0
if __name__=='__main__':sys.exit(main())
