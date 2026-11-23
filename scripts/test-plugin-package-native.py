#!/usr/bin/env python3
"""Check a built Lua package through the native package checker without activation."""
import argparse,hashlib,json,os,shlex,shutil,signal,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def lua_cases(run, cases):
 package = cases / 'preview'
 run('build-preview', ['python3', '-B', ROOT/'scripts/gterm-plugin', 'build',
                       ROOT/'plugins/official/preview', '--out', package])
 facts = dict(side='executor', sdkVersion='1.0.0', protocols=[1], runtimes=['lua'],
              capabilities=['session.rpc', 'resource.receive', 'file.readScoped'])
 def host(name, **changes):
  path = cases / (name + '.json')
  path.write_text(json.dumps({**facts, **changes}))
  return path
 good = host('host')
 invalid = host('invalid-host', sdkVersion='invalid')
 unsupported = host('unsupported-runtime', runtimes=[])
 missing = host('missing-capability', capabilities=['session.rpc'])
 tampered = cases / 'tampered'
 shutil.copytree(package, tampered)
 with (tampered / 'executor.lua').open('ab') as executable:
  executable.write(b'\n-- changed after indexing\n')
 changed = cases / 'changed'
 shutil.copytree(package, changed)
 executable = changed / 'executor.lua'
 original = executable.read_bytes()
 executable.write_bytes(bytes([original[0] ^ 1]) + original[1:])
 rows = [
  ('compatible', package, good, 0, 'OK'),
  ('unknown-host', package, '-', 2, 'HOST_REQUIRED'),
  ('invalid-host', package, invalid, 1, 'HOST'),
  ('unsupported-runtime', package, unsupported, 3, 'HOST'),
  ('missing-capability', package, missing, 3, 'CAPABILITY'),
  ('size-mismatch', tampered, good, 1, 'INTEGRITY'),
  ('hash-mismatch', changed, good, 1, 'INTEGRITY'),
 ]
 vectors = [dict(name=name, directory=directory, hostFile=host_file, registryFile='-',
                 status=status, code=code) for name, directory, host_file, status, code in rows]
 vectors[0]['expected'] = dict(id='org.ghosterm.preview', executor=True, entry='executor.lua',
                              runtime='lua', packageDigest=sha(package/'integrity.json'),
                              manifestDigest=sha(package/'manifest.json'))
 for mode in ('cancel', 'late-cancel'):
  vectors.append(dict(name=mode, directory=package, hostFile=good, registryFile='-',
                      status=7, code='CANCELLED', arguments=[mode]))
 return vectors

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--binary',type=Path,required=True);parser.add_argument('--case',action='append',default=[]);parser.add_argument('--oom-sweep',action='store_true',help='fail each allocation ordinal of the compatible fixture');parser.add_argument('--android',help='explicit adb serial; binary must be an arm64 Android executable');args=parser.parse_args();binary=args.binary.resolve()
 inputs=[p for folder in ['core','plugins/official/preview'] for p in (ROOT/folder).rglob('*') if p.is_file() and not any(x.startswith('build') for x in p.relative_to(ROOT/folder).parts)]+[Path(__file__),ROOT/'scripts/gterm-plugin',ROOT/'scripts/plugin_lua_manifest.py'];hashes={str(p.relative_to(ROOT)):sha(p) for p in inputs}
 work=Path(tempfile.mkdtemp(prefix='pkg-native-',dir=os.environ['TMPDIR']));logs=work/'logs';logs.mkdir();cases=work/'cases';cases.mkdir();commands=[];env=dict(os.environ,TMPDIR=str(work),TMP=str(work),TEMP=str(work));device=None;remote=None;uploaded=False
 adb=ROOT/'build/android-toolchain/sdk/platform-tools/adb'
 def remote_arg(arg):
  text=str(arg)
  if text.startswith(str(cases)+'/'):return remote+'/cases/'+Path(text).relative_to(cases).as_posix()
  return text
 def run(label,cmd,budget=90):
  if args.android and Path(str(cmd[0])).resolve()==binary:cmd=[adb,'-s',args.android,'shell','exec '+shlex.join([remote+'/check',*[remote_arg(a) for a in cmd[1:]]])]
  shell='. "$1/scripts/lib/gt-timeout.sh"; gt_timeout_require package-native; shift; gt_timeout "$@"'
  p=subprocess.run(['bash','-c',shell,'package-native',str(ROOT),str(budget),*map(str,cmd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=budget+10);out=p.stdout+p.stderr;(logs/(label+'.log')).write_text(out);commands.append(dict(label=label,returncode=p.returncode))
  if p.returncode:raise RuntimeError(label+' (exit '+str(p.returncode)+')\n'+out)
  return p.stdout
 try:
  vectors=lua_cases(run,cases);known={v['name'] for v in vectors}
  if set(args.case)-known:parser.error('unknown case')
  if args.case:vectors=[v for v in vectors if v['name'] in args.case]
  if args.android:
   header=binary.read_bytes()[:20];assert header[:4]==b'\x7fELF' and header[4]==2 and int.from_bytes(header[18:20],'little')==183,'expected arm64 Android ELF'
   run('device-state',[adb,'-s',args.android,'get-state']);device=dict(serial=args.android,api=run('device-api',[adb,'-s',args.android,'shell','getprop','ro.build.version.sdk']).strip(),abi=run('device-abi',[adb,'-s',args.android,'shell','getprop','ro.product.cpu.abi']).strip(),page_size=run('device-page-size',[adb,'-s',args.android,'shell','getconf','PAGE_SIZE']).strip(),kind='adb execution, synthetic host/program availability facts; no App activation')
   remote='/data/local/tmp/ghosterm-package-'+work.name;run('device-mkdir',[adb,'-s',args.android,'shell','mkdir','-p',remote]);uploaded=True
   run('device-binary',[adb,'-s',args.android,'push',binary,remote+'/check']);run('device-mode',[adb,'-s',args.android,'shell','chmod','700',remote+'/check']);run('device-cases',[adb,'-s',args.android,'push',cases,remote+'/cases'],budget=180)
  results=[]
  for v in vectors:
   out=run(v['name'],[binary,'probe',v['directory'],v['hostFile'],v['registryFile'],v['status'],v['code'],*v.get('arguments',[])]);result=json.loads(out)
   for key,value in v.get('expected',{}).items():assert result[key]==value,(v['name'],key,result.get(key),value)
   results.append(dict(name=v['name'],status=result['status'],code=result['code']))
  # Allocation failures in validation leave the captured package with its caller.
  if not args.case:
   v=vectors[0];baseline=json.loads((logs/(v['name']+'.log')).read_text());last=baseline['allocationCalls']
   for fail in (range(1,last+1) if args.oom_sweep else sorted({1,2,10,100,1000,last//2,last})):
    if fail>last:continue
    code='MEMORY';run('oom-'+str(fail),[binary,'probe',v['directory'],v['hostFile'],v['registryFile'],6,code,'fail='+str(fail)])
    if args.oom_sweep and fail%100==0:print('OOM sweep '+str(fail)+'/'+str(last),flush=True)
  for name,h in hashes.items():assert sha(ROOT/name)==h,name
  if uploaded:run('device-cleanup',[adb,'-s',args.android,'shell','rm','-rf',remote]);uploaded=False
  (logs/'result.json').write_text(json.dumps(dict(accepted=True,oom_sweep=args.oom_sweep,fixtures=results,inputs_sha256=hashes,binary_sha256=sha(binary),commands=commands,device=device,scope='real directory capture+native JSON/metadata checks; no plugin/backend activation, source authentication, App or release'),indent=2)+'\n');shutil.rmtree(cases)
  print('PACKAGE NATIVE PASS: '+str(len(results))+' fixtures; logs: '+str(logs),flush=True)
 except BaseException as error:(logs/'failure.txt').write_text(repr(error)+'\n');(logs/'partial-commands.json').write_text(json.dumps(commands,indent=2)+'\n');raise
 finally:
  if uploaded:
   try:run('device-cleanup-after-error',[adb,'-s',args.android,'shell','rm','-rf',remote])
   except BaseException as error:(logs/'cleanup-failure.txt').write_text(repr(error)+'\n')
def stop(number,_frame):raise KeyboardInterrupt('signal '+str(number))
if __name__=='__main__':signal.signal(signal.SIGINT,stop);signal.signal(signal.SIGTERM,stop);main()
