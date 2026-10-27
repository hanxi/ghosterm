#!/usr/bin/env python3
"""Validate matching local/Linux runtime inventories and stage a native catalog."""
import argparse,hashlib,json,os,re,shutil,stat,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
LINUX_FILES=('ghosterm-sessiond','ghosterm-session-proxy','lib/libghostty-vt.so.0','LICENSE','SOURCE-INFO.json','sources.tar.gz')
MAC_FILES=('ghosterm-sessiond','ghosterm-session-proxy','lib/libghostty-vt.dylib','LICENSE','SOURCE-INFO.json')
def token(value):return isinstance(value,str) and bool(re.fullmatch('[A-Za-z0-9][A-Za-z0-9._-]{0,127}',value))
def digest(path):
 st=path.lstat()
 if not stat.S_ISREG(st.st_mode) or st.st_mode&0o022:raise RuntimeError('unsafe runtime inventory file: '+str(path))
 with path.open('rb') as stream:return hashlib.file_digest(stream,'sha256').hexdigest()
def macos(path):
 value=json.loads((path/'runtime.json').read_text())
 if value.get('format')!=1 or value.get('platform')!='macos' or value.get('arch')!='arm64' or not token(value.get('release')) or not re.fullmatch('[0-9a-f]{64}',value.get('contract','')) or set(value.get('files',{}))!=set(MAC_FILES):raise RuntimeError('invalid macOS runtime manifest')
 for name,sha in value['files'].items():
  if not re.fullmatch('[0-9a-f]{64}',sha) or digest(path/name)!=sha:raise RuntimeError('macOS runtime digest mismatch/'+name)
 return value

def linux(path,contract):
 lines=(path/'manifest.tsv').read_text().splitlines();header=lines[0].split('\t')
 if len(header)!=6 or header[0]!='GTB1' or not token(header[1]) or header[2] not in ('aarch64','x86_64') or header[5]!=contract or len(lines)!=len(LINUX_FILES)+1:raise RuntimeError('Linux bundle identity mismatch')
 seen=set()
 for line in lines[1:]:
  entry=line.split('\t')
  if len(entry)!=3 or entry[0] not in LINUX_FILES or entry[0] in seen or not entry[1].isdigit() or not re.fullmatch('[0-9a-f]{64}',entry[2]):raise RuntimeError('invalid Linux inventory')
  seen.add(entry[0]);p=path/entry[0]
  if digest(p)!=entry[2] or p.stat().st_size!=int(entry[1]):raise RuntimeError('Linux bundle digest mismatch/'+entry[0])
 return header[2],header[1]

def dedupe_sources(base):
 # 每个架构都附带同一份源码快照。硬链接成一份物理副本，
 # 全架构的 app 不为每个 Linux release 都付一遍源码包的体积。
 seen={}
 for path in sorted(base.rglob('sources.tar.gz')):
  value=digest(path)
  if value in seen:
   path.unlink();os.link(seen[value],path)
  else:seen[value]=path

def copy_release(source,target,native):
 if target.is_symlink():raise RuntimeError('runtime destination symlink')
 names=(*MAC_FILES,'runtime.json') if native else (*LINUX_FILES,'manifest.tsv','manifest.json')
 if target.exists():
  for name in names:
   if digest(source/name)!=digest(target/name):raise RuntimeError('immutable runtime release differs/'+name)
  return
 target.parent.mkdir(parents=True,exist_ok=True)
 stage=Path(tempfile.mkdtemp(prefix='.session-stage-',dir=ROOT/'tmp'))
 try:
  for name in names:
   out=stage/name;out.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(source/name,out)
   out.chmod(0o755 if native and name in MAC_FILES[:2] else 0o644)
  for directory in [stage,*[p for p in stage.rglob('*') if p.is_dir()]]:directory.chmod(0o755)
  stage.rename(target)
 finally:
  if stage.exists():shutil.rmtree(stage)

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--macos',type=Path,required=True);p.add_argument('--linux',type=Path,action='append',default=[]);p.add_argument('--out',type=Path,default=ROOT/'build/runtime');p.add_argument('--app',type=Path);args=p.parse_args()
 os.umask(0o022);local=macos(args.macos);contract=local['contract'];entries={}
 for bundle in args.linux:
  arch,release=linux(bundle,contract)
  if arch in entries:raise RuntimeError('duplicate Linux architecture')
  entries[arch]=(release,bundle)
 if args.app:
  base=args.app/'Contents/Resources/sessiond';native=base/'macos-arm64'
 else:base=args.out;native=base/'macos-arm64'
 copy_release(args.macos,native/local['release'],True)
 for arch,(release,bundle) in entries.items():copy_release(bundle,base/'linux'/arch/release,False)
 dedupe_sources(base)
 catalog=dict(format=1,contract=contract,local=local['release'],linux={arch:release for arch,(release,_) in entries.items()})
 base.mkdir(parents=True,exist_ok=True);temporary=ROOT/'tmp'/('.catalog-'+str(os.getpid()))
 try:temporary.write_text(json.dumps(catalog,sort_keys=True,indent=2)+'\n');temporary.replace(base/'catalog.json')
 finally:
  if temporary.exists():temporary.unlink()
 print('PASS matching native session runtime catalog: '+str(base/'catalog.json'))
 return 0
if __name__=='__main__':raise SystemExit(main())
