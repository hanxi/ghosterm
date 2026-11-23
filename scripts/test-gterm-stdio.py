#!/usr/bin/env python3
"""Real gterm child and public native HOST Session, only owned local fixtures."""
import argparse,hashlib,json,os,pty,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def run_preview_runtime(build,work,package):
 if package.exists(): package.rmdir()
 subprocess.run(['python3','-B',ROOT/'scripts/gterm-plugin','build',ROOT/'plugins/official/preview',
                 '--out',package],cwd=ROOT,check=True,timeout=30)
 workspace=work/'workspace';site=workspace/'site';site.mkdir(parents=True)
 (site/'index.md').write_text('# Preview runtime fixture\n')
 (site/'theme,dark.css').write_text('body { color: rgb(1, 2, 3); }\n')
 store=work/'resource-store';store.mkdir(mode=0o700)
 runtime_tmp=work/'runtime-tmp';runtime_tmp.mkdir(mode=0o700)
 digest=hashlib.sha256((package/'integrity.json').read_bytes()).hexdigest()
 env=os.environ.copy();env['TMPDIR']=str(runtime_tmp)
 r=subprocess.run([str(build/'test_gterm_stdio'),str(build/'gterm'),str(package),digest,'preview',str(workspace),str(store)],capture_output=True,text=True,timeout=45,env=env)
 if 'ABNORMAL' in r.stderr or 'AddressSanitizer' in r.stderr or 'runtime error:' in r.stderr:raise RuntimeError(r.stderr)
 assert r.returncode==0,('preview-runtime',r.returncode,r.stdout,r.stderr)
 assert 'PASS stdio preview' in r.stdout,r.stdout
 print(r.stdout,end='')
 dot_env={**env,'GT_GTERM_PREVIEW_MAIN':'./site/./index.md'}
 dot_store=work/'dot-resource-store';dot_store.mkdir(mode=0o700)
 r=subprocess.run([str(build/'test_gterm_stdio'),str(build/'gterm'),str(package),digest,'preview',str(workspace),str(dot_store)],capture_output=True,text=True,timeout=45,env=dot_env)
 if 'ABNORMAL' in r.stderr or 'AddressSanitizer' in r.stderr or 'runtime error:' in r.stderr:raise RuntimeError(r.stderr)
 assert r.returncode==0,('preview-dot-path-runtime',r.returncode,r.stdout,r.stderr)
 print('PASS stdio Preview ./ identity components through real Core resource publication')
 unicode_parts=['😀'*14]+['😀'*16]*15+['😀'*15]*14+['😀'*15+'.md']
 unicode_path='/'.join(unicode_parts)
 assert len(unicode_path)==512 and len(unicode_parts)==31
 unicode_workspace=work/'unicode-workspace';unicode_workspace.mkdir()
 current_fd=os.open(unicode_workspace,os.O_RDONLY|os.O_DIRECTORY)
 for component in unicode_parts[:-1]:
  os.mkdir(component,dir_fd=current_fd)
  next_fd=os.open(component,os.O_RDONLY|os.O_DIRECTORY,dir_fd=current_fd)
  os.close(current_fd);current_fd=next_fd
 file_fd=os.open(unicode_parts[-1],os.O_WRONLY|os.O_CREAT|os.O_EXCL,0o600,dir_fd=current_fd)
 os.write(file_fd,b'# Preview runtime fixture\n')
 os.close(file_fd);os.close(current_fd)
 unicode_store=work/'unicode-resource-store';unicode_store.mkdir(mode=0o700)
 r=subprocess.run([str(build/'test_gterm_stdio'),str(build/'gterm'),str(package),digest,'preview-unicode',str(unicode_workspace),str(unicode_store),unicode_path],capture_output=True,text=True,timeout=45,env=env)
 if 'ABNORMAL' in r.stderr or 'AddressSanitizer' in r.stderr or 'runtime error:' in r.stderr:raise RuntimeError(r.stderr)
 assert r.returncode==0,('preview-unicode',r.returncode,r.stdout,r.stderr)
 assert 'PASS stdio preview-unicode' in r.stdout,r.stdout
 print(r.stdout,end='')
def build_fixture(work, package, case):
 source = work / 'fixture-source'
 if source.exists():
  shutil.rmtree(source)
 (source / 'schemas').mkdir(parents=True)
 required = ['first', 'empty', 'literal']
 properties = {name: {'type': 'string', 'maxLength': 100} for name in required}
 fields = {name: {'position': index} for index, name in enumerate(required)}
 if case in ('arguments-codec', 'arguments-invalid'):
  required += ['count', 'wrap']
  properties.update(count={'type': 'integer', 'minimum': 0, 'maximum': 100}, wrap={'type': 'boolean'})
  fields.update(count={'option': 'count', 'default': 0}, wrap={'option': 'wrap', 'default': False})
 schema = dict(type='object', properties=properties, required=required, additionalProperties=False)
 schema['x-ghosterm-cli'] = dict(version=1, fields=fields)
 (source / 'schemas/input.json').write_text(json.dumps(schema))
 executor = '"session.rpc"'
 programs = ''
 if case == 'process-config':
  executor += ', "process.spawn", "process.pipes"'
  programs = ', programs = { "cat" }'
 declaration = f'''local manifest = {{
  id = "org.ghosterm.fixture", version = "0.1.0", api = "1", publisher = "fixture", license = "MIT",
  executor = {{ entry = "executor.lua", requires = {{ {executor} }}, rpc = {{ "fixture.echo" }}{programs} }},
  commands = {{ {{ path = {{ "fixture" }}, title = "Fixture", argsSchema = "schemas/input.json" }} }},
}}
return {{}}
'''
 (source / 'main.lua').write_text(declaration)
 code = '''local ctx = ...
local args = ctx.arguments
assert(args.first == "中文" and args.empty == "" and args.literal == "';throw Error('injected');//")
'''
 if case in ('deny', 'cancel'):
  code = 'error("fixture loaded before consent")\n' + code
 if case == 'arguments-codec':
  code += 'assert(args.count == 17 and args.wrap == true)\n'
 if case == 'process-config':
  code += 'assert(ctx.process ~= nil)\n'
 if case == 'host-method':
  code += '''local reply = ctx.rpc.call("fixture.ask", { text = "host", count = 17 }, 3000)
assert(reply and reply.text == "host" and reply.count == 17)
'''
 body = 'while true do ctx.wait() end' if case == 'pending-loss' else 'return input'
 (source / 'executor.lua').write_text(code + f'return {{ ["fixture.echo"] = function(input) {body} end }}\n')
 subprocess.run(['python3', '-B', ROOT / 'scripts/gterm-plugin', 'build', source, '--out', package],
                cwd=ROOT, check=True, timeout=30)
 return hashlib.sha256((package / 'integrity.json').read_bytes()).hexdigest()

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--case',choices=['arguments-codec','arguments-invalid','host-method','roundtrip','deny','cancel','digest','peer-digest','command','pipes','active-cancel','lost','pending-loss','silent','tty','preview-runtime','process-config']);a=p.parse_args()
 project_tmp=ROOT/'tmp';scratch=Path(os.environ.get('TMPDIR',project_tmp)).resolve()
 if not scratch.is_dir() or not scratch.is_relative_to(project_tmp): scratch=project_tmp
 work=Path(tempfile.mkdtemp(prefix='gterm-stdio-',dir=scratch));package=work/'package'
 try:
  if a.case=='preview-runtime':
   run_preview_runtime(a.build.resolve(),work,package);return
  build=a.build.resolve()
  if a.case=='tty':
   digest=build_fixture(work,package,'roundtrip')
   command=[str(build/'gterm'),'relay','--stdio','--trust-development-digest',digest,'--client-digest','d'*64,'--',str(package),'fixture']
   for direction in ['input','output']:
    master,slave=pty.openpty()
    try:
     r=subprocess.run(command,stdin=slave if direction=='input' else subprocess.DEVNULL,stdout=subprocess.PIPE if direction=='input' else slave,stderr=subprocess.PIPE,timeout=15)
     assert r.returncode==2 and b'protocol refuses a terminal' in r.stderr,(r.returncode,r.stderr)
     if direction=='input':assert r.stdout==b'',r.stdout
     else:
      os.set_blocking(master,False)
      try:data=os.read(master,4096)
      except BlockingIOError:data=b''
      assert not data,data
    finally:os.close(master);os.close(slave)
   print('PASS stdio tty input/output');return
  for case in [a.case] if a.case else ['arguments-codec','arguments-invalid','host-method','roundtrip','deny','cancel','digest','peer-digest','command','pipes','active-cancel','lost','pending-loss','silent']:
   process_config=case=='process-config'
   target=package if a.case else work/case
   digest=build_fixture(work,target,case)
   command=[str(build/'test_gterm_stdio'),str(build/'gterm'),str(target),digest,case]
   if case=='process-config':
    workspace=work/'process-workspace';workspace.mkdir()
    command += [str(workspace),'/bin/cat','fixture-1',hashlib.sha256(Path('/bin/cat').read_bytes()).hexdigest()]
   r=subprocess.run(command,capture_output=True,text=True,timeout=20)
   if "ABNORMAL" in r.stderr or "AddressSanitizer" in r.stderr or "runtime error:" in r.stderr:raise RuntimeError(r.stderr)
   assert r.returncode==0,(case,r.returncode,r.stdout,r.stderr)
   assert 'PASS stdio '+case in r.stdout,r.stdout
   print(r.stdout,end='')
   if case=='process-config':
    blocked=subprocess.run([str(build/'gterm'),'relay','--stdio','--trust-development-digest',digest,
      '--client-digest','d'*64,'--root','workspace',str(workspace),'--method','fixture.echo','--',str(target),'fixture'],
      input='',capture_output=True,text=True,timeout=20)
    assert blocked.returncode==4 and 'package compatibility failed' in blocked.stderr,(
      'process-required package without Host mapping must be unsupported',blocked.returncode,blocked.stderr)
    print('PASS stdio process-config-unavailable')
  if a.case is None:run_preview_runtime(build,work,package)
 finally:shutil.rmtree(work)
if __name__=='__main__':
 try:main()
 except AssertionError as error:print('FAIL',error);raise SystemExit(1)
