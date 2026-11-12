#!/usr/bin/env python3
"""Real gterm child and public native HOST Session, only owned local fixtures."""
import argparse,hashlib,json,os,pty,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build',type=Path,required=True);p.add_argument('--case',choices=['arguments-codec','arguments-invalid','roundtrip','deny','cancel','digest','peer-digest','command','dispose-error','pipes','active-cancel','lost','pending-loss','silent','tty']);a=p.parse_args()
 work=Path(tempfile.mkdtemp(prefix='gterm-stdio-',dir=ROOT/'tmp'));package=work/'package';package.mkdir()
 try:
  manifest=dict(schemaVersion=1,id='org.ghosterm.fixture',pluginVersion='0.1.0',sdkApiRange='^1.0.0',bridgeProtocolRange=[1],publisher='fixture',license='MIT',commands=[dict(path=['fixture'],title='Fixture',argsSchema='schemas/input.json')],executor=dict(variants=[dict(id='portable',runtime='portable-js',entry='executor/index.js',requires=['session.rpc'])]),permissions=[])
  plugin="import {object,text,integer} from '@ghosterm/plugin-sdk';export function activate(ctx){if(ctx.invocation.command!=='fixture'||ctx.invocation.arguments[0]!=='中文'||ctx.invocation.arguments[1]!==''||ctx.invocation.arguments[2]!==\"';throw Error('injected');//\")throw Error('argv');const codec=object({text:text(40),count:integer(0,100)});ctx.session.register({name:'fixture.echo',input:codec,output:codec},async(v,c)=>{c.check();return v});return {dispose(){}}}"
  properties={name:{'type':'string','maxLength':100} for name in ['first','empty','literal']}
  cli_fields={name:{'position':i} for i,name in enumerate(['first','empty','literal'])}
  required=['first','empty','literal']
  schema={'type':'object','properties':properties,'required':required,'additionalProperties':False,'x-ghosterm-cli':{'version':1,'fields':cli_fields}}
  files={'plugin.json':json.dumps(manifest),'README':'fixture','LICENSES/MIT.txt':'fixture','schemas/input.json':json.dumps(schema),'executor/index.js':plugin};members=[]
  for name,text in files.items():
   path=package/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text);members.append(dict(path=name,bytes=len(path.read_bytes()),sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
  (package/'integrity.json').write_text(json.dumps(dict(schemaVersion=1,members=members)));digest=hashlib.sha256((package/'integrity.json').read_bytes()).hexdigest();build=a.build.resolve()
  if a.case=='tty':
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
  for case in [a.case] if a.case else ['arguments-codec','arguments-invalid','roundtrip','deny','cancel','digest','peer-digest','command','dispose-error','pipes','active-cancel','lost','pending-loss','silent']:
   code = plugin.replace("dispose(){}", "dispose(){throw Error('cleanup')} ") if case=='dispose-error' else ("throw Error('not before consent');"+plugin if case in ['deny','cancel'] else plugin)
   if case in ['arguments-codec','arguments-invalid']:
    properties.update(count={'type':'integer','minimum':0,'maximum':100},wrap={'type':'boolean'})
    cli_fields.update(count={'option':'count','default':0},wrap={'option':'wrap','default':False})
    required.extend(name for name in ['count','wrap'] if name not in required)
    schema={'type':'object','properties':properties,'required':required,'additionalProperties':False,'x-ghosterm-cli':{'version':1,'fields':cli_fields}}
    schema_text=json.dumps(schema);(package/'schemas/input.json').write_text(schema_text)
    for member in members:
     if member['path']=='schemas/input.json': member.update(bytes=len(schema_text.encode()),sha256=hashlib.sha256(schema_text.encode()).hexdigest())
    if case=='arguments-codec':
     code=code.replace('import {object,text,integer}', 'import {boolean,commandArguments,positional,option,object,text,integer}')
    if case=='arguments-codec':
     code=code.replace("const codec=", "const a=commandArguments({first:positional(text(100),0),empty:positional(text(100),1),literal:positional(text(100),2),count:option(integer(0,100),'count',0),wrap:option(boolean(),'wrap',false)}).parse(ctx.invocation.arguments);const h=ctx.invocation.decodedArguments;if(a.first!=='中文'||a.empty!==''||a.literal!==ctx.invocation.arguments[2]||a.count!==17||!a.wrap||!Object.isFrozen(a)||!h||h.first!==a.first||h.empty!==a.empty||h.literal!==a.literal||h.count!==a.count||h.wrap!==a.wrap||!Object.isFrozen(h))throw Error('decoded argv');const codec=")
   if case=='pending-loss':code=code.replace('c.check();return v','c.check();return await new Promise(()=>{})')
   (package/'executor/index.js').write_text(code)
   for member in members:
    if member['path']=='executor/index.js': member.update(bytes=len(code.encode()),sha256=hashlib.sha256(code.encode()).hexdigest())
   (package/'integrity.json').write_text(json.dumps(dict(schemaVersion=1,members=members)));digest=hashlib.sha256((package/'integrity.json').read_bytes()).hexdigest()
   r=subprocess.run([str(build/'test_gterm_stdio'),str(build/'gterm'),str(package),digest,case],capture_output=True,text=True,timeout=20)
   if "ABNORMAL" in r.stderr or "AddressSanitizer" in r.stderr or "runtime error:" in r.stderr:raise RuntimeError(r.stderr)
   assert r.returncode==0,(case,r.returncode,r.stdout,r.stderr)
   assert 'PASS stdio '+case in r.stdout,r.stdout
   print(r.stdout,end='')
 finally:shutil.rmtree(work)
if __name__=='__main__':
 try:main()
 except AssertionError as error:print('FAIL',error);raise SystemExit(1)
