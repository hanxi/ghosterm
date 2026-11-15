// Developer-only conformance; real Core/package execution is checked separately.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';
const sdk = await import(pathToFileURL(process.env.GHOSTERM_TEST_PLUGIN_SDK_ENTRY).href);
const { activate, Arguments } = await import(pathToFileURL(process.env.GHOSTERM_DIFF_EXECUTOR).href);
const { activate: client } = await import(pathToFileURL(process.env.GHOSTERM_DIFF_CLIENT).href);
const { lines, compare } = await import(pathToFileURL(process.env.GHOSTERM_DIFF_COMPARE).href);
const mode = process.argv[2] ?? 'all'; let checks = 0;
function check(value, message) { assert(value, message); checks++; }
function rejects(run, message) { assert.throws(run, message); checks++; }
async function rejectsAsync(run, message) { await assert.rejects(run, message); checks++; }
const description = { viewId: 'diff-a', revision: '18446744073709551615', title: 'Compare 😀', beforeLabel: 'Old', afterLabel: 'New',
  beforeLines: 2, afterLines: 2, added: 1, removed: 1, total: 3, pageSize: 3 };
const requested = { viewId: description.viewId, revision: description.revision, offset: 0, limit: 3 };
const received = { viewId: description.viewId, revision: description.revision, offset: 0, total: 3, beforeOffset: 0, afterOffset: 0, rows: [
  { kind: 'equal', beforeLine: 1, afterLine: 1, text: 'same', ending: 'lf' },
  { kind: 'delete', beforeLine: 2, afterLine: 0, text: 'old', ending: 'none' },
  { kind: 'insert', beforeLine: 0, afterLine: 2, text: 'new', ending: 'lf' }] };
function codecs() {
  const p = sdk.validateDiffPage(description, requested, received);
  check(Object.isFrozen(p) && Object.isFrozen(p.rows) && Object.isFrozen(p.rows[0]), 'frozen snapshots');
  const mutable = structuredClone(received), captured = sdk.DiffPageCodec.parse(mutable); mutable.rows[0].text = 'changed';
  check(captured.rows[0].text === 'same', 'input cannot change captured values');
  for (const revision of ['0', '01', '+1', '1\n', '18446744073709551616', 1]) rejects(() => sdk.DiffDescriptorCodec.parse({ ...description, revision }), 'canonical positive uint64');
  for (const viewId of ['', '../x', 'x\n', '😀', 'x'.repeat(65)]) rejects(() => sdk.DiffDescriptorCodec.parse({ ...description, viewId }), 'view identity');
  for (const update of [{ beforeLines: 3 }, { afterLines: 1 }, { total: 2 }, { added: 3 }, { removed: -1 }, { extra: 1 }, { pageSize: 4 }])
    rejects(() => sdk.DiffDescriptorCodec.parse({ ...description, ...update }), 'descriptor consistency');
  for (const update of [{ viewId: 'other' }, { revision: '1' }, { offset: 1 }, { total: 4 }, { rows: received.rows.slice(0, 2) },
    { beforeOffset: 1 }, { afterOffset: 1 }]) rejects(() => sdk.validateDiffPage(description, requested, { ...received, ...update }), 'selected page binding');
  for (const update of [{ kind: 'update' }, { beforeLine: 0 }, { afterLine: 2 }, { ending: 'unknown' }, { text: 'x\ny' },
    { text: 'x\ry' }, { text: 'x\0y' }, { text: 'x'.repeat(129) }, { ending: 'none' }])
    rejects(() => sdk.validateDiffPage(description, requested, { ...received, rows: [{ ...received.rows[0], ...update }, ...received.rows.slice(1)] }), 'row and line consistency');
  rejects(() => sdk.DiffPageCodec.parse({ ...received, rows: [...received.rows, received.rows[2]], total: 4 }), 'three-row page bound');
  rejects(() => sdk.DiffPageCodec.parse({ ...received, rows: [] }), 'no false empty page');
  rejects(() => sdk.validateDiffPage(description, { ...requested, limit: 2 }, received), 'exact requested row count');
  const prior = { ...received, rows: received.rows.slice(0, 1) };
  const next = { ...received, offset: 1, beforeOffset: 1, afterOffset: 1, rows: received.rows.slice(1) };
  check(sdk.validateDiffPage(description, { ...requested, offset: 1, limit: 2 }, next, prior).rows.length === 2, 'sequential cursors');
  rejects(() => sdk.validateDiffPage(description, { ...requested, offset: 1, limit: 2 }, next, { ...prior, revision: '1' }), 'prior revision');
  rejects(() => sdk.validateDiffPage(description, { ...requested, offset: 1, limit: 2 }, next, { ...prior, rows: [] }), 'no skipped page');
  let invoked = false;
  rejects(() => sdk.DiffPageCodec.parse({ ...received, get rows() { invoked = true; return []; } }), 'reject getters');
  check(!invoked, 'getter never invoked');
  const max = { ...description, viewId: 'x'.repeat(64), beforeLines: 3, afterLines: 3, added: 0, removed: 0, total: 3 };
  const maximumPage = { ...received, viewId: max.viewId, rows: Array.from({ length: 3 }, (_, i) => ({
    kind: 'equal', beforeLine: i + 1, afterLine: i + 1, ending: 'crlf', text: '😀'.repeat(128) })) };
  check(sdk.utf8Bytes(sdk.encode(sdk.validateDiffPage(max, { ...requested, viewId: max.viewId }, maximumPage))) <= 2048, 'worst-case valid page fits wire budget');
  check(sdk.DiffDescriptorCodec.parse({ ...description, title: '😀'.repeat(128) }).title.length === 256, 'Unicode code point bound');
}
const ending = Object.freeze({ lf: '\n', crlf: '\r\n', cr: '\r', none: '' });
function reconstruct(rows, side) { return rows.filter(r => r.kind !== (side === 'before' ? 'insert' : 'delete')).map(r => r.text + ending[r.ending]).join(''); }
function subsequences(values) {
  let result = ['']; for (const value of values) result = [...result, ...result.map(prefix => prefix + value)]; return new Set(result);
}
function algorithm() {
  const sequences = ['']; for (let n = 1; n <= 4; n++) for (let mask = 0; mask < 2 ** n; mask++)
    sequences.push(Array.from({ length: n }, (_, i) => mask & (1 << i) ? 'a' : 'b').join(''));
  for (const before of sequences) for (const after of sequences) {
    const old = [...before].map(c => ({ text: c, ending: 'lf' })), next = [...after].map(c => ({ text: c, ending: 'lf' }));
    const rows = compare(old, next, () => {}), common = subsequences(before), other = subsequences(after);
    const longest = Math.max(...[...common].filter(s => other.has(s)).map(s => s.length));
    check(rows.filter(r => r.kind !== 'equal').length === before.length + after.length - 2 * longest, 'exhaustive minimum insertion/deletion count');
    check(reconstruct(rows, 'before') === [...before].map(c => c + '\n').join('') && reconstruct(rows, 'after') === [...after].map(c => c + '\n').join(''), 'exact input reconstruction');
  }
  for (const [before, after] of [['a\r\nb\rc\nlast', 'a\nb\rc\r\nlast\n'], ['é\n😀', 'e\u0301\n😀\n'], ['', '\n'], ['\n\n', '\n'], ['\t\\"\n', '\t\\"']]) {
    const rows = compare(lines(JSON.stringify(before)), lines(JSON.stringify(after)), () => {});
    check(reconstruct(rows, 'before') === before && reconstruct(rows, 'after') === after, 'exact Unicode and line terminators');
  }
  assert.deepEqual(compare(lines('"a\\nb\\n"'), lines('"b\\na\\n"'), () => {}).map(r => r.kind), ['delete', 'equal', 'insert']); checks++;
  for (const input of ['{}', 'null', '1', '"\\ud800"', JSON.stringify('x'.repeat(129)), JSON.stringify('\n'.repeat(129)), JSON.stringify('x\0y'), '"' + 'x'.repeat(3072) + '"'])
    rejects(() => lines(input), 'bounded JSON text inputs');
  check(lines(JSON.stringify('\n'.repeat(128))).length === 128 && lines('""').length === 0, 'line boundary and empty input');
  let calls = 0;
  rejects(() => compare(lines(JSON.stringify('a\n'.repeat(128))), lines(JSON.stringify('b\n'.repeat(128))), () => { if (++calls === 2) throw Error('cancelled'); }), 'bounded cooperative cancellation');
}
function fixture(before, after, labels = [], pump = true) {
  const handlers = new Map(); let allowed = true, cancelled = false;
  const context = { side: 'executor', instanceId: 'fixture-diff', epoch: '9',
    invocation: { command: 'diff', arguments: ['--before', JSON.stringify(before), '--after', JSON.stringify(after), ...labels] },
    capabilities: { query: name => allowed && name === 'session.rpc' ? 'allowed' : 'denied' },
    cancellation: { get cancelled() { return cancelled; } }, session: { register(method, handler) {
      assert(!handlers.has(method.name)); handlers.set(method.name, async input => method.output.parse(await handler(method.input.parse(input), { check() { if (cancelled) throw Error('cancelled'); } })));
    } } };
  const disposable = activate(context);
  if (pump) for (let i = 0; i < 200; i++) disposable.step();
  return { handlers, disposable, revoke() { allowed = false; }, cancel() { cancelled = true; } };
}
async function lifecycle() {
  const pending = fixture('a\n'.repeat(128), 'b\n'.repeat(128), [], false);
  let completed = false;
  const read = pending.handlers.get('view.diff.describe')({}).then(value => { completed = true; return value; });
  await Promise.resolve(); check(!completed, 'pending preparation cannot pretend to be an empty comparison');
  for (let i = 0; i < 200; i++) pending.disposable.step();
  await new Promise(resolve => setImmediate(resolve));
  check(completed, 'worker hook must finish within the bounded fixture pumps');
  check((await read).total === 256, 'bounded Native worker hook completes preparation');
  const closed = fixture('a', 'b', [], false), waiting = closed.handlers.get('view.diff.describe')({});
  closed.disposable.dispose(); await rejectsAsync(() => waiting, 'dispose wakes pending queries without exposing partial data');
}
async function executor() {
  for (const [before, after] of [['', ''], ['a\r\nb\nlast', 'a\r\nc\nlast\n'], ['a\n'.repeat(128), 'b\n'.repeat(128)], ['😀'.repeat(128) + '\n'.repeat(2), '😀'.repeat(128) + '\n'.repeat(2)]]) {
    const f = fixture(before, after), d = await f.handlers.get('view.diff.describe')({}), all = [];
    let previous;
    do {
      const requested = { viewId: d.viewId, revision: d.revision, offset: all.length, limit: d.pageSize };
      const p = sdk.validateDiffPage(d, requested, await f.handlers.get('view.diff.page')(requested), previous);
      check(sdk.utf8Bytes(sdk.encode(p)) <= 2048, 'actual bounded output codec'); all.push(...p.rows); previous = p;
    } while (all.length < d.total);
    check(reconstruct(all, 'before') === before && reconstruct(all, 'after') === after, 'all actual handler pages reconstruct original sources');
    for (const change of [{ viewId: 'other' }, { revision: '8' }, { offset: d.total + 1 }, { limit: 4 }])
      await rejectsAsync(() => f.handlers.get('view.diff.page')({ viewId: d.viewId, revision: d.revision, offset: 0, limit: 3, ...change }), 'stale/invalid requests');
    f.disposable.dispose(); await rejectsAsync(() => f.handlers.get('view.diff.describe')({}), 'dispose revokes queries');
  }
  for (const action of ['revoke', 'cancel']) { const f = fixture('a', 'b'); f[action](); await rejectsAsync(() => f.handlers.get('view.diff.describe')({}), 'live capability/cancellation'); }
  const d = await fixture('a', 'b', ['--before-label', '甲', '--after-label', '乙']).handlers.get('view.diff.describe')({});
  check(d.beforeLabel === '甲' && d.afterLabel === '乙', 'captured display labels');
  rejects(() => client({ side: 'client', invocation: { command: 'diff' }, capabilities: { query() { return 'unsupported'; } } }), 'missing renderer explicit');
  assert.deepEqual(JSON.parse(JSON.stringify(Arguments.schema)), JSON.parse(fs.readFileSync(new URL('../plugins/official/diff/schemas/diff.json', import.meta.url)))); checks++;
}
if (mode === 'all' || mode === 'codec') codecs();
if (mode === 'all' || mode === 'algorithm') algorithm();
if (mode === 'all' || mode === 'executor') await executor();
if (mode === 'all' || mode === 'lifecycle') await lifecycle();
console.log('DIFF CONFORMANCE PASS: ' + checks + ' checks / ' + mode);
