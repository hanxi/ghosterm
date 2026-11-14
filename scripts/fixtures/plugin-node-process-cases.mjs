// Synthetic adapter responses, independent of actual native process evidence.
import assert from 'node:assert/strict';
import {pathToFileURL} from 'node:url';
import path from 'node:path';
const [bundle,selected]=process.argv.slice(2);
const {NodeNativePort}=await import(pathToFileURL(path.join(bundle,'node/port.js')));
const {NodeProcessService}=await import(pathToFileURL(path.join(bundle,'node/process.js')));
const options=()=>({program:'cat',argv:['cat','',`中文 "';$()`],cwd:{id:'workspace'},envPolicy:'selected-user-environment',cancellation:{cancelled:false}});
function fixture(handler) {
  let calls=0,aborts=0;
  const native=new NodeNativePort({exchange(text){calls++;const q=JSON.parse(text);assert.equal(q.op,'process');return JSON.stringify({v:1,id:q.id,status:'ok',value:handler(q)});},abort(){aborts++;}});
  return {native,service:new NodeProcessService(native),get calls(){return calls;},get aborts(){return aborts;}};
}
async function broken(f,call) {
  await assert.rejects(async()=>call());assert.equal(f.native.broken,true);assert.equal(f.aborts,1);
  const calls=f.calls;assert.throws(()=>f.service.available());assert.equal(calls,f.calls);
}
const spawn=(q,rest)=>q.action==='spawn'?{status:'ok',ref:'1',version:'fixture-cat'}:rest(q);
const cases={
  async available(){
    assert.equal(fixture(()=>({status:'ok',available:false})).service.available(),false);
    const f=fixture(()=>({status:'ok',available:'true'}));await broken(f,()=>f.service.available());
  },
  async fields(){const f=fixture(()=>({status:'ok',available:true,extra:true}));await broken(f,()=>f.service.available());},
  async status(){const f=fixture(()=>({status:'invented'}));await broken(f,()=>f.service.spawn(options()));},
  async reference(){const f=fixture(()=>({status:'ok',ref:'01',version:'fixture'}));await broken(f,()=>f.service.spawn(options()));},
  async reuse(){const f=fixture(()=>({status:'ok',ref:'1',version:'fixture'}));await f.service.spawn(options());await broken(f,()=>f.service.spawn(options()));},
  async version(){const f=fixture(()=>({status:'ok',ref:'1',version:''}));await broken(f,()=>f.service.spawn(options()));},
  async read(){
    const f=fixture(q=>spawn(q,()=>({status:'ok',state:'data',count:4,error:0,hex:'000aff80'})));
    const c=await f.service.spawn(options());assert.deepEqual(c.readStdout(16384),{state:'data',bytes:Uint8Array.of(0,10,255,128)});
  },
  async bytes(){const f=fixture(q=>spawn(q,()=>({status:'ok',state:'data',count:2,error:0,hex:'ff'})));const c=await f.service.spawn(options());await broken(f,()=>c.readStdout(16384));},
  async errorBytes(){const f=fixture(q=>spawn(q,()=>({status:'ok',state:'error',count:0,error:3,hex:'01'})));const c=await f.service.spawn(options());await broken(f,()=>c.readStdout(16384));},
  async count(){const f=fixture(q=>spawn(q,()=>({status:'ok',state:'wait',count:1,error:0,hex:'00'})));const c=await f.service.spawn(options());await broken(f,()=>c.readStdout(16384));},
  async write(){
    const f=fixture(q=>spawn(q,()=>{assert.equal(q.hex.length,6144);return{status:'ok',state:'data',count:3072,error:0};}));
    const c=await f.service.spawn(options());assert.deepEqual(c.writeStdin(new Uint8Array(16384)),{state:'data',count:3072});
  },
  async writeCount(){const f=fixture(q=>spawn(q,()=>({status:'ok',state:'data',count:2,error:0})));const c=await f.service.spawn(options());await broken(f,()=>c.writeStdin(Uint8Array.of(1)));},
  async exit(){
    const f=fixture(q=>spawn(q,()=>({status:'ok',exited:true,code:0,signal:15,reason:'deadline'})));
    const c=await f.service.spawn(options());assert.deepEqual(c.poll(),{state:'exited',code:null,signal:15,terminationReason:'deadline'});
  },
  async unknownExit(){const f=fixture(q=>spawn(q,()=>({status:'ok',exited:false,code:0,signal:15,reason:'none'})));const c=await f.service.spawn(options());await broken(f,()=>c.poll());},
  async release(){
    let done=false,wait=true;
    const f=fixture(q=>spawn(q,()=>({status:wait?'wait':'ok'}))),c=await f.service.spawn(options());
    const p=c.release().then(()=>{done=true;});await Promise.resolve();assert.equal(done,false);
    f.service.step();await Promise.resolve();assert.equal(done,false);wait=false;f.service.step();await p;
    assert.equal(done,true);assert.throws(()=>c.poll());
  },
  async retirement(){
    let id=0;const f=fixture(q=>q.action==='spawn'?{status:'ok',ref:String(++id),version:'fixture'}:{status:'ok'});
    for(let i=0;i<80;i++){const c=await f.service.spawn(options());await c.release();assert.throws(()=>c.poll());}
  },
  async cancellation(){
    const o=options();let signals=0;
    const f=fixture(q=>spawn(q,()=>{signals++;return{status:'ok'};})),c=await f.service.spawn(o);
    o.cancellation.cancelled=true;assert.deepEqual(c.writeStdin(Uint8Array.of(1)),{state:'closed'});assert.equal(signals,1);
  },
};
try {for(const name of selected?[selected]:Object.keys(cases)){assert.ok(cases[name],name);await cases[name]();console.log(name+': PASS');}}
catch(error){console.error('FAIL Node process: '+error.stack);process.exitCode=1;}
