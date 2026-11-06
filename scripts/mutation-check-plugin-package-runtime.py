#!/usr/bin/env python3
"""Compile package-loader mutations in a private copy; crashes never count as red."""
import argparse,hashlib,json,os,re,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_PACKAGE_RUNTIME_MUTATION'
PACKAGE='core/src/extension/package-runtime.c'
ENGINE='core/src/extension/runtime-quickjs.inc'
SESSION='core/src/extension/session.c'
CASES=[
 ('stop-before-load',ENGINE,'if(atomic_load_explicit(&r->stopped,memory_order_relaxed)){runtime_fail(r,GT_RUNTIME_CANCELLED);return r->info.phase;}','if(false && atomic_load_explicit(&r->stopped,memory_order_relaxed)){runtime_fail(r,GT_RUNTIME_CANCELLED);return r->info.phase;}','stop-created'),
 ('destroy-before-close',ENGINE,'(*out)->info.phase!=GT_RUNTIME_CLOSED','false','close-created'),
 ('identity',SESSION,'!strcmp(s->config.plugin_id,id)','true','id'),
 ('version',SESSION,'!strcmp(s->config.plugin_version,version)','true','version'),
 ('digest',SESSION,'!strcmp(role==GT_BRIDGE_HOST?s->config.client_digest:s->config.executor_digest,digest)','true','digest'),
 ('sdk',PACKAGE,'strcmp(info.sdk_version,GT_PLUGIN_SDK_API_VERSION)','false','sdk-version'),
 ('association',PACKAGE,'info.association_required && !associated','false && info.association_required && !associated','association'),
 ('permission',PACKAGE,'gt__bridge_session_access(session,lease,name)!=GT_BRIDGE_ACCESS_ALLOWED','false','ungranted'),
 ('native-import',ENGINE,'!strcmp(name,"host:/native") && !strcmp(base,"host:/bootstrap.js")','!strcmp(name,"host:/native")','native'),
 ('source-size',ENGINE,'source.size>128*1024','false','source-limit'),
 ('source-total',ENGINE,'source.size>1024*1024-r->info.module_bytes','false','source-total'),
 ('module-count',ENGINE,'r->count>=64','r->count>=63','module-boundary'),
 ('utf8',ENGINE,'!gt__runtime_source_utf8(copy,used)','false','utf8'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--public',action='store_true');args=parser.parse_args()
 cases=CASES
 executable='test_package_runtime';pattern='^plugin_package_runtime_';objects=['runtime','package-runtime','session']
 if args.public:
  api='core/src/extension/package-api.c';runtime='core/src/extension/runtime-api.c'
  cases=[
   ('capture-cancel',api,'if(p->info.phase!=GT_EXT_PACKAGE_CAPTURING)return p->info.phase;','if(p->info.phase!=GT_EXT_PACKAGE_CAPTURING)return p->info.phase; atomic_store(&p->stopped,false);','capture-stop'),
   ('check-cancel',api,'if(p->info.phase!=GT_EXT_PACKAGE_CAPTURED)return p->info.phase;','if(p->info.phase!=GT_EXT_PACKAGE_CAPTURED)return p->info.phase; atomic_store(&p->stopped,false);','check-stop'),
   ('unknown-preserved',api,'case GT_PACKAGE_CHECK_UNKNOWN:error=GT_EXT_PACKAGE_UNKNOWN;','case GT_PACKAGE_CHECK_UNKNOWN:error=GT_EXT_PACKAGE_UNSUPPORTED;','unknown'),
   ('metadata-zero',api,'if(out)memset(out,0,sizeof(*out));if(!p || !out || p->info.phase!=GT_EXT_PACKAGE_CHECKED)','if(!p || !out || p->info.phase!=GT_EXT_PACKAGE_CHECKED)','roundtrip'),
   ('digest-copy',api,'COPY(package_digest);','COPY(manifest_digest);','roundtrip'),
   ('stopped-adoption',runtime,'if(atomic_load_explicit(&package->stopped,memory_order_relaxed))return GT_ERR_CANCELLED;','if(false && atomic_load_explicit(&package->stopped,memory_order_relaxed))return GT_ERR_CANCELLED;','load-stop'),
  ]
  executable='test_ext_package';pattern='^ext_package_';objects=['package-api','runtime-api','runtime','package-runtime','session']
 for _,name,old,_,_ in cases:
  data=(ROOT/name).read_text();assert MARK not in data and data.count(old)==1,name
 work=Path(tempfile.mkdtemp(prefix='pkgload-',dir=ROOT/'tmp'));source=work/'source';build=work/'build';logs=work/'logs';logs.mkdir();commands=[]
 env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
 def run(label,args,red=False):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-runtime; shift; gt_timeout 300 "$@"'
  p=subprocess.run(['bash','-c',shell,'package-runtime',str(ROOT),*map(str,args)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=310)
  out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion=red))
  if red:assert p.returncode==1 and re.search(r'^FAIL ',p.stderr,re.M) and not any(x in out for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer']),label+'\n'+out
  else:assert p.returncode==0,label+'\n'+out
  return out
 try:
  for directory in ['core','packages/plugin-sdk','patches/quickjs-ng','scripts']:
   shutil.copytree(ROOT/directory,source/directory,ignore=shutil.ignore_patterns('build','build-*','__pycache__'))
  (source/'vendor').mkdir();(source/'vendor/quickjs-ng').symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True);(source/'tmp').mkdir()
  hashes={str(p.relative_to(source)):sha(p) for d in ['core','packages/plugin-sdk','patches/quickjs-ng','scripts'] for p in (source/d).rglob('*') if p.is_file()}
  tools=work/'tools';shutil.copytree(ROOT/'scripts/fixtures/plugin-sdk-tools',tools)
  run('tools',['npm','ci','--prefix',tools,'--ignore-scripts','--no-audit','--no-fund','--offline','--cache',ROOT/'tmp/plugin-sdk-npm-cache'])
  sdk=work/'sdk';run('sdk',['node',tools/'node_modules/typescript/bin/tsc','-p',source/'packages/plugin-sdk/tsconfig.json','--outDir',sdk])
  run('configure',['cmake','-S',source/'core','-B',build,'-DGHOSTERM_PLUGIN_SDK_DIR='+str(sdk),'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_WITH_PLUGIN_JS=ON','-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
  def rebuild(label):
   for name in objects:(build/'CMakeFiles/ghosterm_core.dir/src/extension'/(name+'.c.o')).unlink(missing_ok=True)
   for name in [executable,'test_config','libghosterm_core.a']:(build/name).unlink(missing_ok=True)
   out=run(label+'-build',['cmake','--build',build,'--target',executable,'test_config','-j','4'])
   for name in objects:assert 'src/extension/'+name+'.c.o' in out
   assert 'Linking C static library' in out and 'Linking C executable '+executable in out
  def target(label,mode,red=False):run(label,[build/executable,mode,source/'tmp'],red)
  rebuild('baseline');run('baseline',['ctest','--test-dir',build,'-R',pattern,'--output-on-failure'])
  for label,name,old,new,mode in cases:
   path=source/name;original=path.read_bytes()
   try:
    path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n')
    rebuild(label);target(label+'-red',mode,True);run(label+'-control',[build/'test_config',source/'core/tests/fixtures'])
   finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
   rebuild(label+'-restored');target(label+'-restored',mode);print(label+': assertion red / configuration green / restored green / SHA256',flush=True)
  run('final',['ctest','--test-dir',build,'-R',pattern,'--output-on-failure'])
  for name,digest in hashes.items():assert sha(source/name)==digest and sha(ROOT/name)==digest,name
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False),indent=2)+'\n')
  print('PACKAGE RUNTIME MUTATIONS PASS: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error));raise
def stop(number,frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':
 signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
