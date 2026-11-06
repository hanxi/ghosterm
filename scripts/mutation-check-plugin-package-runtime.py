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
 ('stop-before-load',ENGINE,'    }\n    if (atomic_load_explicit(&r->stopped, memory_order_relaxed)) {\n','    }\n    if (false && atomic_load_explicit(&r->stopped, memory_order_relaxed)) {\n','stop-created'),
 ('destroy-before-close',ENGINE,'    if (!out || !*out || (*out)->info.phase != GT_RUNTIME_CLOSED) {\n','    if (!out || !*out || false) {\n','close-created'),
 ('identity',SESSION,'    return !strcmp(s->config.plugin_id, id) && !strcmp(s->config.plugin_version, version) &&\n','    return true && !strcmp(s->config.plugin_version, version) &&\n','id'),
 ('version',SESSION,'    return !strcmp(s->config.plugin_id, id) && !strcmp(s->config.plugin_version, version) &&\n','    return !strcmp(s->config.plugin_id, id) && true &&\n','version'),
 ('digest',SESSION,'    return !strcmp(s->config.plugin_id, id) && !strcmp(s->config.plugin_version, version) &&\n           !strcmp(role == GT_BRIDGE_HOST ? s->config.client_digest : s->config.executor_digest,\n                   digest) &&\n','    return !strcmp(s->config.plugin_id, id) && !strcmp(s->config.plugin_version, version) && true &&\n','digest'),
 ('sdk',PACKAGE,'    if (strcmp(info.runtime, "portable-js") ||\n        strcmp(info.sdk_version, GT_PLUGIN_SDK_API_VERSION)) {\n','    if (strcmp(info.runtime, "portable-js") || false) {\n','sdk-version'),
 ('association',PACKAGE,'info.association_required && !associated','false && info.association_required && !associated','association'),
 ('permission',PACKAGE,'        if (gt__bridge_session_access(session, lease, name) != GT_BRIDGE_ACCESS_ALLOWED) {\n','        if (false) {\n','ungranted'),
 ('native-import',ENGINE,'    if (!strcmp(name, "host:/native") && !strcmp(base, "host:/bootstrap.js")) {\n','    if (!strcmp(name, "host:/native")) {\n','native'),
 ('source-size',ENGINE,'    if (source.size > 128 * 1024 ||\n        (!known && (r->count >= 64 || source.size > 1024 * 1024 - r->info.module_bytes))) {\n','    if (false || (!known && (r->count >= 64 || source.size > 1024 * 1024 - r->info.module_bytes))) {\n','source-limit'),
 ('source-total',ENGINE,'    if (source.size > 128 * 1024 ||\n        (!known && (r->count >= 64 || source.size > 1024 * 1024 - r->info.module_bytes))) {\n','    if (source.size > 128 * 1024 || (!known && (r->count >= 64 || false))) {\n','source-total'),
 ('module-count',ENGINE,'        (!known && (r->count >= 64 || source.size > 1024 * 1024 - r->info.module_bytes))) {\n','        (!known && (r->count >= 63 || source.size > 1024 * 1024 - r->info.module_bytes))) {\n','module-boundary'),
 ('utf8',ENGINE,'    if (used != source.size || !gt__runtime_source_utf8(copy, used) ||\n        !gt__sha256_update(&hash, copy, used) || !gt__sha256_final(&hash, digest) ||\n        strcmp(digest, source.sha256)) {\n','    if (used != source.size || false || !gt__sha256_update(&hash, copy, used) ||\n        !gt__sha256_final(&hash, digest) || strcmp(digest, source.sha256)) {\n','utf8'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--public',action='store_true');parser.add_argument('--registry',action='store_true');parser.add_argument('--registry-api',action='store_true');parser.add_argument('--capabilities',action='store_true');parser.add_argument('--invocation',action='store_true');args=parser.parse_args();assert sum([args.public,args.registry,args.registry_api,args.capabilities,args.invocation])<=1
 cases=CASES
 executable='test_package_runtime';pattern='^plugin_package_runtime_';objects=['runtime','package-runtime','session']
 if args.public:
  api='core/src/extension/package-api.c';runtime='core/src/extension/runtime-api.c'
  cases=[
   ('capture-cancel',api,'    if (p->info.phase != GT_EXT_PACKAGE_CAPTURING) {\n        return p->info.phase;\n    }\n    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) {\n','    if (p->info.phase != GT_EXT_PACKAGE_CAPTURING) {\n        return p->info.phase;\n    }\n    atomic_store(&p->stopped, false);\n    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) {\n','capture-stop'),
   ('check-cancel',api,'    if (p->info.phase != GT_EXT_PACKAGE_CAPTURED) {\n        return p->info.phase;\n    }\n    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) {\n','    if (p->info.phase != GT_EXT_PACKAGE_CAPTURED) {\n        return p->info.phase;\n    }\n    atomic_store(&p->stopped, false);\n    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) {\n','check-stop'),
   ('unknown-preserved',api,'            error = GT_EXT_PACKAGE_UNKNOWN;\n','            error = GT_EXT_PACKAGE_UNSUPPORTED;\n','unknown'),
   ('metadata-zero',api,'    if (out) {\n        memset(out, 0, sizeof(*out));\n    }\n    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED) {\n','    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED) {\n','roundtrip'),
   ('digest-copy',api,'COPY(package_digest);','COPY(manifest_digest);','roundtrip'),
   ('stopped-adoption',runtime,'    if (atomic_load_explicit(&package->stopped, memory_order_relaxed)) {\n','    if (false && atomic_load_explicit(&package->stopped, memory_order_relaxed)) {\n','load-stop'),
  ]
  executable='test_ext_package';pattern='^ext_package_';objects=['package-api','runtime-api','runtime','package-runtime','session']
 if args.registry:
  registry='core/src/extension/registry.c'
  cases=[
   ('command-separator', 'core/src/extension/package-check-js.inc', "                out->path[used++] = ' ';\n", "                out->path[used++] = '-';\n", None),
   ('command-title', 'core/src/extension/package-check-js.inc', '    ok = ok && check_string(c, command, "title", out->title, sizeof(out->title)) &&\n', '    ok = ok && check_string(c, command, "title", out->title, sizeof(out->title) - 1) &&\n', None),
   ('command-count', 'core/src/extension/package-check-js.inc', '        bool valid = JS_IsArray(commands) && !JS_ToUint32(c, &n, length) && n >= 1 && n <= 64;\n', '        bool valid = JS_IsArray(commands) && !JS_ToUint32(c, &n, length) && n >= 1 && n < 64;\n', None),
   ('command-views', 'core/src/extension/package-check-js.inc', '        out->view_count = count;\n', '        out->view_count = 0;\n', None),
   ('source-unknown',registry,'        return GT_REGISTRY_UNKNOWN;\n','        return GT_REGISTRY_DENIED;\n',None),
   ('source-denied',registry,'        return GT_REGISTRY_DENIED;\n','        return GT_REGISTRY_UNKNOWN;\n',None),
   ('conflict',registry,'    if (!id && overlaps > 1) {\n','    if (false && !id && overlaps > 1) {\n',None),
   ('replace-digest',registry,'        if (strcmp(entry->digest, expected)) {\n','        if (false && strcmp(entry->digest, expected)) {\n',None),
   ('remove-digest',registry,'            if (strcmp(e->digest, expected)) {\n','            if (false && strcmp(e->digest, expected)) {\n',None),
   ('selection-digest',registry,'            if (strcmp(digest, e->digest)) {\n','            if (false && strcmp(digest, e->digest)) {\n',None),
   ('prefix-token',registry,"    return !memcmp(a, b, n) && (x == y || (x < y ? b[n] : a[n]) == ' ');\n",'    return !memcmp(a, b, n) && true;\n',None),
   ('side',registry,'        if (!e->package || e->executor != executor) {\n','        if (!e->package || (false && e->executor != executor)) {\n',None),
  ]
  executable='test_plugin_registry';pattern='^plugin_registry$';objects=['registry','package-check']
 if args.registry_api:
  api='core/src/extension/registry-api.c'
  cases=[
   ('api-source-unknown',api,'            return GT_EXT_REGISTRY_UNKNOWN;\n','            return GT_EXT_REGISTRY_DENIED;\n','registry'),
   ('api-source-denied',api,'            return GT_EXT_REGISTRY_DENIED;\n','            return GT_EXT_REGISTRY_UNKNOWN;\n','registry'),
   ('api-stopped-put',api,'    if (atomic_load_explicit(&p->stopped, memory_order_relaxed)) {\n','    if (false && atomic_load_explicit(&p->stopped, memory_order_relaxed)) {\n','registry'),
   ('api-independent-stop',api,'    atomic_init(&p->stopped, false);\n','    atomic_init(&p->stopped, true);\n','registry'),
   ('api-selected-stats',api,'    p->info.members = count;\n','    p->info.members = 0;\n','registry'),
   ('api-command-schema',api,'    memcpy(out->args_schema, c->args_schema, sizeof(c->args_schema));\n','    memset(out->args_schema, 0, sizeof(out->args_schema));\n','registry'),
   ('api-command-zero',api,'    if (out) {\n        memset(out, 0, sizeof(*out));\n    }\n    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED) {\n','    if (!p || !out || p->info.phase != GT_EXT_PACKAGE_CHECKED) {\n','registry'),
  ]
  executable='test_ext_package';pattern='^ext_package_';objects=['registry-api','package-check','package-api','registry']
 if args.capabilities:
  cases=[
   ('access-allowed',ENGINE,'                return JS_NewString(c, "allowed");\n','                return JS_NewString(c, "denied");\n','capabilities'),
   ('access-unsupported',ENGINE,'                return JS_NewString(c, "unsupported");\n','                return JS_NewString(c, "unknown");\n','capabilities'),
   ('access-revoked',ENGINE,'                return JS_NewString(c, "revoked");\n','                return JS_NewString(c, "allowed");\n','capabilities'),
   ('access-input',ENGINE,'        if (!size || size > 128 || memchr(name, 0, size)) {\n','        if (false) {\n','capabilities'),
   ('access-frozen',ENGINE,'capabilities:Object.freeze({query(name){return port.access(name)}})','capabilities:({query(name){return port.access(name)}})','capabilities'),
   ('dispose-error',ENGINE,'resources?.dispose();disposable?.dispose()','resources?.dispose();try{disposable?.dispose()}catch{}','dispose-throw'),
  ]
  executable='test_runtime';pattern='^(ext_)?runtime_(capabilities|dispose-throw)$';objects=['runtime']
 if args.invocation:
  cases=[
   ('invocation-command',ENGINE,'    if (!token) {\n','    if (false && !token) {\n','invocation'),
   ('invocation-utf8',ENGINE,'        if (sizes[i] > 4096 || sizes[i] + 1 > 16384 - total ||\n            !gt__runtime_source_utf8(input->argv[i], sizes[i])) {\n','        if (sizes[i] > 4096 || sizes[i] + 1 > 16384 - total || false) {\n','invocation'),
   ('invocation-argument-limit',ENGINE,'        if (sizes[i] > 4096 || sizes[i] + 1 > 16384 - total ||\n','        if (false || sizes[i] + 1 > 16384 - total ||\n','invocation'),
   ('invocation-total-limit',ENGINE,'        if (sizes[i] > 4096 || sizes[i] + 1 > 16384 - total ||\n            !gt__runtime_source_utf8(input->argv[i], sizes[i])) {\n','        if (sizes[i] > 4096 || false || !gt__runtime_source_utf8(input->argv[i], sizes[i])) {\n','invocation'),
   ('invocation-copy',ENGINE,'        memcpy(r->invocation + at, input->argv[i], sizes[i] + 1);\n','        memset(r->invocation + at, 0, sizes[i] + 1);\n','invocation'),
   ('invocation-freeze',ENGINE,'Object.freeze(invocation.arguments);','void(invocation.arguments);','invocation'),
   ('invocation-declared',ENGINE,'        if (!found) {\n','        if (false && !found) {\n','package:registry'),
  ]
  executable='test_runtime';pattern='^(ext_)?runtime_invocation$|^ext_package_registry$';objects=['runtime','runtime-api']
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
   out=run(label+'-build',['cmake','--build',build,'--target',executable,*(['test_ext_runtime'] if args.capabilities or args.invocation else []),*(['test_ext_package'] if args.invocation else []),'test_config','-j','4'])
   for name in objects:assert 'src/extension/'+name+'.c.o' in out
   assert 'Linking C static library' in out and 'Linking C executable '+executable in out
  def target(label,mode,red=False):
   if mode and mode.startswith('package:'):run(label,[build/'test_ext_package',mode.split(':',1)[1],source/'tmp'],red)
   else:run(label,[build/executable,*([mode] if mode else []),sdk if args.capabilities or args.invocation else source/'tmp'],red)
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
