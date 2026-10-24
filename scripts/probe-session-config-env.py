#!/usr/bin/env python3
"""P0 request-owned configuration environment and explicit expansion failures."""
from pathlib import Path
import argparse,os,subprocess,signal,hashlib,re,runpy,shutil,sys
root=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutations',action='store_true');args=parser.parse_args()
surface=runpy.run_path(str(root/'scripts/probe-session-worker-surface.py'))
patches=surface['PATCHES'];fixture=root/'daemon/probes/session-config-env-test.zig'
mark='GT_CONFIG_ENV_MUTATION_MARK'
inputs=(*patches,fixture,Path(__file__).resolve())
hashes={p:hashlib.sha256(p.read_bytes()).digest() for p in inputs}
if any(mark in p.read_text() for p in (*patches,fixture)):raise SystemExit(2)
work=root/'tmp/config-env';work.mkdir(parents=True,exist_ok=True)
def interrupted(signum,frame):raise RuntimeError('interrupted '+str(signum))
for sig in (signal.SIGINT,signal.SIGTERM):signal.signal(sig,interrupted)
env=os.environ.copy();env.update(TMPDIR=str(root/'tmp'),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',ZIG_LOCAL_CACHE_DIR=str(work/'cache'),ZIG_GLOBAL_CACHE_DIR=str(root/'tmp/zig-global'))
zig=env.get('ZIG') or shutil.which('zig') or str(root/'tmp/zig/zig-aarch64-macos-0.16.0/zig')
if sys.platform!='darwin':raise RuntimeError('this evidence requires macOS; no silent skip')
if subprocess.check_output([zig,'version'],text=True).strip()!='0.16.0':raise RuntimeError('Zig 0.16.0 required')
os.environ.update({k:env[k] for k in ('TMPDIR','DEVELOPER_DIR','ZIG_LOCAL_CACHE_DIR','ZIG_GLOBAL_CACHE_DIR')})
for name in ('engine','engine.tar'):
 p=work/name
 if p.is_dir():shutil.rmtree(p)
 elif p.exists():p.unlink()
engine=surface['FULL']['prepare'](work)
for patch in patches:
 subprocess.run(['git','-C',str(engine),'apply','--check',str(patch)],check=True)
 subprocess.run(['git','-C',str(engine),'apply',str(patch)],check=True)
p=engine/'src/config/Config.zig';p.write_text(p.read_text()+fixture.read_text())
p=engine/'build.zig';text=p.read_text();anchor='        addGhosttyH(b, test_exe.root_module, config.baselineTarget(b.graph.io), .Debug);'
if text.count(anchor)!=1:raise RuntimeError('nonunique compile-only unit anchor')
p.write_text(text.replace(anchor,anchor+'\n        const p0_config_unit_install = b.addInstallArtifact(test_exe, .{});\n        b.step("p0-config-env-build", "Compile isolated configuration tests").dependOn(&p0_config_unit_install.step);'))
paths=[engine/'src/global.zig',engine/'src/config/Config.zig'];before={p:p.read_bytes() for p in paths}
if any(mark.encode() in b for b in before.values()):raise SystemExit(2)
cmd=[zig,'build','p0-config-env-build','-Demit-xcframework=true','-Demit-macos-app=false','-Dsentry=false','-Dtest-filter=p0-config-env','-p',str(work/'out')]
def run(command,cwd,env,log):
 with log.open('w') as out:
  p=subprocess.Popen(command,cwd=cwd,env=env,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
  try:rc=p.wait(timeout=600)
  except BaseException:
   os.killpg(p.pid,signal.SIGKILL);p.wait();raise
 return rc,log.read_text()
def build(label):
 rc,out=run(cmd,engine,env,work/(label+'-build.log'))
 if rc:raise RuntimeError('compile failed '+label+' '+out[-3000:])
def unit(label,negative=None,control=None):
 e=env.copy();e['HOME']='/ghosterm-p0-config-home'
 rc,out=run([str(work/'out/bin/ghostty-test')],root,e,work/(label+'-unit.log'))
 if negative:
  if rc!=1 or not re.search(r'p0-config-env '+re.escape(negative)+r'\.\.\.(?:(?!\n[0-9]+/[0-9]+ ).)*FAIL \(', out, re.S) or 'p0-config-env '+control+'...OK' not in out:raise RuntimeError('target/control failed '+label+' '+out[-4000:])
 elif rc!=0 or 'All 83 tests passed.' not in out:raise RuntimeError('baseline failed '+label+' '+out[-4000:])
control='absolute control does not allocate'
specs=[('allocator',paths[0],'return environ().createMap(allocator);','_ = allocator; return environ().createMap(alloc());','selected allocator failure propagates',control),('swallow',paths[1],'return try internal_os.expandHome(global.io(), &environ_map, path, buf);','return internal_os.expandHome(global.io(), &environ_map, path, buf) catch path;','home expansion and short buffer',control),('working-directory',paths[1],'const expanded = try expandHome(alloc, path, &buf);','_ = &buf; const expanded = path;','working directory failure and control','light theme failure and control'),('light',paths[1],'const light = try expandHome(alloc, self.light, &buf);','const light = self.light;','light theme failure and control','dark theme failure and control'),('dark',paths[1],'const dark = try expandHome(alloc, self.dark, &buf);','const dark = self.dark;','dark theme failure and control','light theme failure and control')]
try:
 build('baseline');unit('baseline');print('PASS baseline 83',flush=True)
 for label,p,old,new,target,ctl in (specs if args.mutations else ()):
  s=before[p].decode();assert s.count(old)==1,(label,s.count(old));p.write_text(s.replace(old,new+' // '+mark))
  build(label);unit(label,target,ctl);p.write_bytes(before[p]);print('PASS '+label+' target red/control green',flush=True)
finally:
 for p,b in before.items():p.write_bytes(b)
 for p,b in before.items():assert hashlib.sha256(p.read_bytes()).digest()==hashlib.sha256(b).digest()
build('rollback');unit('rollback');print('PASS SHA256 rollback and 83 green',flush=True)

for p,digest in hashes.items():
 if hashlib.sha256(p.read_bytes()).digest()!=digest:raise RuntimeError("repository input changed: "+str(p))
