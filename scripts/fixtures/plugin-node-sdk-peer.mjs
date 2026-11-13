// Real portable SessionChannel + Node NativePort + bounded stdio worker, driven
// by the real C Session fixture. Imported from an owned generated entry module.
import assert from 'node:assert/strict';
import { setImmediate } from 'node:timers/promises';

export async function run({ sdk, NodeNativePort, NodeStdioPipe }) {
  assert.equal(process.env.HOME, undefined);
  assert.equal(process.env.NODE_OPTIONS, undefined);
  const pipe = new NodeStdioPipe({ timeoutMs: 5000 });
  await pipe.ready;
  const port = new NodeNativePort(pipe);
  const session = new sdk.SessionChannel(port);
  const codec = sdk.object({ peer: sdk.boolean() });
  const ask = Object.freeze({ name: 'fixture.ask', input: codec, output: codec });
  const echo = Object.freeze({ name: 'fixture.echo', input: codec, output: codec });
  const done = Object.freeze({ name: 'fixture.done', input: codec, output: codec });
  let handled = false;
  try {
    assert.equal(port.access('ui.form'), 'allowed');
    assert.equal(port.access('ui.chat'), 'unsupported');
    session.register(echo, async (input, context) => {
      context.check(); await Promise.resolve(); context.check();
      assert.equal(input.peer, true); handled = true; return input;
    });
    let first;
    void session.request(ask, { peer: true }, 10000).outcome.then(value => { first = value; });
    for (let step = 0; step < 2000 && (!first || !handled || session.retained); step++) {
      session.step(); await setImmediate();
    }
    assert.equal(first?.kind, 'success'); assert.equal(first.value.peer, true);
    assert(handled); assert.equal(session.retained, 0); assert.equal(port.retained, 0);
    // The Host validates its actual result before answering done; no assumption
    // that a local respond ACK proves the preceding remote result was received.
    let observed;
    void session.request(done, { peer: true }, 10000).outcome.then(value => { observed = value; });
    for (let step = 0; step < 2000 && (!observed || session.retained); step++) {
      session.step(); await setImmediate();
    }
    assert.equal(observed?.kind, 'success'); assert.equal(observed.value.peer, true);
    assert.equal(session.retained, 0); assert.equal(port.retained, 0);
    session.dispose();
    assert.equal(port.access('ui.form'), 'revoked');
  } finally {
    // Native controller still owns process termination and Session cleanup.
    await pipe.close();
  }
}
