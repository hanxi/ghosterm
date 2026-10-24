#!/usr/bin/env python3
"""Build/validate the session engine in tmp, publish only build/ SDK artifacts.
The pinned vendor sources are never patched in place.
"""
import hashlib,json,os,shutil,signal,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PIN='81598385d608dda4946093d6d6a234d204a2f225'
def run(args,**kwargs):
 p=subprocess.Popen([str(x) for x in args],cwd=ROOT,start_new_session=True,**kwargs)
 try:
  result=p.wait()
  if result:raise RuntimeError('session engine command failed: '+str(args[0]))
 except BaseException:
  if p.poll() is None:os.killpg(p.pid,signal.SIGTERM)
  try:p.wait(timeout=10)
  except subprocess.TimeoutExpired:os.killpg(p.pid,signal.SIGKILL);p.wait()
  raise

def publish():
 vendor=ROOT/'vendor/ghostty';engine=ROOT/'tmp/worker-surface/engine'
 subprocess.run(['git','-C',vendor,'diff','--quiet','HEAD','--'],check=True)
 if subprocess.check_output(['git','-C',vendor,'rev-parse','HEAD'],text=True).strip()!=PIN:raise RuntimeError('engine pin mismatch')
 source=engine/'macos/GhosttyKit.xcframework';lib=source/'macos-arm64/libghostty-internal.a'
 if not lib.is_file():raise RuntimeError('validated native SDK missing')
 required=('ghostty_p0_surface_import_once','ghostty_p0_surface_input_enable','ghostty_p0_surface_event_output_for','ghostty_p0_config_replica_colors')
 symbols=subprocess.check_output(['nm','-g','--defined-only',lib],text=True,stderr=subprocess.DEVNULL)
 defined={line.split()[-1].removeprefix('_') for line in symbols.splitlines() if len(line.split())>=3 and line.split()[-2]=='T'}
 if not all(name in defined for name in required):raise RuntimeError('session engine entry points missing')
 staging=Path(tempfile.mkdtemp(prefix='session-sdk-',dir=ROOT/'tmp'))
 backup=ROOT/'tmp/session-sdk-previous';destination=ROOT/'build/GhosttyKit.xcframework';destination.parent.mkdir(exist_ok=True)
 try:
  sdk=staging/'GhosttyKit.xcframework';shutil.copytree(source,sdk,symlinks=True)
  headers=sdk/'macos-arm64/Headers'
  # Allocator-only Surface budget uses engine ABI types, never links the VT
  # runtime into the GUI. Copy the exact public header tree from this build.
  for header in (engine/'include').rglob('*.h'):
   target=headers/header.relative_to(engine/'include');target.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(header,target)
  # Full API is modular; VT allocator-only headers remain textual C includes.
  # An umbrella module would silently hide their typedefs from native C TUs.
  (headers/'module.modulemap').write_text('module GhosttyKit { header "ghostty.h" export * }\n')
  patches=sorted((ROOT/'patches/libghostty/8159838').rglob('*.patch'))
  metadata={'engine':PIN,'profile':'empty-replica-io-import','snapshot_format':0x47545643,
   'patches':{str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in patches},
   'headers':{str(p.relative_to(headers)):hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(headers.rglob('*.h'))},
   'archive_sha256':hashlib.sha256(lib.read_bytes()).hexdigest(),'validation':'11 native import cases including Metal pixels, isolated config and real input'}
  (sdk/'session-engine.json').write_text(json.dumps(metadata,indent=2)+'\n')
  if backup.exists():raise RuntimeError('previous SDK publication requires cleanup: '+str(backup))
  if destination.is_symlink():raise RuntimeError('SDK destination must not be a symlink')
  had=destination.exists()
  if had:destination.rename(backup)
  try:sdk.rename(destination)
  except BaseException:
   if had:backup.rename(destination)
   raise
  if had:shutil.rmtree(backup)
  subprocess.run(['git','-C',vendor,'diff','--quiet','HEAD','--'],check=True)
  print('PASS validated session SDK published in build/; vendor source unchanged: '+str(destination),flush=True)
 finally:shutil.rmtree(staging)

def main():
 if sys.platform!='darwin' or os.uname().machine!='arm64':raise RuntimeError('Apple Silicon macOS builder required')
 (ROOT/'tmp').mkdir(exist_ok=True);os.environ['TMPDIR']=str(ROOT/'tmp');os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
 def interrupted(sig,frame):raise RuntimeError('session engine build interrupted: '+str(sig))
 for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
 run([sys.executable,ROOT/'scripts/probe-session-import.py']);publish();return 0
if __name__=='__main__':sys.exit(main())
