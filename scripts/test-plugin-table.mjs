// Developer conformance only. Core/QuickJS execution is verified separately.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { pathToFileURL } from 'node:url';
const sdk = await import(pathToFileURL(process.env.GHOSTERM_TEST_PLUGIN_SDK_ENTRY).href);
const { activate, Arguments } = await import(pathToFileURL(process.env.GHOSTERM_TABLE_EXECUTOR).href);
const { activate: client } = await import(pathToFileURL(process.env.GHOSTERM_TABLE_CLIENT).href);
const mode = process.argv[2] ?? 'all';
let checks = 0;
function check(value, message) { assert(value, message); checks++; }
function rejects(run, message) { assert.throws(run, message); checks++; }
const description = { viewId: 'table-a', revision: '18446744073709551615', title: '表格😀',
  columns: [{ id: 'n', title: '数字' }], total: 3, pageSize: 2 };
const requested = { viewId: 'table-a', revision: description.revision, offset: 0, limit: 2 };
const received = { viewId: 'table-a', revision: description.revision, offset: 0, total: 3,
  rows: [{ id: '1', cells: ['一'] }, { id: '2', cells: ['二'] }] };
function codecs() {
  const d = sdk.TableDescriptorCodec.parse(description), r = sdk.TablePageRequestCodec.parse(requested);
  const p = sdk.validateTablePage(d, r, received);
  check(Object.isFrozen(p) && Object.isFrozen(p.rows) && Object.isFrozen(p.rows[0].cells), 'immutable captured values');
  const mutable = structuredClone(received), captured = sdk.TablePageCodec.parse(mutable);
  mutable.rows[0].cells[0] = 'changed'; check(captured.rows[0].cells[0] === '一', 'copy before mutation');
  for (const revision of ['0', '01', '1\n', '1\r', '1\u2028', '18446744073709551616', 1])
    rejects(() => sdk.TableDescriptorCodec.parse({ ...description, revision }), 'canonical exact revision');
  for (const viewId of ['', 'table\n', '../path', '😀', 'x'.repeat(65)])
    rejects(() => sdk.TableDescriptorCodec.parse({ ...description, viewId }), 'bounded identifier');
  for (const columns of [[], Array(9).fill(description.columns[0]), [description.columns[0], description.columns[0]]])
    rejects(() => sdk.TableDescriptorCodec.parse({ ...description, columns }), 'column bound and uniqueness');
  for (const title of ['', 'a\0b', 'x'.repeat(129)]) rejects(() => sdk.TableDescriptorCodec.parse({ ...description, title }), 'text bound');
  for (const limit of [0, 17, 1.5, '2']) rejects(() => sdk.TablePageRequestCodec.parse({ ...requested, limit }), 'request page bound');
  rejects(() => sdk.TablePageCodec.parse({ ...received, rows: Array.from({ length: 17 }, (_, i) => ({ id: String(i), cells: [''] })), total: 20 }), 'response page bound');
  rejects(() => sdk.TablePageCodec.parse({ ...received, rows: [received.rows[0], received.rows[0]] }), 'unique row ids');
  rejects(() => sdk.TablePageCodec.parse({ ...received, rows: [] }), 'no false empty page before end');
  rejects(() => sdk.TablePageCodec.parse({ ...received, extra: true }), 'unknown fields');
  for (const update of [{ viewId: 'other' }, { revision: '1' }, { offset: 1 }, { total: 4 }, { rows: [received.rows[0]] },
    { rows: [{ id: '1', cells: ['a', 'b'] }, received.rows[1]] }])
    rejects(() => sdk.validateTablePage(description, requested, { ...received, ...update }), 'selected page consistency');
  rejects(() => sdk.validateTablePage(description, { ...requested, limit: 3 }, received), 'descriptor page size');
  rejects(() => sdk.TablePageCodec.parse({ ...received, total: 4, rows: Array.from({ length: 4 }, (_, i) => ({ id: String(i), cells: ['😀'.repeat(160)] })) }), 'aggregate encoded bytes');
  let invoked = false;
  rejects(() => sdk.TablePageCodec.parse({ ...received, get rows() { invoked = true; return []; } }), 'no accessor execution');
  check(!invoked, 'accessor remains untouched');
}
function fixture(count = 1000000) {
  const handlers = new Map(); let allowed = true, cancelled = false;
  const context = { side: 'executor', instanceId: 'fixture-table', epoch: '9',
    invocation: { command: 'table', arguments: ['--count', String(count)] },
    capabilities: { query: name => allowed && name === 'session.rpc' ? 'allowed' : 'denied' },
    cancellation: { get cancelled() { return cancelled; } },
    session: { register(method, handler) { assert(!handlers.has(method.name)); handlers.set(method.name,
      input => method.output.parse(handler(method.input.parse(input), { check() { if (cancelled) throw Error('cancelled'); } }))); } } };
  const disposable = activate(context);
  return { context, handlers, disposable, revoke() { allowed = false; }, cancel() { cancelled = true; } };
}
function executor() {
  const f = fixture(), d = f.handlers.get(sdk.TableDescribeMethod.name)({});
  check(d.total === 1000000 && d.pageSize === 16 && d.columns.length === 3, 'computed descriptor');
  const page = f.handlers.get(sdk.TablePageMethod.name)({ viewId: d.viewId, revision: d.revision, offset: 999984, limit: 16 });
  check(page.rows.length === 16 && page.rows[15].cells.join(',') === '1000000,1000000000000,1000000000000000000', 'exact large integers');
  check(sdk.utf8Bytes(sdk.encode(page)) <= sdk.TABLE_BODY_BYTES_MAX, 'wire-compatible bounded page');
  const base = { viewId: d.viewId, revision: d.revision, offset: 0, limit: 16 };
  for (const update of [{ viewId: 'other' }, { revision: '8' }, { offset: 1000001 }, { limit: 17 }])
    rejects(() => f.handlers.get(sdk.TablePageMethod.name)({ ...base, ...update }), 'stale/invalid query');
  f.disposable.dispose(); rejects(() => f.handlers.get(sdk.TableDescribeMethod.name)({}), 'dispose revokes read access');
  const revoked = fixture(); revoked.revoke(); rejects(() => revoked.handlers.get(sdk.TableDescribeMethod.name)({}), 'live capability recheck');
  const stopped = fixture(); stopped.cancel(); rejects(() => stopped.handlers.get(sdk.TableDescribeMethod.name)({}), 'cancel check');
  const empty = fixture(0), ed = empty.handlers.get(sdk.TableDescribeMethod.name)({});
  check(empty.handlers.get(sdk.TablePageMethod.name)({ viewId: ed.viewId, revision: ed.revision, offset: 0, limit: 16 }).rows.length === 0, 'empty dataset');
  check(Arguments.parse([]).count === 1000 && Arguments.parse(['--count=0']).count === 0, 'argv defaults and explicit zero');
  for (const args of [['--count', '-1'], ['--count', '1000001'], ['--count=1', '--count=2'], ['--unknown']]) rejects(() => Arguments.parse(args), 'CLI mapping');
  assert.deepEqual(JSON.parse(JSON.stringify(Arguments.schema)), JSON.parse(fs.readFileSync(new URL('../plugins/official/table/schemas/table.json', import.meta.url)))); checks++;
  rejects(() => client({ side: 'client', invocation: { command: 'table' }, capabilities: { query() { return 'unsupported'; } } }), 'no silent unsupported renderer');
}
if (mode === 'all' || mode === 'codec') codecs();
if (mode === 'all' || mode === 'executor') executor();
console.log('TABLE CONFORMANCE PASS: ' + checks + ' checks / ' + mode);
