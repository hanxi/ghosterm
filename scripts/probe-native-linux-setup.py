#!/usr/bin/env python3
"""Mac native setup/RootView integration against a dedicated Linux SSH fixture."""
import argparse, json, os, re, select, signal, subprocess, sys, time
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]

def fixture_paths(env):
    runtime=Path(env['GHOSTERM_REPLICA_RUNTIME']); install=Path(env['GHOSTERM_SESSION_SETUP_ROOT'])
    scope=Path(env.get('GHOSTERM_TEST_RUNTIME_SCOPE',str(runtime)))
    if not all(p.is_absolute() and '..' not in p.parts for p in (runtime,install,scope)) or scope.parent!=install.parent or not all(p.name.startswith('ghosterm-test-') for p in (scope,install)):
        raise RuntimeError('dedicated ghosterm-test runtime scope/install siblings required')
    try:relative=runtime.relative_to(scope)
    except ValueError:raise RuntimeError('runtime must remain inside its owned fixture scope')
    if len(relative.parts)>4:raise RuntimeError('runtime scope nesting exceeded')
    return runtime,install,scope

def cleanup(env):
    runtime,install,scope=fixture_paths(env)
    release=Path(env['GHOSTERM_SESSION_LINUX_BUNDLE']).joinpath('manifest.tsv').read_text().splitlines()[0].split('\t')[1]
    if not re.fullmatch(r'[A-Za-z0-9._-]+',release):raise RuntimeError('invalid fixture release')
    config={'runtime':str(runtime),'scope':str(scope),'install':str(install),'release':release,'emulated':env.get('GHOSTERM_TEST_EMULATED_X86')=='1'}
    if config['emulated']:
        # User-mode QEMU lacks pidfd and changes vfork semantics. Teardown the
        # exact disposable container before native Linux removes its files.
        identifier=env.get('GHOSTERM_TEST_CONTAINER_ID','')
        if not re.fullmatch('[0-9a-f]{64}',identifier):raise RuntimeError('explicit disposable emulation container ID required')
        details=json.loads(subprocess.check_output(['docker','inspect',identifier],text=True))[0]
        if details['Id']!=identifier or details['Config'].get('Labels',{}).get('dev.ghosterm.fixture')!='sessiond-x86':raise RuntimeError('unexpected emulation container owner')
        subprocess.run(['docker','stop','--timeout','10',identifier],check=True,stdout=subprocess.DEVNULL,timeout=15)
    code='import json\ncfg=json.loads('+repr(json.dumps(config))+')\n'+r'''
from pathlib import Path
import os,select,shutil,signal,socket,stat,struct
runtime=Path(cfg['runtime']); scope=Path(cfg['scope']); root=Path(cfg['install'])
expected=str(root/'releases'/cfg['release']/'ghosterm-sessiond')
def private(path):
 st=path.lstat()
 if not stat.S_ISDIR(st.st_mode) or st.st_uid!=os.getuid() or st.st_mode&0o077:raise RuntimeError('unsafe fixture directory')
if scope.exists() or scope.is_symlink():
 private(scope)
 cursor=scope
 for part in runtime.relative_to(scope).parts:
  cursor=cursor/part
  if cursor.exists() or cursor.is_symlink():private(cursor)
if runtime.exists():
 private(runtime); client=socket.socket(socket.AF_UNIX)
 try:client.connect(str(runtime/'session.sock'))
 except (FileNotFoundError,ConnectionRefusedError):client.close()
 else:
  pid,uid,gid=struct.unpack('3i',client.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12));client.close()
  if uid!=os.getuid() or pid<=1:raise RuntimeError('unexpected fixture peer')
  handle=os.pidfd_open(pid)
  try:
   exe=os.readlink('/proc/'+str(pid)+'/exe')
   argv=Path('/proc/'+str(pid)+'/cmdline').read_bytes().split(b'\0')
   if str(runtime).encode() not in argv:raise RuntimeError('fixture runtime argument mismatch')
   if exe!=expected:
    maps=Path('/proc/'+str(pid)+'/maps').read_text().splitlines()
    mapped=any(line.split()[-1:]==[expected] for line in maps)
    if not cfg['emulated'] or 'qemu' not in Path(exe).name or expected.encode() not in argv or not mapped:raise RuntimeError('unexpected fixture executable')
   signal.pidfd_send_signal(handle,signal.SIGTERM)
   if not select.select([handle],[],[],10)[0]:raise RuntimeError('fixture daemon did not stop')
  finally:os.close(handle)
if scope.exists():shutil.rmtree(scope)
if root.exists():private(root);shutil.rmtree(root)
print('PASS owned Linux setup fixture cleanup')
'''
    cleanup_port=env.get('GHOSTERM_TEST_CLEANUP_SSH_PORT',env['GHOSTERM_REPLICA_SSH_PORT'])
    cleanup_known=env.get('GHOSTERM_TEST_CLEANUP_SSH_KNOWN',env['GHOSTERM_REPLICA_SSH_KNOWN'])
    args=['ssh','-p',cleanup_port,'-i',env['GHOSTERM_REPLICA_SSH_KEY'],'-o','IdentitiesOnly=yes','-o','BatchMode=yes','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile='+cleanup_known,'-o','ConnectTimeout=5',env.get('GHOSTERM_REPLICA_SSH_USER',os.environ.get('USER',''))+'@127.0.0.1','python3 -']
    result=subprocess.run(args,input=code,text=True,capture_output=True,timeout=25)
    if result.returncode:raise RuntimeError('fixture cleanup failed: '+result.stderr[-2000:])
    print(result.stdout,end='',flush=True)

class Family:
    """Track descendant process groups, including XCTest's separate group."""
    def __init__(self, child):
        self.child=child;self.queue=select.kqueue();self.live={};self.seen=set()
    def drain(self):
        for event in self.queue.control(None,max(1,len(self.seen)),0):self.live.pop(event.ident,None)
    def observe(self):
        self.drain()
        rows=[tuple(map(int,line.split())) for line in subprocess.check_output(['ps','-axo','pid,ppid,pgid'],text=True).splitlines()[1:]]
        owned=set(self.live)
        if self.child.poll() is None:owned.add(self.child.pid)
        for _ in range(8):
            previous=len(owned)
            for pid,parent,group in rows:
                if parent in owned:owned.add(pid)
            if len(owned)==previous:break
        for pid,parent,group in rows:
            if pid in owned and pid not in self.seen:
                try:self.queue.control([select.kevent(pid,filter=select.KQ_FILTER_PROC,flags=select.KQ_EV_ADD|select.KQ_EV_ONESHOT,fflags=select.KQ_NOTE_EXIT)],0,0)
                except ProcessLookupError:continue
                self.seen.add(pid);self.live[pid]=group
    def stop(self):
        self.observe()
        for sig,seconds in ((signal.SIGTERM,5),(signal.SIGKILL,5)):
            self.drain()
            groups=set(self.live.values())-{os.getpgrp()}
            if self.child.poll() is None:groups.add(self.child.pid)
            for group in groups:
                try:os.killpg(group,sig)
                except ProcessLookupError:pass
            end=time.monotonic()+seconds
            while self.live and time.monotonic()<end:
                for event in self.queue.control(None,max(1,len(self.seen)),.05):self.live.pop(event.ident,None)
            if not self.live and self.child.poll() is not None:break
        if self.live:raise RuntimeError('owned native setup descendants failed to stop')
        self.child.communicate(timeout=2)
    def close(self):self.queue.close()

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.parse_args()
    env=dict(os.environ,DEVELOPER_DIR=os.environ.get('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer'),TMPDIR=str(ROOT/'tmp'))
    required=('GHOSTERM_REPLICA_RUNTIME','GHOSTERM_SESSION_SETUP_ROOT','GHOSTERM_SESSION_LINUX_BUNDLE','GHOSTERM_REPLICA_SSH_PORT','GHOSTERM_REPLICA_SSH_KEY','GHOSTERM_REPLICA_SSH_KNOWN','GHOSTERM_REPLICA_CONTRACT')
    if any(not env.get(name) for name in required):raise RuntimeError('dedicated Linux setup fixture environment missing')
    # Validate before starting a test, not only in its teardown.
    fixture_paths(env)
    def interrupted(sig,frame):raise RuntimeError('native Linux setup probe interrupted: '+str(sig))
    for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
    package=Path(env.get('GHOSTERM_NATIVE_SETUP_PACKAGE',ROOT/'apple/GhostermApp'))
    target='SessionSetupTransportTests.testRealLinuxInstallNativePaneResumeAndExplicitStop'
    marker='PASS Mac-to-Linux native setup:'
    child=subprocess.Popen(['xcrun','swift','test','--package-path',str(package),'--filter',target],env=env,start_new_session=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    family=Family(child)
    try:
        deadline=time.monotonic()+180
        while True:
            family.observe()
            try:output,_=child.communicate(timeout=.2);break
            except subprocess.TimeoutExpired:
                if time.monotonic()>=deadline:raise RuntimeError('native setup test deadline expired')
        print(output,end='',flush=True)
        if child.returncode or marker not in output or 'Executed 1 test, with 0 failures' not in output:raise RuntimeError('native Linux setup integration failed')
    finally:
        try:family.stop()
        finally:
            family.close()
            cleanup(env)
    return 0
if __name__=='__main__':sys.exit(main())
