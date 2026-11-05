#!/usr/bin/env python3
"""Optional Core JS backend, isolated real builds/negative controls. No App enable.
Tools default offline. --mutate edits only copied Core, never vendor/worktree.
"""
import argparse,hashlib,json,os,re,shutil,signal,subprocess,sys,tempfile
from pathlib import Path
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[1];MARK='GT_PLUGIN_RUNTIME_MUTATION'
MODES='invalid roundtrip reverse prototype isolation relative top-await handled stop cpu top-cpu atomics jobs memory stack reject unhandled entry missing-export native-import node-import escape dynamic-import dispose-loop activation-timeout peer-loss'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--mutate',action='store_true');parser.add_argument('--fetch',action='store_true');parser.add_argument('--android-build',action='store_true');args=parser.parse_args()
 runtime=ROOT/'core/src/extension/runtime-quickjs.inc'
 if MARK in runtime.read_text():print('residual runtime mutation',file=sys.stderr);raise SystemExit(2)
 inputs=[p for folder in ['core','packages/plugin-sdk'] for p in (ROOT/folder).rglob('*') if p.is_file() and 'build' not in p.relative_to(ROOT/folder).parts and p.suffix in ['.c','.h','.inc','.cmake','.json','.ts','.md']]
 inputs += [ROOT/p for p in ['core/CMakeLists.txt','.gitmodules','scripts/test-plugin-runtime.py','scripts/fixtures/plugin-sdk-tools/package.json','scripts/fixtures/plugin-sdk-tools/package-lock.json']]
 hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};work=Path(tempfile.mkdtemp(prefix='plugin-runtime-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';commands=[]
 env={**os.environ,'TMPDIR':str(work),'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
 def run(label,cmd,budget=180,red=False,negative=None):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-runtime; shift; gt_timeout "$@"'
  p=subprocess.run(['bash','-c',shell,'plugin-runtime',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion=red,configuration_negative=negative))
  if red:
   if p.returncode!=1 or not p.stderr.startswith('FAIL ') or any(s in out for s in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer']):raise RuntimeError(label+': expected normal C assertion\n'+out)
  elif negative:
   if p.returncode!=1 or negative not in out:raise RuntimeError(label+': configuration not explicitly refused\n'+out)
  elif p.returncode:raise RuntimeError(label+'\n'+out)
  return out
 try:
  shutil.copytree(ROOT/'core',source/'core',ignore=shutil.ignore_patterns('build','build-*'))
  shutil.copytree(ROOT/'packages/plugin-sdk',source/'packages/plugin-sdk')
  (source/'vendor').mkdir();vendor=source/'vendor/quickjs-ng';vendor.symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True)
  tools=work/'tools';tools.mkdir()
  for name in ['package.json','package-lock.json']:shutil.copyfile(ROOT/'scripts/fixtures/plugin-sdk-tools'/name,tools/name)
  lock=json.loads((tools/'package-lock.json').read_text())
  for name,entry in lock['packages'].items():
   if name and (not entry['resolved'].startswith('https://registry.npmjs.org/') or not entry['integrity'].startswith('sha512-')):raise RuntimeError('unpinned developer tool')
  npmrc=work/'empty.npmrc';npmrc.touch();env['NPM_CONFIG_USERCONFIG']=str(npmrc)
  cmd=['npm','ci','--prefix',tools,'--ignore-scripts','--no-audit','--no-fund','--cache',ROOT/'tmp/plugin-sdk-npm-cache','--registry','https://registry.npmjs.org']
  if not args.fetch:cmd.append('--offline')
  run('tools',cmd);sdk=work/'sdk';run('typescript',['node',tools/'node_modules/typescript/bin/tsc','-p',source/'packages/plugin-sdk/tsconfig.json','--outDir',sdk])
  build=work/'build';core=source/'core'
  base=['cmake','-S',core,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_BUILD_TESTS=ON','-DGHOSTERM_PLUGIN_SDK_DIR='+str(sdk),'-DCMAKE_BUILD_TYPE=RelWithDebInfo']
  run('configure',[*base,'-B',build,'-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'],budget=120)
  def rebuild(label):
   objects=[('ghosterm_core','src/extension/runtime.c'),('test_runtime','tests/test_runtime.c'),('test_runtime_boundary','tests/test_runtime_boundary.c')]
   for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
   for name in ['libghosterm_core.a','test_runtime','test_runtime_boundary','test_config']:(build/name).unlink(missing_ok=True)
   out=run(label+'-build',['cmake','--build',build,'--target','test_runtime','test_runtime_boundary','test_config','-j','2'],budget=300)
   for owner,name in objects:
    assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in out,(label,name)
   for name in ['test_runtime','test_runtime_boundary','test_config']:assert 'Linking C executable '+name in out,(label,name)
   assert 'Linking C static library libghosterm_core.a' in out
  def suite(label):
   expected={'runtime_'+m for m in MODES}|{'runtime_boundary_outbound'}
   discovered=json.loads(run(label+'-discover',['ctest','--test-dir',build,'--show-only=json-v1']))
   assert {t['name'] for t in discovered['tests'] if t['name'].startswith('runtime_')}==expected
   output=run(label,['ctest','--test-dir',build,'-R','^runtime_','--output-on-failure'],budget=90)
   assert '100% tests passed out of '+str(len(expected)) in output
   for name in expected:assert re.search(r'Test\s+#\d+: '+re.escape(name)+r'\s+\.+\s+Passed',output),name
  rebuild('baseline');suite('baseline')
  cases=[
   ('module-digest',' || strcmp(hex,m->sha256)',' || false','invalid'),
   ('role-binding','if((unsigned)actual_role!=(unsigned)c->role)','if(false && (unsigned)actual_role!=(unsigned)c->role)','invalid'),
   ('copy-source','memcpy(copy->source,m->source,m->size);','memset(copy->source,32,m->size);','roundtrip'),
   ('native-module-boundary','!strcmp(base,"host:/bootstrap.js")','true','native-import'),
   ('cpu-budget','now>=r->deadline','now>=r->deadline+60000','cpu'),
   ('atomics-block','JS_SetCanBlock(r->engine,false);','JS_SetCanBlock(r->engine,true);','atomics'),
   ('memory-budget','JS_SetMemoryLimit(r->engine,memory);','JS_SetMemoryLimit(r->engine,128*1024*1024);','memory'),
   ('job-budget','if(*used>=r->jobs_per_step)','if(*used>=4096)','jobs'),
   ('unhandled-rejection','if(!handled){if(r->unhandled_count==64)','if(false && !handled){if(r->unhandled_count==64)','unhandled'),
   ('handled-rejection','if(handled){JS_FreeValue(ctx,r->unhandled[i]);','if(false && handled){JS_FreeValue(ctx,r->unhandled[i]);','handled'),
   ('stop-signal','if(r)atomic_store_explicit(&r->stopped,true,memory_order_relaxed);','if(r)atomic_store_explicit(&r->stopped,false,memory_order_relaxed);','stop'),
   ('engine-release',' runtime_free_engine(r);',' /* engine retained */','roundtrip'),
   ('handoff-revoke','gt_ext_session_revoke(r->session);}\nstatic int runtime_interrupt','(void)r->session;}\nstatic int runtime_interrupt','boundary'),
  ]
  if args.mutate:
   path=core/'src/extension/runtime-quickjs.inc';original=path.read_bytes()
   for label,old,new,mode in cases:
    assert old in original.decode(),label;path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n')
    target=[build/'test_runtime_boundary',sdk] if mode=='boundary' else [build/'test_runtime',mode,sdk]
    try:
     rebuild(label);run(label+'-target',target,budget=30,red=True);run(label+'-control',[build/'test_config',core/'tests/fixtures'])
    finally:path.write_bytes(original)
    rebuild(label+'-restored');run(label+'-restored',target,budget=30);assert sha(path)==hashlib.sha256(original).hexdigest();print(label+': actual C/archive/executable assertion red / same archive config green / restored / SHA256',flush=True)
  suite('final')
  # Fresh CMake invocations must reject capability/dependency/test omissions.
  run('guard-provider',[*base,'-B',work/'guard-provider','-DGHOSTERM_WITH_PLUGIN_CHANNEL=OFF'],negative='Plugin JS requires')
  run('guard-test-input',[*base,'-B',work/'guard-test-input','-DGHOSTERM_PLUGIN_SDK_DIR='],negative='Enabled runtime tests require')
  pin=core/'cmake/PluginJS.cmake';original=pin.read_bytes()
  try:
   pin.write_text(original.decode().replace('d7215b0ad3fc2c562e8317fd9bca058fd4863804','0'*40));run('guard-pin',[*base,'-B',work/'guard-pin'],negative='commit differs')
  finally:pin.write_bytes(original)
  vendor.unlink();vendor.mkdir()
  gitdir=run('vendor-gitdir',['git','-C',ROOT/'vendor/quickjs-ng','rev-parse','--absolute-git-dir']).strip();(vendor/'.git').write_text('gitdir: '+gitdir+'\n')
  try:
   for p in (ROOT/'vendor/quickjs-ng').iterdir():
    if p.suffix in ['.c','.h'] or p.name=='LICENSE':shutil.copyfile(p,vendor/p.name)
   with (vendor/'quickjs.c').open('a') as f:f.write('\n/* isolated source damage */\n')
   run('guard-content',[*base,'-B',work/'guard-content'],negative='source modified: quickjs.c')
  finally:shutil.rmtree(vendor);vendor.symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True)
  disabled=work/'disabled';run('disabled-configure',['cmake','-S',core,'-B',disabled,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_WITH_PLUGIN_JS=OFF']);run('disabled-build',['cmake','--build',disabled,'--target','test_runtime','-j','2'],budget=180);run('disabled-test',[disabled/'test_runtime','disabled'])
  symbols=run('symbols',['nm','-g','--defined-only',build/'libghosterm_core.a']);declared=set()
  for p in (core/'include').rglob('*.h'):declared.update(n for n in re.findall(r'\b(gt_[a-z][a-z0-9_]*)\s*\(',p.read_text()) if not n.endswith('_t'))
  actual={m.group(1) for m in re.finditer(r'\bT\s+_?(gt_[a-z][a-z0-9_]*)\b',symbols)};assert actual==declared
  android=None
  if args.android_build:
   ndk=ROOT/'build/android-toolchain/sdk/ndk/28.2.13676358';android_build=work/'android'
   run('android-configure',[*base,'-B',android_build,'-DCMAKE_TOOLCHAIN_FILE='+str(ndk/'build/cmake/android.toolchain.cmake'),'-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-29','-DCMAKE_C_FLAGS='],budget=120)
   run('android-build',['cmake','--build',android_build,'--target','test_runtime','-j','2'],budget=300)
   binary=android_build/'test_runtime';nm=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-nm';exports=run('android-symbols',[nm,'--defined-only',binary]);assert 'gt__bridge_runtime_open' in exports and 'JS_NewRuntime' in exports;assert binary.read_bytes()[:4]==b'\x7fELF'
   android=dict(abi='arm64-v8a',api=29,executed=False,binary_sha256=sha(binary),core_sha256=sha(android_build/'libghosterm_core.a'),engine_sha256=sha(android_build/'libghosterm_quickjs.a'))
  for name,h in hashes.items():assert sha(ROOT/name)==h,name
  receipt=dict(accepted=True,inputs_sha256=hashes,commands=commands,groups=[c[0] for c in cases] if args.mutate else [],runtime_tests=len(MODES)+1,runtime_modes=MODES,guards=4,abi=len(actual),core_sha256=sha(build/'libghosterm_core.a'),engine_sha256=sha(build/'libghosterm_quickjs.a'),android=android,sanitizers=['address','undefined'],leak_sanitizer=False,scope='optional private C runtime / copied modules / native RPC; no public runtime ABI, App enable, OS effects, gterm, physical devices, WAN or gateway')
  (logs/'result.json').write_text(json.dumps(receipt,indent=2)+'\n');shutil.rmtree(source);print('PLUGIN RUNTIME PASS; logs: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(n,_frame):raise KeyboardInterrupt('signal '+str(n))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
