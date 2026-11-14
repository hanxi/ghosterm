// Synthetic adapter faults only; actual native effects are tested separately.
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
const [bundle, selected] = process.argv.slice(2);
const {NodeNativePort} = await import(pathToFileURL(path.join(bundle,'node/port.js')));
const {NodeResourcePort} = await import(pathToFileURL(path.join(bundle,'node/resources.js')));
const descriptor = () => ({id:'a'.repeat(32),generation:'1',sha256:'b'.repeat(64),displayName:'展示"\\.bin',mediaType:'application/octet-stream',sizeKnown:true,bytes:'3'});
const publication = () => ({phase:'uncertain',error:'unknown',expired:false,clockError:false,hasTransfer:true,confirmed:false,transferState:'unknown',readBytes:'3',accepted:'1',descriptor:descriptor()});
function fixture(handler) {
  let calls=0,aborts=0;
  const native=new NodeNativePort({exchange(text){calls++; const q=JSON.parse(text);assert.equal(q.op,'resource');return JSON.stringify({v:1,id:q.id,status:'ok',value:handler(q)});},abort(){aborts++;}});
  return {port:new NodeResourcePort(native), native, get calls(){return calls;},get aborts(){return aborts;}};
}
function broken(f,call) {
  assert.throws(call);
  assert.equal(f.native.broken,true);assert.equal(f.aborts,1);
  const count=f.calls;assert.throws(()=>f.port.resourceWorkspaces());assert.equal(f.calls,count);
}
const cases = {
  workspaces(){
    const f=fixture(()=>({status:'ok',roots:['workspace']}));assert.deepEqual(f.port.resourceWorkspaces(),['workspace']);
    assert.equal(fixture(()=>({status:'unsupported'})).port.resourceWorkspaces(),null);
    const bad=fixture(()=>({status:'ok',roots:['same','same']}));broken(bad,()=>bad.port.resourceWorkspaces());
  },
  fields(){const f=fixture(()=>({status:'ok',roots:[],extra:true}));broken(f,()=>f.port.resourceWorkspaces());},
  status(){const f=fixture(()=>({status:'invented'}));broken(f,()=>f.port.resourceWorkspaces());},
  path(){
    const input='展示"\\.bin'; const boundary='"'.repeat(4096);
    let id=0;const f=fixture(q=>{assert.equal(Buffer.from(q.path64,'base64').toString('utf8'),id?boundary:input);assert.ok(Buffer.byteLength(JSON.stringify(q))<=8192);return{status:'ok',handle:String(++id)};});
    assert.equal(f.port.resourcePublish('workspace',input,'text/plain','1',1000).handle,'1');
    assert.equal(f.port.resourcePublish('workspace',boundary,'text/plain','1',1000).handle,'2');
  },
  publication(){
    const f=fixture(q=>q.action==='publish'?{status:'ok',handle:'1'}:{status:'ok',publication:publication()});
    f.port.resourcePublish('workspace','file.bin','text/plain','3',1000);
    const p=f.port.resourcePublication('1');assert.equal(p.phase,'uncertain');assert.equal(p.error,'unknown');assert.equal(p.transferState,'unknown');assert.equal(p.confirmed,false);
  },
  publicationShape(){
    const f=fixture(q=>q.action==='publish'?{status:'ok',handle:'1'}:{status:'ok',publication:{...publication(),confirmed:'true'}});
    f.port.resourcePublish('workspace','file.bin','text/plain','3',1000);broken(f,()=>f.port.resourcePublication('1'));
  },
  reference(){
    const f=fixture(q=>q.action==='take'?{status:'ok',handle:'1',descriptor:descriptor()}:{status:'ok'});
    assert.equal(f.port.resourceTake().handle,'1');assert.equal(f.port.resourceRelease('1'),'ok');assert.equal(f.port.retained,0);
    broken(f,()=>f.port.resourceTake());
  },
  domains(){
    const f=fixture(q=>q.action==='publish'?{status:'ok',handle:'1'}:q.action==='take'?{status:'ok',handle:'2',descriptor:descriptor()}:{status:'ok'});
    f.port.resourcePublish('workspace','file.bin','text/plain','3',1000);f.port.resourceTake();const n=f.calls;
    assert.equal(f.port.resourceRelease('1'),'invalid');assert.equal(f.port.resourceCancel('2'),'invalid');assert.equal(f.calls,n);
  },
  retirement(){
    let wait=true;const f=fixture(q=>q.action==='publish'?{status:'ok',handle:'1'}:{status:wait?'wait':'ok'});
    f.port.resourcePublish('workspace','file.bin','text/plain','3',1000);assert.equal(f.port.resourceForget('1'),'wait');assert.equal(f.port.retained,1);
    wait=false;assert.equal(f.port.resourceForget('1'),'ok');assert.equal(f.port.retained,0);const n=f.calls;assert.equal(f.port.resourceCancel('1'),'invalid');assert.equal(f.calls,n);
  },
  read(){
    const f=fixture(q=>q.action==='take'?{status:'ok',handle:'1',descriptor:descriptor()}:q.action==='open'?{status:'ok'}:{status:'ok',hex:'0001ff'});
    f.port.resourceTake();assert.equal(f.port.resourceOpen('1','3',1000),'ok');assert.deepEqual(f.port.resourceRead('1','0',16384).bytes,new Uint8Array([0,1,255]));
  },
  readLimit(){const f=fixture(q=>q.action==='take'?{status:'ok',handle:'1',descriptor:descriptor()}:{status:'ok',hex:'00'.repeat(3073)});f.port.resourceTake();broken(f,()=>f.port.resourceRead('1','0',16384));},
  errorBytes(){const f=fixture(q=>q.action==='take'?{status:'ok',handle:'1',descriptor:descriptor()}:{status:'closed',hex:'00'});f.port.resourceTake();broken(f,()=>f.port.resourceRead('1','0',16384));},
  descriptor(){const f=fixture(()=>({status:'ok',handle:'1',descriptor:{...descriptor(),generation:9007199254740992}}));broken(f,()=>f.port.resourceTake());},
  capacity(){let n=0;const f=fixture(()=>({status:'ok',handle:String(++n),descriptor:descriptor()}));for(let i=0;i<64;i++)f.port.resourceTake();broken(f,()=>f.port.resourceTake());},
};
try {
  assert.ok(!selected || Object.hasOwn(cases,selected));
  for (const [name, test] of Object.entries(cases)) if (!selected || selected===name) test();
  console.log(`NODE RESOURCE CASES PASS: ${selected??Object.keys(cases).length}`);
} catch(error) {
  if (error?.code==='ERR_ASSERTION') {console.error(`ASSERT ${selected??'all'}: ${error.message}`);process.exitCode=1;}
  else {console.error(`ERROR ${error?.stack??error}`);process.exitCode=2;}
}
