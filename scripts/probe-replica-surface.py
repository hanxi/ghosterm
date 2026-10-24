#!/usr/bin/env python3
"""Native Surface bridge consumes real synthetic VT containers off the UI thread."""
import os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 os.environ['TMPDIR']=str(ROOT/'tmp');os.environ.setdefault('DEVELOPER_DIR','/Applications/Xcode.app/Contents/Developer')
 work=ROOT/'tmp/replica-surface-fixtures';work.mkdir(exist_ok=True)
 headers=ROOT/'build/GhosttyKit.xcframework/macos-arm64/Headers';source=ROOT/'terminal/session-state/full-surface';vt=ROOT/'tmp/worker-surface/out/lib/libghostty-vt.dylib'
 subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-I'+str(headers),'-I'+str(ROOT/'core/include'),'-I'+str(ROOT/'terminal/state'),'-I'+str(ROOT/'terminal/session-state'),source/'replica-fixture.c',source/'import-fixture.c',ROOT/'terminal/session-state/budget-posix.c',ROOT/'terminal/state/checkpoint-wire.c',vt,'-Wl,-rpath,'+str(vt.parent),'-pthread','-o',work/'generate'],check=True)
 for case in ('plain','utf8','image'):
  directory=work/case;directory.mkdir(exist_ok=True);subprocess.run([work/'generate',case,directory],cwd=ROOT,check=True)
 env=dict(os.environ,GHOSTERM_REPLICA_FIXTURES=str(work))
 subprocess.run(['xcrun','swift','test','--package-path',ROOT/'apple/GhostermTerminal','--filter','ReplicaSurfaceTests'],cwd=ROOT,env=env,check=True)
 return 0
if __name__=='__main__':sys.exit(main())
