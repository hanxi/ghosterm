// Developer conformance; Native public package/Session execution is separate.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const sdk = await import(pathToFileURL(process.env.GHOSTERM_TEST_PLUGIN_SDK_ENTRY).href);
const { activate, Arguments } = await import(pathToFileURL(process.env.GHOSTERM_TASK_EXECUTOR).href);
const { activate: client } = await import(pathToFileURL(process.env.GHOSTERM_TASK_CLIENT).href);
const { PrimeTask } = await import(pathToFileURL(process.env.GHOSTERM_TASK_ENGINE).href);
const mode = process.argv[2] ?? 'all'; let checks = 0;
function check(value, label) { assert(value, label); checks++; }
function rejects(run, label) { assert.throws(run, label); checks++; }
async function rejectsAsync(run, label) { await assert.rejects(run, label); checks++; }
const description = { viewId: 'task-a', revision: '18446744073709551615', title: 'Count primes 😀', unit: 'candidates', cancellable: true };
const request = { viewId: description.viewId, revision: description.revision };
const base = { ...request, sequence: '9007199254740993', phase: 'running', completed: '2', total: { kind: 'known', units: '10' }, message: 'Working', result: { kind: 'none' } };
const cancel = { ...request, requestId: 'cancel-1' };
const ack = { ...cancel, kind: 'requested' };
function codecs() {
  const value = sdk.validateTaskSnapshot(description, request, base);
  check(Object.isFrozen(value) && Object.isFrozen(value.total) && Object.isFrozen(value.result), 'deep immutable observations');
  const input = structuredClone(base), captured = sdk.TaskSnapshotCodec.parse(input); input.total.units = '20';
  check(captured.total.units === '10', 'captured values independent of caller');
  for (const revision of ['0', '01', '+1', '1\n', '18446744073709551616', 1]) rejects(() => sdk.TaskDescriptorCodec.parse({ ...description, revision }), 'canonical positive revision');
  for (const id of ['', '../a', 'x\n', '😀', 'a'.repeat(65)]) rejects(() => sdk.TaskDescriptorCodec.parse({ ...description, viewId: id }), 'view identity');
  for (const patch of [{ title: '' }, { unit: '' }, { cancellable: 1 }, { extra: true }, { title: 'a\0b' }, { title: 'e\u0301'.repeat(65) }])
    rejects(() => sdk.TaskDescriptorCodec.parse({ ...description, ...patch }), 'descriptor value boundaries');
  check(sdk.TaskDescriptorCodec.parse({ ...description, title: '😀'.repeat(128) }).title.length === 256, 'Unicode scalar count');
  for (const completed of ['-1', '00', '1\n', '18446744073709551616', 1]) rejects(() => sdk.TaskSnapshotCodec.parse({ ...base, completed }), 'exact progress counter');
  for (const sequence of ['0', '01', '18446744073709551616', 1]) rejects(() => sdk.TaskSnapshotCodec.parse({ ...base, sequence }), 'positive sequence');
  for (const patch of [{ completed: '11' }, { phase: 'queued' }, { phase: 'succeeded' }, { phase: 'failed' }, { phase: 'cancelled' }, { phase: 'unknown' }, { result: { kind: 'success', summary: 'Not a running result' } }, { total: { kind: 'known' } }, { total: { kind: 'unknown', units: '0' } }, { extra: true }])
    rejects(() => sdk.TaskSnapshotCodec.parse({ ...base, ...patch }), 'result and progress cannot fabricate success');
  const unknownSize = sdk.TaskSnapshotCodec.parse({ ...base, total: { kind: 'unknown' } });
  const knownZero = sdk.TaskSnapshotCodec.parse({ ...base, completed: '0', total: { kind: 'known', units: '0' } });
  check(unknownSize.total.kind === 'unknown' && knownZero.total.kind === 'known', 'unknown total distinct from zero');
  const unknown = { ...base, phase: 'unknown', result: { kind: 'unknown', reason: 'Outcome not confirmed' } };
  check(sdk.TaskSnapshotCodec.parse(unknown).result.kind === 'unknown', 'business uncertainty retained');
  const success = { ...base, phase: 'succeeded', completed: '10', result: { kind: 'success', summary: '4 primes' } };
  check(sdk.TaskSnapshotCodec.parse(success).result.kind === 'success', 'confirmed final result');
  rejects(() => sdk.TaskSnapshotCodec.parse({ ...success, completed: '9' }), 'no incomplete known success');
  for (const effect of ['none', 'possible']) check(sdk.TaskSnapshotCodec.parse({ ...base, phase: 'cancelled', result: { kind: 'cancelled', effect } }).result.effect === effect, 'cancel effect retained');
  check(sdk.TaskSnapshotCodec.parse({ ...base, phase: 'failed', result: { kind: 'failed', code: 'IO', message: 'Error observed' } }).phase === 'failed', 'known failure separate');
  rejects(() => sdk.TaskSnapshotCodec.parse({ ...base, viewId: 'x'.repeat(64), message: '😀'.repeat(128), phase: 'failed', result: { kind: 'failed', code: '😀'.repeat(64), message: '😀'.repeat(256) } }), 'aggregate UTF-8 limit');
  for (const patch of [{ viewId: 'other' }, { revision: '1' }]) {
    rejects(() => sdk.validateTaskSnapshot(description, request, { ...base, ...patch }), 'snapshot scope');
    rejects(() => sdk.validateTaskSnapshot(description, { ...request, ...patch }, base), 'request scope');
  }
  check(sdk.validateTaskSnapshot(description, request, { ...base, sequence: '9007199254740994', completed: '3' }, base).completed === '3', 'monotonic progress above Number precision');
  check(sdk.validateTaskSnapshot(description, request, base, base).sequence === base.sequence, 'identical repeat observation');
  for (const patch of [{ sequence: '9007199254740992' }, { message: 'Changed under same sequence' }, { sequence: '9007199254740994', completed: '1' }, { sequence: '9007199254740994', total: { kind: 'unknown' } }, { sequence: '9007199254740994', total: { kind: 'known', units: '11' } }])
    rejects(() => sdk.validateTaskSnapshot(description, request, { ...base, ...patch }, base), 'observation rollback or total rewrite rejected');
  rejects(() => sdk.validateTaskSnapshot(description, request, { ...success, sequence: '9007199254740994' }, success), 'confirmed terminal observation immutable');
  rejects(() => sdk.validateTaskSnapshot(description, request, { ...base, completed: '0', sequence: '9007199254740994', phase: 'queued' }, { ...base, completed: '0' }), 'running task cannot requeue');
  const cancelling = { ...base, phase: 'cancelling' };
  rejects(() => sdk.validateTaskSnapshot(description, request, { ...base, sequence: '9007199254740994' }, cancelling), 'cancellation cannot regress to running');
  rejects(() => sdk.validateTaskSnapshot({ ...description, cancellable: false }, request, cancelling), 'unsupported cancellation is explicit');
  check(sdk.validateTaskCancelAck(description, cancel, ack).kind === 'requested', 'cancel acknowledgement binds request identity');
  check(sdk.validateTaskCancelAck(description, cancel, { ...ack, kind: 'alreadyFinished' }).kind === 'alreadyFinished', 'late cancellation separate from stopped');
  for (const patch of [{ requestId: 'other' }, { revision: '1' }, { viewId: 'other' }, { kind: 'cancelled' }, { extra: true }])
    rejects(() => sdk.validateTaskCancelAck(description, cancel, { ...ack, ...patch }), 'invalid acknowledgement rejected');
  rejects(() => sdk.validateTaskCancelAck({ ...description, cancellable: false }, cancel, ack), 'no cancellation grant through acknowledgement');
  let invoked = false;
  rejects(() => sdk.TaskSnapshotCodec.parse({ ...base, get total() { invoked = true; return base.total; } }), 'reject accessor');
  check(!invoked, 'never invoke untrusted getter');
}
function oracle(limit) {
  let count = 0;
  for (let n = 2; n <= limit; n++) {
    let prime = true;
    for (let divisor = 2; divisor * divisor <= n; divisor++) if (n % divisor === 0) { prime = false; break; }
    if (prime) count++;
  }
  return count;
}
function complete(limit) {
  const task = new PrimeTask(limit); let old = task.snapshot(), pumps = 0;
  while (old.phase !== 'succeeded' && pumps++ < 10000) {
    task.step(); const value = task.snapshot();
    check(BigInt(value.sequence) > BigInt(old.sequence) && BigInt(value.completed) >= BigInt(old.completed) &&
      BigInt(value.completed) - BigInt(old.completed) <= 512n, 'bounded worker work and exact monotonic progress'); old = value;
  }
  check(old.phase === 'succeeded' && old.completed === String(Math.max(0, limit - 1)) && old.result.summary === `${oracle(limit)} primes up to ${limit}`, 'independent trial-division prime oracle');
  task.step(); check(JSON.stringify(task.snapshot()) === JSON.stringify(old), 'terminal computation never advances'); task.dispose();
}
function algorithm() {
  for (let limit = 0; limit <= 256; limit++) complete(limit);
  for (const limit of [1000, 10000, 100000, 1000000]) complete(limit);
  for (const limit of [-1, 1.5, 1000001, NaN, Infinity]) rejects(() => new PrimeTask(limit), 'bounded computation input');
}
function fixture(limit = 1000000) {
  const handlers = new Map(); let allowed = true, cancelled = false, requestCancelled = false;
  const context = { side: 'executor', instanceId: description.viewId, epoch: description.revision,
    invocation: { command: 'task primes', arguments: ['--limit', String(limit)] },
    capabilities: { query: name => allowed && name === 'session.rpc' ? 'allowed' : 'denied' },
    cancellation: { get cancelled() { return cancelled; } }, session: { register(method, handler) {
      assert(!handlers.has(method.name)); handlers.set(method.name, async input => method.output.parse(await handler(method.input.parse(input), { check() { if (requestCancelled) throw Error('request cancelled'); } })));
    } } };
  const disposable = activate(context);
  return { disposable, context, read: () => handlers.get('view.task.read')(request), describe: () => handlers.get('view.task.describe')({}),
    cancel: id => handlers.get('view.task.cancel')({ ...request, requestId: id }), handlers,
    revoke() { allowed = false; }, cancelInstance() { cancelled = true; }, cancelRequest() { requestCancelled = true; } };
}
async function executor() {
  const packageRoot = path.resolve(path.dirname(process.env.GHOSTERM_TASK_EXECUTOR), '..');
  assert.deepEqual(JSON.parse(fs.readFileSync(path.join(packageRoot, 'schemas/primes.json'), 'utf8')), JSON.parse(JSON.stringify(Arguments.schema))); checks++;
  check(Arguments.parse([]).limit === 1000000 && Arguments.parse(['--limit', '0']).limit === 0, 'real args schema defaults');
  for (const args of [['--limit', '1000001'], ['--limit', '-1'], ['--limit', '01'], ['--limit', '1.5'], ['--unknown']]) rejects(() => Arguments.parse(args), 'argument bounds');
  const f = fixture(); check((await f.read()).phase === 'queued', 'activation does not pretend work completed');
  check((await f.describe()).cancellable, 'Native descriptor reports actual cancellation service');
  f.disposable.step(); const before = await f.read(); check(before.phase === 'running' && before.completed === '1', 'real first sieve slice');
  const first = await f.cancel('cancel-1'); check(first.kind === 'requested' && (await f.read()).phase === 'cancelling', 'acknowledged request is not stopped');
  check(JSON.stringify(await f.cancel('cancel-1')) === JSON.stringify(first), 'same cancellation identity idempotent');
  await rejectsAsync(() => f.cancel('cancel-2'), 'different cancellation identity never repeats an action');
  f.disposable.step(); const stopped = await f.read(); check(stopped.phase === 'cancelled' && stopped.result.effect === 'none' && stopped.completed === before.completed, 'worker confirms stop before further computation');
  f.disposable.step(); check(JSON.stringify(await f.read()) === JSON.stringify(stopped), 'cancelled task stable'); f.disposable.dispose();
  const late = fixture(100); for (let i = 0; i < 200; i++) late.disposable.step(); const done = await late.read();
  check(done.result.summary === '25 primes up to 100' && (await late.cancel('late-1')).kind === 'alreadyFinished', 'late cancel preserves independently verified success');
  check(JSON.stringify(await late.read()) === JSON.stringify(done), 'late cancellation never overwrites result'); late.disposable.dispose();
  const stale = fixture(); await rejectsAsync(() => stale.handlers.get('view.task.read')({ ...request, revision: '1' }), 'stale task query');
  await rejectsAsync(() => stale.handlers.get('view.task.cancel')({ ...cancel, viewId: 'other' }), 'stale task cancellation'); stale.disposable.dispose();
}
async function lifecycle() {
  for (const operation of ['revoke', 'cancelInstance', 'cancelRequest']) {
    const f = fixture(); f.disposable.step(); f[operation]();
    await rejectsAsync(() => f.cancel('cancel-1'), 'revoked or cancelled request cannot effect task');
    if (operation !== 'cancelRequest') await rejectsAsync(() => f.read(), 'revoked instance cannot supply observations');
    f.disposable.dispose();
  }
  const f = fixture(); const captured = await f.read(); f.disposable.dispose();
  await rejectsAsync(() => f.describe(), 'dispose revokes current descriptor');
  await rejectsAsync(() => f.read(), 'dispose revokes reads, not a fabricated cancelled result');
  await rejectsAsync(() => f.cancel('cancel-1'), 'dispose revokes cancellation'); f.disposable.step();
  check(captured.phase === 'queued' && captured.result.kind === 'none', 'historical observation remains unchanged');
  const job = new PrimeTask(1000000); job.step(); job.cancel(); const pending = job.snapshot(); job.dispose();
  check(pending.phase === 'cancelling', 'pending cancellation stays unconfirmed on disposal'); rejects(() => job.snapshot(), 'no invented disposal result');
  rejects(() => client({ ...f.context, side: 'client' }), 'missing Native task service explicit');
  client({ ...f.context, side: 'client', capabilities: { query: name => name === 'ui.task' ? 'allowed' : 'unsupported' } }).dispose(); checks++;
}
async function adapter() {
  const f = fixture(); f.disposable.step(); let next = 0, requests = 0; const results = new Map();
  const port = { request(method, json) {
    requests++; const handle = String(++next); results.set(handle, { state: 'pending', confirmed: false, outcome: '' });
    void f.handlers.get(method)(JSON.parse(json)).then(value => {
      const lost = method === 'view.task.cancel';
      results.set(handle, lost ? { state: 'unknown', confirmed: false, outcome: '{"kind":"unknown","reason":"LOST_ACK"}' } :
        { state: 'succeeded', confirmed: true, outcome: JSON.stringify({ kind: 'success', value }) });
    });
    return { status: 'ok', handle };
  }, result: h => results.get(h), cancel: () => 'ok', forget: h => { results.delete(h); return 'ok'; },
    take: () => null, check: () => 'ok', respond: () => 'ok', revoke() {} };
  const channel = new sdk.SessionChannel(port);
  async function settle(call) { for (let i = 0; i < 20; i++) { channel.step(); await Promise.resolve(); } return await call.outcome; }
  const uncertain = await settle(channel.request(sdk.TaskCancelMethod, cancel, 1000));
  check(uncertain.kind === 'unknown' && requests === 1, 'lost cancellation ack is not denied or confirmed stopped');
  for (let i = 0; i < 20; i++) channel.step(); check(requests === 1, 'no automatic action replay');
  f.disposable.step(); const observed = await settle(channel.request(sdk.TaskReadMethod, request, 1000));
  check(observed.kind === 'success' && observed.value.phase === 'cancelled' && requests === 2, 'explicit read observes confirmed stop after lost ack without replay');
  channel.dispose(); f.disposable.dispose();
}
if (mode === 'all' || mode === 'codec') codecs();
if (mode === 'all' || mode === 'algorithm') algorithm();
if (mode === 'all' || mode === 'executor') await executor();
if (mode === 'all' || mode === 'lifecycle') await lifecycle();
if (mode === 'all' || mode === 'adapter') await adapter();
console.log(`TASK CONFORMANCE PASS: ${checks} checks / ${mode}`);
