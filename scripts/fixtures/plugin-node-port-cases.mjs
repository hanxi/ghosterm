// Focused adapter conformance with explicitly synthetic native replies. Real C
// Session/pipe evidence is collected separately; these are not OS effect proofs.
import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
const root = path.resolve(process.argv[2]), selected = process.argv[3];
const { NodeNativePort } = await import(pathToFileURL(path.join(root, 'node/port.js')));
const { parseMessage } = await import(pathToFileURL(path.join(root, 'node/message.js')));
const sdk = await import(pathToFileURL(path.join(root, 'portable/index.js')));

function fake(overrides = {}) {
  const calls = []; let reference = 0, delivered = false;
  const state = { access: 'allowed', result: 'pending', confirmed: false, outcome: null, interrupted: false };
  const pipe = {
    aborted: false, calls, state,
    exchange(text) {
      const q = JSON.parse(text); calls.push(q);
      assert.equal(q.id, String(calls.length));
      const r = { v: 1, id: q.id, status: 'ok' };
      if (q.op === 'request') Object.assign(r, { handle: String(++reference) });
      if (q.op === 'take') {
        if (delivered) r.status = 'wait';
        else { delivered = true; Object.assign(r, { ticket: String(++reference), method: q.methods[0], input: { peer: true } }); }
      }
      if (q.op === 'result') Object.assign(r, { state: state.result, confirmed: state.confirmed,
        cancelRequested: false, deadlineElapsed: false, outcome: state.outcome });
      if (q.op === 'cancel') Object.assign(state, { result: 'cancelled', outcome: { kind: 'cancelled', effect: 'none' } });
      if (q.op === 'respond' && state.interrupted) r.interrupted = true;
      if (q.op === 'access') r.access = state.access;
      if (q.op === 'revoke') state.access = 'revoked';
      return overrides.reply ? overrides.reply(q, r, pipe) : JSON.stringify(r);
    },
    abort() { this.aborted = true; },
  };
  return { pipe, port: new NodeNativePort(pipe) };
}
const input = '{"peer":true}', success = { kind: 'success', value: { peer: true } };
function malformed(reply, operation = p => p.access('ui.form')) {
  const { pipe, port } = fake({ reply });
  assert.throws(() => operation(port)); assert.equal(port.broken, true); assert.equal(pipe.aborted, true);
  const n = pipe.calls.length;
  assert.throws(() => port.access('ui.form')); assert.equal(pipe.calls.length, n);
}
const cases = {
  roundtrip() {
    const { pipe, port } = fake();
    const q = port.request('fixture.ask', input, 1000); assert.equal(q.handle, '1');
    assert.equal(port.result(q.handle).state, 'pending');
    Object.assign(pipe.state, { result: 'succeeded', confirmed: true, outcome: success });
    const result = port.result(q.handle); assert.equal(result.state, 'succeeded'); assert.equal(result.confirmed, true);
    assert.equal(JSON.parse(result.outcome).value.peer, true);
    assert.equal(port.forget(q.handle), 'ok'); assert.equal(port.retained, 0);
  },
  admission() {
    for (const s of ['wait', 'invalid', 'denied', 'unsupported', 'closed']) {
      const { port } = fake({ reply(q) { return JSON.stringify({ v: 1, id: q.id, status: s }); } });
      assert.deepEqual(port.request('fixture.ask', input, 1000), { status: s }); assert.equal(port.retained, 0);
    }
  },
  access() {
    const { pipe, port } = fake();
    for (const value of ['unknown', 'allowed', 'denied', 'revoked', 'unsupported']) {
      pipe.state.access = value; assert.equal(port.access('ui.form'), value);
    }
    assert.equal(pipe.calls.length, 5);
  },
  cancel() {
    const { port } = fake(); const q = port.request('fixture.ask', input, 1000);
    assert.equal(port.cancel(q.handle), 'ok'); const r = port.result(q.handle);
    assert.equal(r.state, 'cancelled'); assert.equal(r.confirmed, false);
    assert.equal(JSON.parse(r.outcome).effect, 'none');
  },
  unknown() {
    const { pipe, port } = fake(); const q = port.request('fixture.ask', input, 1000);
    Object.assign(pipe.state, { result: 'unknown', confirmed: false, outcome: { kind: 'unknown', reason: 'link lost' } });
    const r = port.result(q.handle); assert.equal(r.state, 'unknown'); assert.equal(r.confirmed, false);
    assert.equal(JSON.parse(r.outcome).kind, 'unknown');
  },
  interrupted() {
    const { pipe, port } = fake(); const q = port.take(['fixture.echo']); assert(q);
    assert.equal(port.check(q.ticket), 'ok'); pipe.state.interrupted = true;
    assert.doesNotThrow(() => assert.equal(port.respond(q.ticket, JSON.stringify(success)), 'ok'));
    assert.equal(port.retained, 0);
  },
  references() {
    const { pipe, port } = fake(); const outgoing = port.request('fixture.ask', input, 1000);
    const incoming = port.take(['fixture.echo']); assert(incoming);
    assert.notEqual(incoming.ticket, outgoing.handle);
    const before = pipe.calls.length;
    assert.throws(() => port.result(incoming.ticket));
    assert.equal(port.check(outgoing.handle), 'invalid'); assert.equal(pipe.calls.length, before);
    assert.equal(port.respond(incoming.ticket, JSON.stringify(success)), 'ok'); assert.equal(port.retained, 1);
  },
  reuse() {
    const { port } = fake({ reply(q, r) { if (q.op === 'request') r.handle = '1'; return JSON.stringify(r); } });
    const q = port.request('fixture.ask', input, 1000);
    assert.equal(port.forget(q.handle), 'ok');
    assert.throws(() => port.request('fixture.ask', input, 1000)); assert.equal(port.broken, true);
  },
  capacity() {
    const { pipe, port } = fake();
    for (let i = 1; i <= 64; i++) assert.equal(port.request('fixture.ask', input, 1000).handle, String(i));
    const before = pipe.calls.length;
    assert.deepEqual(port.request('fixture.ask', input, 1000), { status: 'wait' }); assert.equal(pipe.calls.length, before);
  },
  resultSchema() {
    for (const change of [r => { r.confirmed = 'true'; }, r => { r.state = ['succeeded']; },
      r => { r.confirmed = false; }, r => { r.outcome = null; }, r => { r.extra = true; }]) {
      malformed((q, r) => {
        if (q.op === 'result') { Object.assign(r, { state: 'succeeded', confirmed: true, cancelRequested: false, deadlineElapsed: false, outcome: success }); change(r); }
        return JSON.stringify(r);
      }, p => p.result(p.request('fixture.ask', input, 1000).handle));
    }
  },
  replyIdentity() {
    for (const change of [r => { r.v = 2; }, r => { r.id = '2'; }, r => { r.status = 'unknown'; },
      r => { r.token = 'fake'; }, r => { r.interrupted = true; }]) {
      malformed((q, r) => { change(r); return JSON.stringify(r); });
    }
  },
  duplicate() {
    malformed(() => '{"v":1,"id":"1","status":"ok","access":"allowed","\\u0061ccess":"allowed"}');
  },
  reentry() {
    let port;
    const made = fake({ reply(q, r) { assert.throws(() => port.access('ui.form')); return JSON.stringify(r); } });
    port = made.port; assert.throws(() => port.access('ui.form')); assert.equal(port.broken, true);
  },
  ambiguous() {
    let sends = 0;
    const port = new NodeNativePort({ exchange() { sends++; throw Error('CHILD_PIPE_TIMEOUT'); }, abort() {} });
    const session = new sdk.SessionChannel(port), codec = sdk.object({ peer: sdk.boolean() });
    assert.throws(() => session.request({ name: 'fixture.ask', input: codec, output: codec }, { peer: true }, 1000), /CHILD_PIPE_TIMEOUT/);
    assert.equal(port.broken, true); assert.equal(sends, 1);
    assert.throws(() => session.request({ name: 'fixture.ask', input: codec, output: codec }, { peer: true }, 1000));
    assert.equal(sends, 1); // No fabricated failed/effect:none Call or replay.
  },
  async codec() {
    const { pipe, port } = fake(); const session = new sdk.SessionChannel(port);
    const codec = sdk.object({ peer: sdk.boolean() });
    const call = session.request({ name: 'fixture.ask', input: codec, output: codec }, { peer: true }, 1000);
    Object.assign(pipe.state, { result: 'succeeded', confirmed: true, outcome: { kind: 'success', value: { peer: 'wrong' } } });
    session.step(); const outcome = await call.outcome;
    assert.equal(outcome.kind, 'unknown'); assert.equal(port.retained, 0);
  },
  lexical() {
    for (const text of ['{"x":1,"x":2}', '{"x":{"y":1,"y":2}}', '{"x":"\\ud800"}', '\ufeff{}', '{} false', '[1,]', '1e400']) {
      assert.throws(() => parseMessage(text));
    }
    assert.doesNotThrow(() => parseMessage('{}' + ' '.repeat(8190)));
    assert.throws(() => parseMessage('{}' + ' '.repeat(8191)));
    assert.throws(() => parseMessage('['.repeat(34) + '0' + ']'.repeat(34)));
  },
};
try {
  assert(selected === 'all' || Object.hasOwn(cases, selected));
  for (const [name, run] of Object.entries(cases)) if (selected === 'all' || name === selected) await run();
} catch (error) {
  console.error((error?.name === 'AssertionError' ? 'ASSERT ' : 'ERROR ') + (error?.stack ?? error));
  process.exitCode = error?.name === 'AssertionError' ? 1 : 2;
}
