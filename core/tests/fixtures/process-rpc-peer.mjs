// Disposable real Node stdio peer; no model, user account, network or OS effect.
import assert from 'node:assert/strict';
assert.equal(process.env.HOME, undefined);
assert.equal(process.env.NODE_OPTIONS, undefined);
let sequence = 0n, buffer = Buffer.alloc(0), resolveReply;
process.stdin.on('data', chunk => {
  buffer = Buffer.concat([buffer, chunk]);
  assert(buffer.length <= 8196);
  if (buffer.length < 4) return;
  const n = buffer.readUInt32BE(0);
  assert(n > 0 && n <= 8192);
  if (buffer.length < n + 4) return;
  assert(resolveReply);
  const reply = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(buffer.subarray(4, n + 4)));
  buffer = buffer.subarray(n + 4);
  const resolve = resolveReply; resolveReply = undefined; resolve(reply);
});
process.stdin.on('end', () => { if (resolveReply) throw Error('reply truncated'); });
async function call(op, fields = {}) {
  assert(!resolveReply);
  const id = String(++sequence);
  const body = Buffer.from(JSON.stringify({ v: 1, id, op, ...fields }));
  assert(body.length <= 8192);
  const header = Buffer.alloc(4); header.writeUInt32BE(body.length);
  const pending = new Promise(resolve => { resolveReply = resolve; });
  process.stdout.write(Buffer.concat([header, body]));
  const reply = await pending;
  assert.equal(reply.v, 1); assert.equal(reply.id, id);
  return reply;
}
const start = await call('request', { method: 'fixture.ask', input: { peer: true }, timeoutMs: 10000 });
assert.equal(start.status, 'ok'); assert.equal(start.handle, '1');
let result;
do { result = await call('result', { ref: start.handle }); } while (['pending', 'queued'].includes(result.state));
assert.equal(result.state, 'succeeded'); assert.equal(result.confirmed, true);
assert.deepEqual(result.outcome, { kind: 'success', value: { peer: true } });
assert.equal((await call('forget', { ref: start.handle })).status, 'ok');
let incoming;
do { incoming = await call('take', { methods: ['fixture.echo'] }); } while (incoming.status === 'wait');
assert.equal(incoming.status, 'ok'); assert.equal(incoming.ticket, '2');
assert.deepEqual(incoming.input, { peer: true });
assert.equal((await call('check', { ref: incoming.ticket })).status, 'ok');
assert.equal((await call('respond', { ref: incoming.ticket, outcome: { kind: 'success', value: incoming.input } })).status, 'ok');
process.stdin.destroy();
