#!/usr/bin/env python3
"""Isolated Core memory mutants: compiled assertion red, same-library config
green, rebuilt restore green, SHA256 unchanged. Requires a compiled portable SDK.
No App enable, network, RSS claim, OS sandbox or background-session restoration.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
MARK='GT_PLUGIN_MEMORY_MUTATION'
MEM='core/src/extension/memory.c'
LEX='core/src/base/json-reader.inc'
JSON='core/src/extension/package-json.c'
RT='core/src/extension/runtime-quickjs.inc'
CASES=[
 ('total-limit',MEM,'    bool ok = n <= memory.limit - memory.used;\n','    bool ok = true;\n','test_plugin_memory','budget'),
 ('failure-rollback',MEM,'    void *p = a ? a->allocate(a->context, n) : malloc(n);\n    if (!p) {\n        uncharge(n);\n    }\n','    void *p = a ? a->allocate(a->context, n)\n                : malloc(n); /* reservation retained after failed allocator */\n','test_plugin_memory','budget'),
 ('release-after-callback',MEM,'    gt__secure_erase(p, n);\n    if (a) {\n        a->release(a->context, p, n);\n    } else {\n        free(p);\n    }\n    uncharge(n);\n','    uncharge(n);\n    gt__secure_erase(p, n);\n    if (a) {\n        a->release(a->context, p, n);\n    } else {\n        free(p);\n    }\n','test_plugin_memory','budget'),
 ('release-wipe',MEM,'        return;\n    }\n    gt__secure_erase(p, n);\n','        return; /* wipe omitted */\n    }\n','test_plugin_memory','budget'),
 ('live-limit-change',MEM,'    bool ok = limit && !memory.used;\n','    bool ok = limit;\n','test_plugin_memory','budget'),
 ('queue-account','core/src/extension/frame.c','    d->bytes = gt__plugin_memory_alloc(&a, limit);\n','    d->bytes = a.allocate(a.context, limit);\n','test_plugin_memory','queue'),
 ('json-demand',LEX,'    if (!json_strings(d, d->strings_size + 1)) {\n','    if (!json_strings(d, GT_JSON_BYTES)) {\n','test_package_json','small-memory'),
 ('json-buffer-release',JSON,'    json_buffers_free(d);\n    return result;\n','    return result;\n','test_package_json','growth'),
 ('json-rehash',LEX,'                keys[slot] = d->keys[i];\n','                keys[slot] = 0;\n','test_package_json','growth-duplicate'),
 ('json-oom-kind',LEX,'    if (!p) {\n        return fail(d, GT_JSON_MEMORY);\n','    if (!p) {\n        return fail(d, GT_JSON_LIMIT);\n','test_package_json','global-memory'),
 ('runtime-source-account',RT,'        copy->source = gt__plugin_memory_alloc(NULL, m->size + 1);\n','        copy->source = malloc(m->size + 1);\n','test_runtime','roundtrip'),
 ('runtime-heap-account',RT,'    runtime_allocation_t *p = gt__plugin_memory_alloc(NULL, bytes);\n','    runtime_allocation_t *p = malloc(bytes);\n','test_runtime','global-memory'),
 ('runtime-local-limit',RT,'    r->heap_limit = memory;\n','    r->heap_limit = 128 * 1024 * 1024;\n','test_runtime','caught-memory'),
 ('runtime-oom-classification',RT,'                 : r->memory_failed                                      ? GT_RUNTIME_MEMORY\n                                                                         : GT_RUNTIME_SCRIPT);\n','                                                                         : GT_RUNTIME_SCRIPT);\n','test_runtime','caught-memory'),
]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--sdk',type=Path,required=True)
 parser.add_argument('--case',action='append',default=[])
 args=parser.parse_args();sdk=args.sdk.resolve()
 for name in ['index.js','codec.js','session.js','wire-contract.js','resources.js']:
  if not (sdk/name).is_file():parser.error('missing compiled SDK: '+name)
 if set(args.case)-{c[0] for c in CASES}:parser.error('unknown case')
 cases=[c for c in CASES if not args.case or c[0] in args.case]
 inputs=[p for folder in ['core','packages/plugin-sdk','patches/quickjs-ng'] for p in (ROOT/folder).rglob('*')
         if p.is_file() and not any(x.startswith('build') for x in p.relative_to(ROOT/folder).parts)]
 inputs += [p for p in (ROOT/'scripts').rglob('*') if p.is_file() and '__pycache__' not in p.parts]
 for name in {c[1] for c in CASES}:
  if MARK in (ROOT/name).read_text():raise RuntimeError('residual mutation: '+name)
 for label,name,old,*_ in cases:
  if (ROOT/name).read_text().count(old)!=1:raise RuntimeError('ambiguous anchor: '+label)
 hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs};sdk_hashes={p.name:sha(p) for p in sdk.glob('*.js')}
 work=Path(tempfile.mkdtemp(prefix='plugin-memory-',dir=ROOT/'tmp'));source=work/'source';build=work/'build';logs=work/'logs';logs.mkdir()
 env=dict(os.environ,TMPDIR=str(work),ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
 commands=[]
 def run(label,cmd,red=False,budget=120):
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require plugin-memory; shift; gt_timeout "$@"'
  p=subprocess.run(['bash','-c',shell,'plugin-memory',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10)
  output=p.stdout+p.stderr;(logs/(label+'.log')).write_text(output)
  commands.append(dict(label=label,returncode=p.returncode,assertion_red=red))
  ok=p.returncode==0
  if red:ok=p.returncode==1 and re.search(r'^FAIL ',p.stderr,re.M) and not any(x in output for x in ['AddressSanitizer','UndefinedBehaviorSanitizer','runtime error:'])
  if not ok:raise RuntimeError(label+'\n'+output)
  return output
 try:
  for folder in ['core','packages/plugin-sdk','patches/quickjs-ng','scripts']:
   shutil.copytree(ROOT/folder,source/folder,ignore=shutil.ignore_patterns('build','build-*','__pycache__'))
  (source/'vendor').mkdir();(source/'vendor/quickjs-ng').symlink_to(ROOT/'vendor/quickjs-ng',target_is_directory=True);(source/'tmp').mkdir()
  run('configure',['cmake','-S',source/'core','-B',build,'-DGHOSTERM_WITH_LIBSSH2=OFF','-DGHOSTERM_WITH_PLUGIN_JS=ON',
    '-DGHOSTERM_PLUGIN_SDK_DIR='+str(sdk),'-DCMAKE_BUILD_TYPE=RelWithDebInfo','-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'])
  targets=['test_plugin_memory','test_package_json','test_runtime','test_ext_runtime','test_config']
  run('baseline-build',['cmake','--build',build,'--target',*targets,'-j','4'],budget=300)
  pattern=r'^(plugin_memory_|plugin_package_json_|runtime_|ext_runtime_)'
  # Ownership/boundary targets are covered by the ordinary enabled full build.
  # This isolated runner discovers only tests backed by the built executables.
  def suite(label):
   tests=json.loads(run(label+'-discover',['ctest','--test-dir',build,'--show-only=json-v1']))['tests']
   excluded={'runtime_boundary_outbound','ext_runtime_boundary_outbound',
             *('ext_runtime_'+n for n in ['outgoing-owner','taken-owner','untaken-peer','resource-owner'])}
   selected=[t for t in tests if re.match(pattern,t['name']) and t['name'] not in excluded]
   assert len(selected)==91,len(selected)
   for t in selected:assert 'command' in t and Path(t['command'][0]).name in targets,t['name']
   names='^('+'|'.join(re.escape(t['name']) for t in selected)+')$'
   output=run(label,['ctest','--test-dir',build,'-R',names,'--output-on-failure'],budget=180)
   assert f'100% tests passed out of {len(selected)}' in output
   return len(selected)
  total=suite('baseline')
  def rebuild(label,name,binary):
   sources=['src/extension/runtime.c'] if name==RT else ['src/extension/package-json.c','src/extension/wire.c'] if name==LEX else [str(Path(name).relative_to('core'))]
   objects=[('ghosterm_core',n) for n in sources]+[(binary,'tests/'+binary+'.c')]
   # Some make/filesystem combinations collapse rapid restore/write timestamps.
   # Remove actual objects and products, then require real compilation/linking.
   for owner,n in objects:(build/'CMakeFiles'/(owner+'.dir')/(n+'.o')).unlink(missing_ok=True)
   for n in ['libghosterm_core.a',binary,'test_config']:(build/n).unlink(missing_ok=True)
   output=run(label,['cmake','--build',build,'--target',binary,'test_config','-j','4'],budget=300)
   for owner,n in objects:assert 'Building C object CMakeFiles/'+owner+'.dir/'+n+'.o' in output,(label,n)
   assert 'Linking C static library libghosterm_core.a' in output and 'Linking C executable '+binary in output
  def target(label,binary,mode,red=False):
   cmd=[build/binary,mode]
   if binary in ['test_runtime','test_ext_runtime']:cmd.append(sdk)
   if binary=='test_package_check':cmd.append(source/'tmp')
   run(label,cmd,red=red)
  for label,name,old,new,binary,mode in cases:
   path=source/name;original=path.read_bytes()
   try:
    path.write_text(original.decode().replace(old,new)+'\n/* '+MARK+' */\n')
    rebuild(label+'-build',name,binary)
    target(label+'-target',binary,mode,True);run(label+'-control',[build/'test_config',source/'core/tests/fixtures'])
   finally:path.write_bytes(original);assert sha(path)==hashlib.sha256(original).hexdigest()
   rebuild(label+'-restore-build',name,binary)
   target(label+'-restored',binary,mode)
   print(label+': compiled assertion red / same archive config green / rebuilt restore / SHA256',flush=True)
  run('final-build',['cmake','--build',build,'--target',*targets,'-j','4'],budget=300);assert suite('final')==total
  for name,h in hashes.items():assert sha(ROOT/name)==h,name
  for name,h in sdk_hashes.items():assert sha(sdk/name)==h,name
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,groups=[c[0] for c in cases],native_cases=total,commands=commands,inputs_sha256=hashes,sdk_sha256=sdk_hashes,sanitizers=['address','undefined'],leak_sanitizer=False,scope='requested plugin heap reservations, not RSS/App/OS sandbox/device/release'),indent=2)+'\n')
  shutil.rmtree(source);shutil.rmtree(build);print('PLUGIN MEMORY MUTATIONS PASS; logs: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');raise

def stop(number,_frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
