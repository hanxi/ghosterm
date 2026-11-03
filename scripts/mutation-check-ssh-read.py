#!/usr/bin/env python3
"""Compile shell-read mutants: confirmed EOF, genuine resets and final output."""
from pathlib import Path
import hashlib,json,os,signal,subprocess
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_SSH_READ_MUTATION'
def main():
 path=ROOT/'core/src/ssh.c';original=path.read_bytes();digest=hashlib.sha256(original).hexdigest()
 if MARK.encode() in original:raise RuntimeError('Residual SSH read mutation')
 work=ROOT/'tmp/ssh-read-mutations';work.mkdir(parents=True,exist_ok=True)
 def run(label,args,expected=0):
  p=subprocess.run([str(a) for a in args],cwd=ROOT,env={**os.environ,"TMPDIR":str(ROOT/"tmp")},capture_output=True,text=True,timeout=180)
  output=p.stdout+p.stderr;(work/(label+'.log')).write_text(output)
  if p.returncode!=expected:raise RuntimeError(label+': '+output)
  if expected and 'FAIL ' not in output:raise RuntimeError('Not an assertion failure: '+label)
 def build(label):
  # Force fresh objects even on filesystems with whole-second timestamps.
  run(label+'-build',['cmake','--build',ROOT/'core/build','--clean-first','--target','test_ssh_read','test_api','-j','2'])
 run('configure',['bash',ROOT/'scripts/build-core.sh'])
 run('baseline',[ROOT/'core/build/test_ssh_read'])
 run('baseline-control',[ROOT/'core/build/test_api'])
 cases=[
  ('confirmed','if (libssh2_channel_eof(sh->chan)) return GT_TRANSPORT_READ_EOF;',
   'if (n == 0 && libssh2_channel_eof(sh->chan)) return GT_TRANSPORT_READ_EOF;','confirmed'),
  ('reset','if (libssh2_channel_eof(sh->chan)) return GT_TRANSPORT_READ_EOF;',
   'if (libssh2_channel_eof(sh->chan) || n == LIBSSH2_ERROR_SOCKET_RECV) return GT_TRANSPORT_READ_EOF;','reset'),
  ('tail','if (n > 0) return (int)n;',
   'if (n > 0 && !libssh2_channel_eof(sh->chan)) return (int)n;','tail'),
 ]
 results=[]
 try:
  for label,old,new,mode in cases:
   source=original.decode()
   if source.count(old)!=1:raise RuntimeError('Ambiguous mutation anchor: '+label)
   path.write_text(source.replace(old,new+' /* '+MARK+' */'))
   build(label)
   run(label+'-target',[ROOT/'core/build/test_ssh_read',mode],1)
   run(label+'-control',[ROOT/'core/build/test_api'])
   path.write_bytes(original)
   if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise RuntimeError('Restore SHA256 mismatch')
   build(label+'-restored');run(label+'-restored',[ROOT/'core/build/test_ssh_read'])
   results.append(label);print(label+': compiled assertion red / independent API green / restored all modes green / SHA256',flush=True)
 finally:
  if MARK.encode() in path.read_bytes():path.write_bytes(original)
  if hashlib.sha256(path.read_bytes()).hexdigest()!=digest:raise RuntimeError('Source changed during mutations')
 (work/'result.json').write_text(json.dumps(dict(accepted=True,groups=results,source_sha256=digest),indent=2)+'\n')
 print('SSH READ MUTATIONS PASS: '+str(len(results))+' groups')
if __name__=='__main__':
 def interrupted(signum,frame):raise KeyboardInterrupt('signal '+str(signum))
 signal.signal(signal.SIGINT,interrupted);signal.signal(signal.SIGTERM,interrupted)
 main()
