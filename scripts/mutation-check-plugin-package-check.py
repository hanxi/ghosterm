#!/usr/bin/env python3
"""Isolated compiled native package checks and regenerated semantic mutations.
No build failure/crash/timeout counts as an assertion red. No App or gateway.
"""
import argparse,hashlib,json,os,re,shutil,signal,subprocess,sys,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_PLUGIN_PACKAGE_CHECK_MUTATION'
C='core/src/extension/package-check.c';BACKEND='core/src/extension/package-check-js.inc';JSON='core/src/extension/package-json.c';LEX='core/src/base/json-reader.inc';SNAPSHOT='core/src/extension/package-snapshot.c';JS='packages/plugin-sdk/testkit/package-metadata.mjs'
GENERATED=['core/src/extension/package-validator-'+s for s in ['code.inc','inputs.json','LICENSES.txt']]
PREFLIGHT=[C,BACKEND,JSON,LEX,SNAPSHOT,JS,'core/src/extension/package-check.h','core/src/extension/package-json.h','core/src/extension/package-snapshot.h','core/tests/test_package_check.c','core/tests/test_package_check_boundary.c',*GENERATED]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--case',action='append',default=[]);args=parser.parse_args()
 for name in PREFLIGHT:
  if MARK in (ROOT/name).read_text():print('residual package check mutation: '+name,file=sys.stderr);raise SystemExit(2)
 cases=[
  ('json-preflight',BACKEND,'if(error!=GT_PACKAGE_JSON_OK){','if(false && error!=GT_PACKAGE_JSON_OK){','native:duplicate'),
  ('exact-integer',BACKEND,'gt__package_json_check(json,raw[i],lengths[i],true)','gt__package_json_check(json,raw[i],lengths[i],false)','native:fractional'),
  ('catalog-sha256',BACKEND,'JS_NewString(c,member.sha256)','JS_NewString(c,"'+('a'*64)+'")','native:compatible'),
  ('catalog-size',BACKEND,'(double)member.bytes','(double)member.bytes+1','native:compatible'),
  ('catalog-path',BACKEND,'JS_NewString(c,member.path)','JS_NewString(c,"README")','native:compatible'),
  ('catalog-complete',BACKEND,'for(size_t i=0;i<count;i++){','for(size_t i=0;i+1<count;i++){','native:compatible'),
  ('heap-budget',BACKEND,'n+sizeof(package_js_allocation_t)>s->limit-s->used','(false && n+sizeof(package_js_allocation_t)>s->limit-s->used)','boundary:caught-oom'),
  ('caught-oom-sticky',BACKEND,'if(state.memory){check_report(report,GT_PACKAGE_CHECK_MEMORY,"ENGINE_MEMORY");goto done;}','if(false && state.memory){check_report(report,GT_PACKAGE_CHECK_MEMORY,"ENGINE_MEMORY");goto done;}','boundary:caught-oom'),
  ('js-interrupt',BACKEND,'return !check_budget(opaque);','(void)check_budget(opaque);return 0;','boundary:js-expired'),
  ('stop-not-ignored',BACKEND,'s->config->stop && atomic_load_explicit','false && s->config->stop && atomic_load_explicit','boundary:js-cancel'),
  ('clock-not-expiry',BACKEND,'s->clock_error=true;return false;','s->expired=true;return false;','boundary:js-clock'),
  ('deadline-boundary',BACKEND,'now>=s->deadline','now>s->deadline','native:expired'),
  ('cancel-at-commit',BACKEND,'if(report->status==GT_PACKAGE_CHECK_COMPATIBLE && !check_budget(&state))check_exception(&state,report);','/* omitted final budget barrier */','native:late-cancel'),
  ('clock-at-commit',BACKEND,'if(report->status==GT_PACKAGE_CHECK_COMPATIBLE && !check_budget(&state))check_exception(&state,report);','/* omitted final budget barrier */','native:late-clock'),
  ('deadline-at-commit',BACKEND,'if(report->status==GT_PACKAGE_CHECK_COMPATIBLE && !check_budget(&state))check_exception(&state,report);','/* omitted final budget barrier */','native:late-expired'),
  ('source-consumed',BACKEND,'checked->snapshot=*snapshot;*snapshot=NULL;*out=checked;','checked->snapshot=*snapshot;*out=checked;','native:compatible'),
  ('source-failure-retained',BACKEND,'return report->status;','if(report->status!=GT_PACKAGE_CHECK_COMPATIBLE)gt__package_snapshot_destroy(snapshot);return report->status;','native:hash'),
  ('package-digest',BACKEND,'memcpy(info->package_digest,metadata[1].sha256,65);','memcpy(info->package_digest,metadata[0].sha256,65);','native:compatible'),
  ('unknown-not-unsupported',BACKEND,'!strcmp(kind,"unknown")?GT_PACKAGE_CHECK_UNKNOWN','!strcmp(kind,"unknown")?GT_PACKAGE_CHECK_UNSUPPORTED','native:unknown'),
  ('required-service-copy',BACKEND,'if(ok)*count=n;','if(ok)*count=0;','native:compatible'),
  ('association-visible',BACKEND,'info->association_required=JS_ToBool(c,association)==1;','info->association_required=false;','fixture:registry-conflict'),
  ('failure-info-wiped',C,'if(out)memset(out,0,sizeof(*out));','(void)out;','native:hash'),
  ('destroy-error-visible',C,'gt__plugin_memory_free(&a,p,sizeof(*p));return status;}','gt__plugin_memory_free(&a,p,sizeof(*p));(void)status;return GT_PACKAGE_OK;}','boundary:destroy-error'),
  ('snapshot-view-hidden',SNAPSHOT,'if(bytes)*bytes=NULL;','(void)bytes;','native:compatible'),
  ('code-copy-wiped',BACKEND,'check_wipe_free(&state,code,GT_PACKAGE_VALIDATOR_BYTES+1);','if(code)state.allocator.release(state.allocator.context,code,GT_PACKAGE_VALIDATOR_BYTES+1);','native:compatible'),
  ('hash-required',JS,'actual.sha256 !== member.sha256','false','fixture:hash-mismatch'),
  ('index-complete',JS,'!excluded.has(path) && !declared.has(path)','false','fixture:unindexed-member'),
  ('permission-required',JS,"!permissionKeys.has(name + ':' + capability)",'false','fixture:missing-permission'),
  ('range-syntax',JS,'if (!semver.validRange(value))','if (false)','fixture:bad-range'),
  ('host-unknown',JS,"fail('HOST_UNKNOWN', 'unknown')","fail('HOST_UNKNOWN', 'unsupported')",'fixture:unknown-host'),
  ('variant-choice',JS,'if (variants.length !== 1)','if (!variants.length)','fixture:variant-choice'),
  ('protocol-intersection',JS,'host.protocols.includes(version)','true','fixture:protocol-unsupported'),
  ('explicit-denial',JS,'if (host.deniedCapabilities?.includes(capability))','if (false)','fixture:capability-denied'),
  ('digest-not-publisher',JS,'publisherAuthenticated: false','publisherAuthenticated: true','native:compatible'),
  ('compatibility-not-grant',JS,"authorization: 'not-granted'","authorization: 'granted'",'native:compatible'),
 ]
 known={c[0] for c in cases}
 if set(args.case)-known:parser.error('unknown case')
 if args.case:cases=[c for c in cases if c[0] in args.case]
 for label,name,old,*_ in cases:
  expected=2 if label=='json-preflight' else 1
  if (ROOT/name).read_text().count(old)!=expected:raise RuntimeError('missing/ambiguous anchor '+label)
 inputs=[p for folder in ['core','packages/plugin-sdk'] for p in (ROOT/folder).rglob('*') if p.is_file() and not any(x.startswith('build') for x in p.relative_to(ROOT/folder).parts)]
 inputs += [Path(__file__),ROOT/'scripts/generate-plugin-package-validator.mjs',ROOT/'scripts/check-plugin-package-validator.py',ROOT/'scripts/probes/plugin-package-native.mjs',*list((ROOT/'patches/quickjs-ng').rglob('*'))]
 inputs=[p for p in inputs if p.is_file()]
 for folder in ['plugin-package-tools','plugin-sdk-tools']:
  inputs += [ROOT/'scripts/fixtures'/folder/n for n in ['package.json','package-lock.json']]
 hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};work=Path(tempfile.mkdtemp(prefix='pkgcheck-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';build=work/'build';commands=[]
 env=dict(os.environ,TMPDIR=str(work),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',NPM_CONFIG_USERCONFIG=str(work/'empty.npmrc'));(work/'empty.npmrc').touch()
 def run(label,cmd,red=False,reject=False,budget=180):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-check; shift; gt_timeout "$@"'
  p=subprocess.run(['bash','-c',shell,'package-check',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion=red,configuration_rejection=reject))
  good=p.returncode==0
  if red:good=p.returncode==1 and re.search(r'^FAIL ',p.stderr,re.M) and not any(x in out for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer'])
  if reject:good=p.returncode==1 and ('Generated package validator' in out or 'Pinned QuickJS-NG patch' in out)
  if not good:raise RuntimeError(label+'\n'+out)
  return out
 try:
  shutil.copytree(ROOT/'core',source/'core',ignore=shutil.ignore_patterns('build','build-*'));shutil.copytree(ROOT/'packages/plugin-sdk',source/'packages/plugin-sdk');shutil.copytree(ROOT/'patches/quickjs-ng',source/'patches/quickjs-ng');(source/'vendor').mkdir();(source/'vendor/quickjs-ng').symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True);(source/'tmp').mkdir();(source/'scripts').mkdir()
  for name in ['generate-plugin-package-validator.mjs','check-plugin-package-validator.py','generate-plugin-runtime-sdk.mjs','check-plugin-runtime-sdk.py']:shutil.copyfile(ROOT/'scripts'/name,source/'scripts'/name)
  shutil.copytree(ROOT/'scripts/fixtures/plugin-package-tools',source/'scripts/fixtures/plugin-package-tools')
  shutil.copytree(ROOT/'scripts/fixtures/plugin-sdk-tools',source/'scripts/fixtures/plugin-sdk-tools')
  tools={}
  for kind,fixture in [('package','plugin-package-tools'),('sdk','plugin-sdk-tools')]:
   tools[kind]=work/(kind+'-tools');tools[kind].mkdir()
   for name in ['package.json','package-lock.json']:shutil.copyfile(ROOT/'scripts/fixtures'/fixture/name,tools[kind]/name)
   lock=json.loads((tools[kind]/'package-lock.json').read_text())
   for name,p in lock['packages'].items():
    if name:assert p['resolved'].startswith('https://registry.npmjs.org/') and p['integrity'].startswith('sha512-'),name
   cache='plugin-schema-npm-cache' if kind=='package' else 'plugin-sdk-npm-cache';run(kind+'-npm',['npm','ci','--prefix',tools[kind],'--ignore-scripts','--no-audit','--no-fund','--offline','--cache',ROOT/'tmp'/cache])
  sdk=work/'sdk';run('sdk',['node',tools['sdk']/'node_modules/typescript/bin/tsc','-p',source/'packages/plugin-sdk/tsconfig.json','--outDir',sdk])
  fixtures=work/'fixtures';fixtures.mkdir();run('fixtures',['node',ROOT/'scripts/probes/plugin-package-native.mjs',ROOT,tools['package'],fixtures]);vectors={v['name']:v for v in json.loads((fixtures/'vectors.json').read_text())['fixtures']}
  run('generated',['node',source/'scripts/generate-plugin-package-validator.mjs','--tools',tools['package'],'--check'])
  run('configure',['cmake','-S',source/'core','-B',build,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_PLUGIN_SDK_DIR='+str(sdk),'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
  binaries=['test_package_check','test_package_check_boundary','test_package_json','test_package_compiler','test_config'];objects=[('ghosterm_core','src/extension/'+n+'.c') for n in ['package-check','package-json','package-snapshot']]+[(n,'tests/'+n+'.c') for n in binaries[:-1]]
  def rebuild(label):
   for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
   for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
   out=run(label+'-build',['cmake','--build',build,'--target',*binaries,'-j','2'],budget=300)
   for owner,name in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in out,(label,name)
   for n in binaries:assert 'Linking C executable '+n in out,(label,n)
   assert 'Linking C static library libghosterm_core.a' in out
  def target(label,mode,red=False):
   kind,mode=mode.split(':',1)
   if kind=='fixture':
    v=vectors[mode];cmd=[build/'test_package_check','probe',v['directory'],v['hostFile'],v['registryFile'],v['status'],v['code']]
    if mode=='registry-conflict':cmd+=['association=true']
   else:cmd=[build/('test_package_check_boundary' if kind=='boundary' else 'test_package_check'),mode,source/'tmp']
   try:run(label,cmd,red=red,budget=90)
   finally:
    for p in (source/'tmp').iterdir():
     if p.is_dir() and not p.is_symlink():shutil.rmtree(p)
     else:p.unlink()
  def suite(label):
   discovered=json.loads(run(label+'-discovery',['ctest','--test-dir',build,'--show-only=json-v1']))['tests'];selected=[t for t in discovered if re.match(r'^plugin_package_(check|json|validator|compiler)',t['name'])];assert len(selected)==50,len(selected)
   out=run(label,['ctest','--test-dir',build,'-R','^plugin_package_(check|json|validator|compiler)','--output-on-failure']);assert '100% tests passed out of 50' in out
   for t in selected:assert re.search(r'Test\s+#\d+: '+re.escape(t['name'])+r'\s+\.+\s+Passed',out),t['name']
  rebuild('baseline');suite('baseline');run('baseline-control',[build/'test_config',source/'core/tests/fixtures'])
  for label,name,old,new,mode in cases:
   touched=[name,*GENERATED] if name==JS else [name];original={n:(source/n).read_bytes() for n in touched}
   try:
    (source/name).write_text(original[name].decode().replace(old,new)+'\n/* '+MARK+' */\n')
    if name==JS:run(label+'-generate',['node',source/'scripts/generate-plugin-package-validator.mjs','--tools',tools['package']])
    rebuild(label);target(label+'-target',mode,True);run(label+'-control',[build/'test_config',source/'core/tests/fixtures'])
   finally:
    for n,data in original.items():(source/n).write_bytes(data);assert sha(source/n)==hashlib.sha256(data).hexdigest()
   rebuild(label+'-restored');target(label+'-restored',mode);print(label+': actual C/generated JS/archive/executable assertion red / config green / rebuilt restore / SHA256',flush=True)
  # Configuration guards are rejection evidence, not compiled assertion groups.
  guards=['packages/plugin-sdk/schema/manifest-v1.candidate.json',JS,GENERATED[0],GENERATED[2],'scripts/generate-plugin-package-validator.mjs','scripts/fixtures/plugin-package-tools/package-lock.json']
  for index,name in enumerate(guards):
   path=source/name;original=path.read_bytes()
   try:
    path.write_bytes(original+b'\n ');run('guard-'+str(index),['cmake','-S',source/'core','-B',work/('guard-'+str(index)),'-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_BUILD_TESTS=OFF'],reject=True)
   finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
  path=source/GENERATED[1];original=path.read_bytes()
  try:
   data=json.loads(original);data['inputs'].pop(JS);path.write_text(json.dumps(data));run('guard-input-set',['cmake','-S',source/'core','-B',work/'guard-input-set','-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_BUILD_TESTS=OFF'],reject=True)
  finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
  patch_guards=0
  patch_paths=sorted((source/'patches/quickjs-ng/0.18.0').glob('*.patch'))
  assert len(patch_paths)==8,len(patch_paths)
  for path in patch_paths:
   original=path.read_bytes()
   for kind in ['missing','changed']:
    try:
     if kind=='missing':path.unlink()
     else:path.write_bytes(original+b'\n ')
     run('guard-'+path.stem+'-'+kind,['cmake','-S',source/'core','-B',work/('guard-'+path.stem+'-'+kind),'-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_BUILD_TESTS=OFF'],reject=True)
    finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
    patch_guards+=1
  assert patch_guards==16,patch_guards
  rebuild('final');suite('final');run('final-generated',['node',source/'scripts/generate-plugin-package-validator.mjs','--tools',tools['package'],'--check'])
  for name,h in hashes.items():assert sha(ROOT/name)==h,name
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],configuration_guards=7+patch_guards,native_cases=50,inputs_sha256=hashes,commands=commands,sanitizers=['address','undefined'],leak_sanitizer=False,scope='native metadata checks; no runtime/module activation, App, backend, gateway or release'),indent=2)+'\n');shutil.rmtree(source);shutil.rmtree(build);shutil.rmtree(fixtures);print('PACKAGE CHECK MUTATIONS PASS; logs: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,_frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
