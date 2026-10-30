#!/usr/bin/env python3
"""No-libssh2 controlled-shell contract, isolated archive and status mutation."""
import hashlib, pathlib, shutil, subprocess, tempfile, signal
def interrupted(signum, frame): raise KeyboardInterrupt(signum)
signal.signal(signal.SIGINT, interrupted)
signal.signal(signal.SIGTERM, interrupted)
root=pathlib.Path(__file__).resolve().parents[1];source=root/'core/src/ssh.c';before=hashlib.sha256(source.read_bytes()).digest()
(root/'tmp').mkdir(exist_ok=True)
log=root/'tmp/shell-unavailable.log'
if b'GT_SSH_SHELL_UNAVAILABLE_MUTATION' in source.read_bytes():raise RuntimeError('Residual unavailable mutation')
with tempfile.TemporaryDirectory(dir=root/'tmp',prefix='shell-unavailable-') as directory,log.open('w') as output:
 work=pathlib.Path(directory);shutil.copytree(root/'core',work/'core',ignore=shutil.ignore_patterns('build','build-*'))
 build=work/'core/build'
 def run(args,red=False):
  result=subprocess.run(['bash','-c','. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require shell-unavailable; shift; gt_timeout 180 "$@"','unavailable',str(root),*args],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  output.write(result.stdout);output.flush()
  if red:
   assert result.returncode==1 and 'FAIL unavailable shell status' in result.stdout,result.stdout
  else: assert result.returncode==0,result.stdout
 run(['cmake','-S',str(work/'core'),'-B',str(build),'-DGHOSTERM_WITH_LIBSSH2=OFF'])
 probe=work/'probe.c';probe.write_text('''#include "ghosterm.h"
#include <stdio.h>
int main(void) {
 gt_ssh_opts_t opts={.host="localhost",.username="fixture"};
 gt_ssh_t *ssh=gt_ssh_new(&opts); if(!ssh) return 2;
 gt_status_t status=GT_OK;
 gt_transport_t *t=gt_transport_open_ssh_shell_controlled(ssh,NULL,100,NULL,NULL,&status);
 int failed=t!=NULL || status!=GT_ERR_UNSUPPORTED;
 gt_transport_close(t);gt_ssh_close(ssh);
 if(failed)puts("FAIL unavailable shell status");
 return failed;
}
''')
 def build_probe():
  run(['cmake','--build',str(build),'--clean-first','--target','test_api'])
  run(['cc','-std=c11','-I'+str(work/'core/include'),str(probe),str(build/'libghosterm_core.a'),'-lpthread','-o',str(work/'probe')])
 build_probe();run([str(work/'probe')]);run([str(build/'test_api')])
 copy=work/'core/src/ssh.c';clean=copy.read_bytes();text=clean.decode();old='if (status) *status = GT_ERR_UNSUPPORTED;';assert text.count(old)==1
 copy.write_text(text.replace(old,'if (status) *status = GT_OK; /* GT_SSH_SHELL_UNAVAILABLE_MUTATION */'))
 build_probe();run([str(work/'probe')],red=True);run([str(build/'test_api')])
 copy.write_bytes(clean);build_probe();run([str(work/'probe')]);assert hashlib.sha256(copy.read_bytes()).digest()==before
 assert hashlib.sha256(source.read_bytes()).digest()==before
 print('PASS no-libssh2: real archive / status mutation red / API green / restored green / SHA256',file=output)
print('SSH SHELL UNAVAILABLE PASS: status red / API green / restored green / SHA256')
