// Actual native child fixtures: only approved local cat/env/pwd executables.
const pause = () => new Promise(resolve => setTimeout(resolve, 1));
function check(value, message) { if (!value) throw Error(message); }
export async function runProcessCase(ctx, mode) {
  check(ctx.process && ctx.invocation.workspaceHandle.id === 'workspace', 'process context');
  let cancelled = false;
  const cancellation = { get cancelled() { return cancelled; } };
  const spawn = (program, argv = [program], cwd = ctx.invocation.workspaceHandle) => ctx.process.spawn({
    program, argv, cwd, envPolicy: 'selected-user-environment', cancellation,
  });
  const untilExit = async child => {
    for (let i=0;i<10000;i++) {
      const result = child.poll(); if (result.state === 'exited') return result;
      await pause();
    }
    throw Error('no exit');
  };
  const collect = async child => {
    const output=[];
    for (;;) {
      const r=child.readStdout(16384);
      if(r.state==='data') output.push(...r.bytes);
      else if(r.state==='eof') break;
      else check(r.state==='wait','read error');
      await pause();
    }
    return Uint8Array.from(output);
  };
  if (mode === 'process-roundtrip') {
    const pending=await spawn('cat'); let released=false;
    const release=pending.release().then(()=>{released=true;});
    for(let i=0;i<10;i++){await pause();check(!released,'release before exit');}
    pending.closeStdin(); await release; check(released,'release');
    const child=await spawn('cat');
    check(child.verifiedVersion==='fixture-cat' && Object.isFrozen(child),'version');
    let sent=0, received=0, closed=false;
    while(received<262144) {
      if(sent<262144) {
        const bytes=Uint8Array.from({length:Math.min(16384,262144-sent)},(_,i)=>(sent+i)%251);
        const w=child.writeStdin(bytes);
        if(w.state==='data') sent+=w.count; else check(w.state==='wait','write');
      }
      if(sent===262144 && !closed){child.closeStdin();closed=true;}
      const r=child.readStdout(16384);
      if(r.state==='data') for(const byte of r.bytes){check(byte===received%251,'binary');received++;}
      else check(r.state==='wait','premature eof');
      await pause();
    }
    check((await untilExit(child)).code===0,'exit code');
    await child.release();
    let invalid=false;try{child.poll();}catch{invalid=true;} check(invalid,'retired child');
    // Every spawn is rechecked against native package scope and workspace IDs.
    for(const [program,cwd] of [['missing',ctx.invocation.workspaceHandle],['cat',{id:'outside'}]]) {
      let denied=false;try{await spawn(program,[program],cwd);}catch{denied=true;}check(denied,'scope denied');
    }
    const env=await spawn('env'), envBytes=await collect(env);
    check(new TextDecoder().decode(envBytes)==='GT_SELECTED=approved\n','selected environment');
    check((await untilExit(env)).code===0,'env exit');await env.release();
    const pwd=await spawn('pwd'), pwdBytes=await collect(pwd);
    check(new TextDecoder().decode(pwdBytes)===process.env.GT_RUNTIME_WORKSPACE+'\n','cwd');
    check((await untilExit(pwd)).code===0,'pwd exit');await pwd.release();
  } else if(mode==='process-first-reason') {
    const child=await spawn('cat');
    check(child.writeStdin(new Uint8Array(64)).state==='data','write before deadline');
    check((await untilExit(child)).terminationReason==='deadline','first deadline');
    check(child.readStdout(16384).state==='error','late output limit');
    check(child.poll().terminationReason==='deadline','preserve first stop cause');await child.release();
  } else if(mode==='process-output') {
    const child=await spawn('cat');
    check(child.writeStdin(new Uint8Array(64)).state==='data','write limit');
    for(;;){const r=child.readStdout(16384);if(r.state==='error')break;check(r.state==='wait','output exposed');await pause();}
    check((await untilExit(child)).terminationReason==='output-limit','output reason');await child.release();
  } else if(mode==='process-cancel') {
    const child=await spawn('cat'); cancelled=true;
    check(child.writeStdin(new Uint8Array([1])).state==='closed','cancel write');
    check((await untilExit(child)).signal!==null,'cancel stop');await child.release();
  } else if(mode==='process-capacity') {
    const children=[];for(let i=0;i<8;i++)children.push(await spawn('cat'));
    let limit=false;try{await spawn('cat');}catch{limit=true;}check(limit,'capacity');
    for(const c of children)c.signal('kill');
    for(const c of children){check((await untilExit(c)).signal!==null,'killed');await c.release();}
    const next=await spawn('cat');next.closeStdin();await untilExit(next);await next.release();
  } else if(mode==='process-stop') {
    await spawn('cat'); process.stderr.write('business child waiting\n');await new Promise(()=>{});
  } else if(mode==='process-deadline') {
    const child=await spawn('cat'), result=await untilExit(child);
    check(result.terminationReason==='deadline' && result.signal!==null,'deadline');await child.release();
  }
}
