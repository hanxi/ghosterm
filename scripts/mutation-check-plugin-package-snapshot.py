#!/usr/bin/env python3
"""Physical C package capture mutations, no plugin execution or gateway.
Normal assertions only, same archive config controls, exact restore and digests.
"""
import argparse, hashlib, json, os, re, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_PACKAGE_SNAPSHOT_MUTATION'
SOURCE='core/src/extension/package-snapshot.c'
HEADER='core/src/extension/package-snapshot.h'
TEST='core/tests/test_package_snapshot.c'
BOUNDARY='core/tests/test_package_snapshot_boundary.c'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--case',action='append',default=[]);args=parser.parse_args()
 for name in [SOURCE,HEADER,TEST,BOUNDARY]:
  if MARK in (ROOT/name).read_text():print('residual package snapshot mutation: '+name,file=sys.stderr);raise SystemExit(2)
 cases=[
  ('member-budget','s->count==s->config.members','false','member-limit'),
  ('entry-budget','s->used-1==s->config.entries','s->used==s->config.entries','entry-exact'),
  ('member-byte-budget','(uint64_t)st.st_size>s->config.member_bytes','false','byte-limit'),
  ('total-byte-budget','(uint64_t)st.st_size>s->config.total_bytes-s->total','false','total-limit'),
  ('physical-metadata-budget','(uint64_t)st.st_size>s->config.total_bytes-s->total','false','physical-self-count'),
  ('hardlink-rejected','st.st_nlink!=1','false','hardlink'),
  ('path-backslash',"cp=='\\\\'",'false','bad-backslash'),
  ('path-colon',"cp==':'",'false','bad-colon'),
  ('path-dot',"p[end-1]=='.'",'false','bad-trailing-dot'),
  ('path-space',"p[end-1]==' '",'false','bad-trailing-space'),
  ('path-control','cp<32','false','bad-control'),
  ('utf8-overlong','cp<minimum','(minimum && false)','boundary:bad-utf8'),
  ('duplicate-entry','if(package_ascii_equal(path,s->entries[i].path))','if(false && package_ascii_equal(path,s->entries[i].path))','boundary:duplicate-entry'),
  ('ascii-case-collision',"if(x>='A' && x<='Z')x+=32;",'/* case-sensitive */','boundary:case-entry'),
  ('replacement-identity','!S_ISREG(opened.st_mode) || !package_same(&st,&opened)','!S_ISREG(opened.st_mode)','race-replacement'),
  ('truncated-read','if(!got)return package_fail(s,GT_PACKAGE_CHANGED);','if(!got)return package_fail(s,GT_PACKAGE_IO);','boundary:read-truncated'),
  ('extra-eof-byte','if(got)return package_fail(s,GT_PACKAGE_CHANGED);','if(got)return package_fail(s,GT_PACKAGE_IO);','boundary:read-grown'),
  ('read-error-not-unknown','if(errno==EINTR)return GT_PACKAGE_WAIT;return package_fail(s,GT_PACKAGE_IO);','if(errno==EINTR)return GT_PACKAGE_WAIT;return package_fail(s,GT_PACKAGE_CHANGED);','boundary:read-error'),
  ('readdir-error','if(error || fstat(dirfd(s->directory),&st))','if((false && error) || fstat(dirfd(s->directory),&st))','boundary:readdir-error'),
  ('sticky-failure','if(s->status!=GT_PACKAGE_WAIT)return s->status;','if(s->status!=GT_PACKAGE_WAIT)return GT_PACKAGE_OK;','byte-limit'),
  ('partial-catalog','!s || !s->ready || !count || !bytes','!s || !count || !bytes','roundtrip'),
  ('read-offset','if(offset>size)return GT_PACKAGE_INVALID;','if(offset>size)return GT_PACKAGE_OK;','roundtrip'),
  ('chunk-cap','cap>GT_PACKAGE_CHUNK_MAX','false','roundtrip'),
  ('sha256-content','if(!gt__sha256_final(&s->hash,e->sha256))','if(!gt__sha256_final(&s->hash,e->sha256) || (e->sha256[0]=\'0\',false))','roundtrip'),
  ('memory-wiped','gt__secure_erase(p,n);gt__plugin_memory_free(&s->allocator,p,n);','s->allocator.release(s->allocator.context,p,n);','roundtrip'),
  ('root-cloexec','s->root=openat(c->directory_fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);','s->root=openat(c->directory_fd,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW);','roundtrip'),
  ('file-cloexec','O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK','O_RDONLY|O_NOFOLLOW|O_NONBLOCK','roundtrip'),
  ('destroy-error-visible','return failed?GT_PACKAGE_IO:GT_PACKAGE_OK;','(void)failed;return GT_PACKAGE_OK;','boundary:destroy-error'),
 ]
 known={c[0] for c in cases}
 if set(args.case)-known:parser.error('unknown case')
 if args.case:cases=[c for c in cases if c[0] in args.case]
 # Two read-error branches share this guard; review/rewrite both together.
 for label,old,*_ in cases:
  count=(ROOT/SOURCE).read_text().count(old);expected=2 if label=='read-error-not-unknown' else 1
  if count!=expected:raise RuntimeError('missing/ambiguous anchor '+label+': '+str(count))
 inputs=[p for p in (ROOT/'core').rglob('*') if p.is_file() and not any(x.startswith('build') for x in p.relative_to(ROOT/'core').parts)]+[Path(__file__)]
 hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs}
 work=Path(tempfile.mkdtemp(prefix='pkgcap-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build';io=work/'io';io.mkdir();commands=[]
 env=dict(os.environ,TMPDIR=str(work),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 def run(label,cmd,red=False,budget=180):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-snapshot; shift; gt_timeout "$@"'
  p=subprocess.run(['bash','-c',shell,'package-snapshot',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion=red))
  accepted=p.returncode==0
  if red:accepted=p.returncode==1 and p.stderr.startswith('FAIL ') and not any(x in out for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer'])
  if not accepted:raise RuntimeError(label+'\n'+out)
  return out
 try:
  shutil.copytree(ROOT/'core',source,ignore=shutil.ignore_patterns('build','build-*'))
  run('configure',['cmake','-S',source,'-B',build,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_WITH_PLUGIN_JS=OFF','-DCMAKE_BUILD_TYPE=Debug','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
  binaries=['test_package_snapshot','test_package_snapshot_boundary','test_config'];objects=[('ghosterm_core','src/extension/package-snapshot.c'),('test_package_snapshot','tests/test_package_snapshot.c'),('test_package_snapshot_boundary','tests/test_package_snapshot_boundary.c')]
  def rebuild(label):
   for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
   for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
   out=run(label+'-build',['cmake','--build',build,'--target',*binaries,'-j','2'],budget=300)
   for owner,name in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in out,(label,name)
   for name in binaries:assert 'Linking C executable '+name in out,(label,name)
   assert 'Linking C static library libghosterm_core.a' in out
  def target(label,mode,red=False):
   boundary=mode.startswith('boundary:');mode=mode.removeprefix('boundary:');binary=binaries[1 if boundary else 0]
   try:run(label,[build/binary,mode,io],red=red,budget=60)
   finally:
    for p in io.iterdir():
     if p.is_dir() and not p.is_symlink():shutil.rmtree(p)
     else:p.unlink()
  def suite(label):
   tests=json.loads(run(label+'-discovery',['ctest','--test-dir',build,'--show-only=json-v1']))['tests'];selected=[t for t in tests if t['name'].startswith('plugin_package_snapshot_')];assert len(selected)==41,len(selected)
   for t in selected:
    boundary=t['name'].startswith('plugin_package_snapshot_boundary_');mode=t['command'][1];target(label+'-'+('boundary-' if boundary else '')+mode,('boundary:' if boundary else '')+mode)
  rebuild('baseline');suite('baseline');run('baseline-control',[build/'test_config',source/'tests/fixtures'])
  path=source/'src/extension/package-snapshot.c';original=path.read_bytes()
  for label,old,new,mode in cases:
   try:
    path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n');rebuild(label);target(label+'-target',mode,True);run(label+'-control',[build/'test_config',source/'tests/fixtures'])
   finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
   rebuild(label+'-restored');target(label+'-restored',mode);print(label+': compiled assertion red / same archive config green / rebuilt restore green / SHA256',flush=True)
  suite('final')
  for name,h in hashes.items():assert sha(ROOT/name)==h,name
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=hashes,commands=commands,cases=41,c_sanitizers=['address','undefined'],leak_sanitizer=False,scope='physical native package capture only; no package trust/activation, App, gateway or release'),indent=2)+'\n');shutil.rmtree(source);shutil.rmtree(build);print('PACKAGE SNAPSHOT MUTATIONS PASS; logs: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,_frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
