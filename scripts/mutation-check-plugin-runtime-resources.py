#!/usr/bin/env python3
"""Isolated actual C/JS resource worker mutations; no App enable or gateway.
Normal C assertion red, same archive configuration control, rebuilt restore.
"""
import argparse, hashlib, json, os, re, shutil, signal, subprocess, sys, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
MARK='GT_RUNTIME_RESOURCES_MUTATION'
OWNER='core/src/extension/runtime-resource.c'
ADAPTER='core/src/extension/runtime-resource-js.inc'
FACADE='core/src/extension/runtime-api.c'
SDK='packages/plugin-sdk/portable/resources.ts'
OWNER_MODES='invalid parent ungranted symlink capacity expired clock cancel close-sent roundtrip large empty read-expired read-clock revoked fairness'.split()
JS_MODES='roundtrip large empty read read-large read-empty leak-ref cancel ungranted close-live invalid-root adapter-unconfirmed adapter-truncated adapter-read-error'.split()
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser();parser.add_argument('--case',action='append',default=[]);args=parser.parse_args()
 for name in [OWNER,ADAPTER,FACADE,SDK]:
  if MARK in (ROOT/name).read_text():print('residual runtime resource mutation: '+name,file=sys.stderr);raise SystemExit(2)
 cases=[
  ('root-id-copy',OWNER,'        strcpy(r->roots[i].id, roots[i].id);\n','        r->roots[i].id[0] = 0;\n','owner','invalid'),
  ('root-cloexec',OWNER,'        int fd = fcntl(roots[i].directory_fd, F_DUPFD_CLOEXEC, 3);\n','        int fd = dup(roots[i].directory_fd);\n','owner','invalid'),
  ('root-quota',OWNER,'    if (!limit || limit > scope->limit) {\n','    if (!limit || (false && limit > scope->limit)) {\n','owner','invalid'),
  ('unknown-root-denied',OWNER,'    if (!scope) {\n        return GT_RUNTIME_RESOURCE_DENIED;\n','    if (!scope) {\n        return GT_RUNTIME_RESOURCE_LIMIT;\n','owner','invalid'),
  ('source-deadline',OWNER,'                (!valid || now >= r->sources[i].deadline)) {\n','                (!valid || now > r->sources[i].deadline)) {\n','owner','expired'),
  ('clock-error-not-expiry',OWNER,'                r->sources[i].expired = valid;\n','                r->sources[i].expired = true;\n','owner','clock'),
  ('source-cancel',OWNER,'    gt_ext_source_cancel(s->source);\n','    (void)s->source;\n','owner','cancel'),
  ('source-round-robin',OWNER,'            (void)gt_ext_source_send(r->sources[i].source);\n        }\n        break;\n','            (void)gt_ext_source_send(r->sources[i].source); /* no break */\n        }\n','owner','fairness'),
  ('source-forget-owner',OWNER,'    gt_runtime_resource_status_t status = source_status(gt_ext_source_destroy(&s->source));\n    if (status == GT_RUNTIME_RESOURCE_OK) {\n','    gt_runtime_resource_status_t status = source_status(gt_ext_source_destroy(&s->source));\n    if (false && status == GT_RUNTIME_RESOURCE_OK) {\n','owner','roundtrip'),
  ('read-quota',OWNER,'        r->read_budget -= *read;\n','        r->read_budget -= 0;\n','owner','large'),
  ('read-sequential',OWNER,'    if (!slot->reading || at != slot->read_at) {\n','    if (!slot->reading || (false && at != slot->read_at)) {\n','owner','roundtrip'),
  ('read-deadline',OWNER,'    if (now >= slot->deadline) {\n','    if (now > slot->deadline) {\n','owner','read-expired'),
  ('read-error-sticky',OWNER,'    if (slot->read_error != GT_RUNTIME_RESOURCE_OK) {\n','    if (false && slot->read_error != GT_RUNTIME_RESOURCE_OK) {\n','owner','read-clock'),
  ('read-revoke',OWNER,'  * plugin reads. Cleanup release remains available after stop. */\n    gt_runtime_resource_status_t access = resource_access(r);\n    if (access != GT_RUNTIME_RESOURCE_OK) {\n','  * plugin reads. Cleanup release remains available after stop. */\n    gt_runtime_resource_status_t access = resource_access(r);\n    if (false && access != GT_RUNTIME_RESOURCE_OK) {\n','owner','revoked'),
  ('snapshot-release',OWNER,'        resource_status(gt_ext_resource_store_release(r->store, &r->lease, &slot->snapshot));\n','        (false\n             ? resource_status(gt_ext_resource_store_release(r->store, &r->lease, &slot->snapshot))\n             : GT_RUNTIME_RESOURCE_OK);\n','owner','roundtrip'),
  ('root-count-facade',FACADE,'                             .root_count = config->root_count,\n','                             .root_count = 0,\n','js','roundtrip'),
  ('root-fd-facade',FACADE,'        roots[i] = (gt_runtime_root_t){\n            config->roots[i].id, config->roots[i].directory_fd, config->roots[i].byte_limit};\n','        roots[i] = (gt_runtime_root_t){config->roots[i].id, -1, config->roots[i].byte_limit};\n','js','roundtrip'),
  ('native-receipt',ADAPTER,'            runtime_set(c, out, "confirmed", JS_NewBool(c, p.source.confirmed)) < 0 ||\n','            runtime_set(c, out, "confirmed", JS_NewBool(c, false)) < 0 ||\n','js','roundtrip'),
  ('native-read-bytes',ADAPTER,'            runtime_set(c, out, "bytes", JS_NewUint8ArrayCopy(c, bytes, read)) < 0) {\n','            runtime_set(c, out, "bytes", JS_NewUint8ArrayCopy(c, bytes, 0)) < 0) {\n','js','read'),
  ('sdk-copied-reference',SDK,'const local = this.#refs.get(ref); if (!local) return { status: "invalid" };','const local = this.#refs.values().next().value; if (!local) return { status: "invalid" };','js','read'),
  ('sdk-explicit-release',SDK,'const status = this.#port.resourceRelease(local.handle);','const status = "ok" as ResourceStatus;','js','read'),
  ('sdk-unconfirmed-receipt',SDK,'!result.confirmed || result.transferState !== "succeeded"','result.transferState !== "succeeded"','js','adapter-unconfirmed'),
  ('sdk-truncated-eof',SDK,'(!result.bytes.length && local.at !== BigInt(size.bytes))','false','js','adapter-truncated'),
  ('sdk-read-error-sticky',SDK,'if (local.failure) return Promise.resolve(local.failure);','if (local.failure) return Promise.resolve<Outcome<ReadChunk>>({kind: "success", value: {bytes: new Uint8Array(), done: true}});','js','adapter-read-error'),
  ('cleanup-io-before-wait',OWNER,'                if (result == GT_RUNTIME_RESOURCE_OK || result == GT_RUNTIME_RESOURCE_WAIT) {\n                    result = s;\n                }\n','                result = s;\n','close',None),
  ('missing-pin-release',OWNER,'    if (status == GT_RUNTIME_RESOURCE_NOT_FOUND) {\n','    if (false && status == GT_RUNTIME_RESOURCE_NOT_FOUND) {\n','close',None),
 ]
 known={c[0] for c in cases}
 if set(args.case)-known:parser.error('unknown case')
 if args.case:cases=[c for c in cases if c[0] in args.case]
 for label,name,old,*_ in cases:
  if (ROOT/name).read_text().count(old)!=1:raise RuntimeError('missing/ambiguous anchor: '+label)
 inputs=[p for folder in ['core','packages/plugin-sdk'] for p in (ROOT/folder).rglob('*') if p.is_file() and not any(x.startswith('build') for x in p.relative_to(ROOT/folder).parts)]
 inputs+=[Path(__file__),ROOT/'scripts/fixtures/plugin-sdk-tools/package.json',ROOT/'scripts/fixtures/plugin-sdk-tools/package-lock.json']
 hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};work=Path(tempfile.mkdtemp(prefix='plugin-runtime-resources-mutations-',dir=ROOT/'tmp'));logs=work/'logs';logs.mkdir();source=work/'source';commands=[]
 env=dict(os.environ,TMPDIR=str(work),DEVELOPER_DIR='/Applications/Xcode.app/Contents/Developer',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 def run(label,cmd,red=False,budget=180):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require runtime-resources; shift; gt_timeout "$@"'
  p=subprocess.run(['bash','-c',shell,'runtime-resources',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode,assertion=red))
  valid=p.returncode==0
  if red:valid=p.returncode==1 and p.stderr.startswith('FAIL ') and not any(x in out for x in ['AddressSanitizer','runtime error:','UndefinedBehaviorSanitizer'])
  if not valid:raise RuntimeError(label+'\n'+out)
  return out
 try:
  shutil.copytree(ROOT/'core',source/'core',ignore=shutil.ignore_patterns('build','build-*'));shutil.copytree(ROOT/'packages/plugin-sdk',source/'packages/plugin-sdk');(source/'vendor').mkdir();(source/'vendor/quickjs-ng').symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True)
  tools=work/'tools';tools.mkdir()
  for name in ['package.json','package-lock.json']:shutil.copyfile(ROOT/'scripts/fixtures/plugin-sdk-tools'/name,tools/name)
  for name,e in json.loads((tools/'package-lock.json').read_text())['packages'].items():
   if name and (not e['resolved'].startswith('https://registry.npmjs.org/') or not e['integrity'].startswith('sha512-')):raise RuntimeError('unpinned tool')
  npmrc=work/'empty.npmrc';npmrc.touch();env['NPM_CONFIG_USERCONFIG']=str(npmrc)
  run('tools',['npm','ci','--prefix',tools,'--ignore-scripts','--no-audit','--no-fund','--offline','--cache',ROOT/'tmp/plugin-sdk-npm-cache']);sdk=work/'sdk';core=source/'core';build=work/'build'
  def compile_sdk(label):run(label+'-typescript',['node',tools/'node_modules/typescript/bin/tsc','-p',source/'packages/plugin-sdk/tsconfig.json','--outDir',sdk])
  compile_sdk('baseline');run('configure',['cmake','-S',core,'-B',build,'-DGHOSTERM_WITH_PLUGIN_JS=ON','-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_PLUGIN_SDK_DIR='+str(sdk),'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
  binaries=['test_runtime_resources','test_runtime_resource_js','test_runtime_resource_oom','test_runtime_resource_close','test_config']
  objects=[('ghosterm_core','src/extension/'+n+'.c') for n in ['runtime','runtime-resource','runtime-api']]+[(n,'tests/'+n+'.c') for n in binaries[:-1]]
  def rebuild(label):
   for owner,name in objects:(build/'CMakeFiles'/(owner+'.dir')/(name+'.o')).unlink(missing_ok=True)
   for name in ['libghosterm_core.a',*binaries]:(build/name).unlink(missing_ok=True)
   out=run(label+'-build',['cmake','--build',build,'--target',*binaries,'-j','2'],budget=300)
   for owner,name in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+name+'.o' in out,(label,name)
   for name in binaries:assert 'Linking C executable '+name in out,(label,name)
   assert 'Linking C static library libghosterm_core.a' in out
  def suite(label):
   expected={'plugin_resource_owner_'+m for m in OWNER_MODES}|{'plugin_resource_js_'+m for m in JS_MODES}|{'plugin_resource_js_snapshot_oom','plugin_resource_owner_close_error'}
   discovered=json.loads(run(label+'-discovery',['ctest','--test-dir',build,'--show-only=json-v1']));assert {t['name'] for t in discovered['tests'] if t['name'].startswith(('plugin_resource_owner_','plugin_resource_js_'))}==expected
   out=run(label,['ctest','--test-dir',build,'-R','^plugin_resource_(js|owner)_','--output-on-failure'],budget=120)
   assert '100% tests passed out of 32' in out
   for name in expected:assert re.search(r'Test\s+#\d+: '+re.escape(name)+r'\s+\.+\s+Passed',out),name
  rebuild('baseline');suite('baseline');run('baseline-control',[build/'test_config',core/'tests/fixtures'])
  for label,name,old,new,kind,mode in cases:
   path=source/name;original=path.read_bytes();binary=build/('test_runtime_resources' if kind=='owner' else 'test_runtime_resource_close' if kind=='close' else 'test_runtime_resource_js');location=sdk if kind=='js' else build;target=[binary,location] if mode is None else [binary,mode,location]
   try:
    path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n')
    compile_sdk(label);rebuild(label);run(label+'-target',target,red=True,budget=30);run(label+'-control',[build/'test_config',core/'tests/fixtures'])
   finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
   compile_sdk(label+'-restored');rebuild(label+'-restored');run(label+'-restored',target,budget=30);print(label+': actual C/TS/archive/executable assertion red / same archive config green / restored / SHA256',flush=True)
  suite('final')
  for name,h in hashes.items():assert sha(ROOT/name)==h,name
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],inputs_sha256=hashes,commands=commands,owner_cases=len(OWNER_MODES)+1,js_cases=len(JS_MODES)+1,c_sanitizers=['address','undefined'],leak_sanitizer=False,sdk_sha256={p.name:sha(p) for p in sdk.glob('*.js')},scope='native runtime resource owner and actual SDK/QuickJS/Core file+snapshot transport; no App activation, gateway, physical device, WAN or release'),indent=2)+'\n');shutil.rmtree(source);print('RUNTIME RESOURCE MUTATIONS PASS; logs: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise
def stop(number,_frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
