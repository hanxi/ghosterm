// Portable codec / local adapter conformance; Native behavior is a separate CTest.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
import path from 'node:path';
const [sdkRoot, tools] = process.argv.slice(2);
const { ViewRegistry, ViewSpecCodec } = await import(pathToFileURL(path.resolve(sdkRoot, 'views.js')));
const require = createRequire(path.resolve(tools, 'package.json'));
const Ajv = require('ajv/dist/2020').default;
assert.equal(require('ajv/package.json').version, '8.17.1');
let checks = 0;
function check(value, label) { assert(value, label); checks++; }
function rejects(callback, code) { assert.throws(callback, e => e.code === code); checks++; }
const table = { kind: 'table', viewId: 'a', methods: { describe: 'table.describe', read: 'table.page' } };
const diff = { kind: 'diff', viewId: 'b', methods: { describe: 'diff.describe', read: 'diff.page' } };
const task = { kind: 'task', viewId: 'c', methods: { describe: 'task.describe', read: 'task.read', cancel: 'task.cancel' } };
const schema = JSON.parse(fs.readFileSync(new URL('../packages/plugin-sdk/schema/views-v1.candidate.json', import.meta.url)));
const { $schema, $id, ...body } = schema;
assert($schema && $id); assert.deepEqual(JSON.parse(JSON.stringify(ViewSpecCodec.schema)), body); checks++;
const validate = new Ajv({ strict: true }).compile(schema);
const corpus = [table, diff, task, {}, null, { ...table, kind: 'chat' }, { ...table, kind: {} }, { ...table, viewId: '' },
  { ...table, viewId: 'x\n' }, { ...table, viewId: '😀' }, { ...table, viewId: 'x'.repeat(65) },
  { ...table, extra: true }, { ...table, methods: { ...table.methods, cancel: 'task.cancel' } },
  { ...task, methods: table.methods }, { ...table, methods: { describe: 'Node.describe', read: 'table.page' } },
  { ...table, methods: { describe: 'a'.repeat(129), read: 'table.page' } }];
for (const item of corpus) {
  let accepted = true; try { ViewSpecCodec.parse(item); } catch { accepted = false; }
  check(accepted === validate(item), 'independent schema parity');
}
rejects(() => ViewSpecCodec.parse({ ...table, methods: { describe: 'a', read: 'a' } }), 'VIEW_METHODS');
let invoked = false;
assert.throws(() => ViewSpecCodec.parse({ ...table, get viewId() { invoked = true; return 'a'; } }));
check(!invoked, 'no accessor evaluation');
const mutable = structuredClone(table), frozen = ViewSpecCodec.parse(mutable);
mutable.methods.read = 'changed'; check(frozen.methods.read === 'table.page' && Object.isFrozen(frozen.methods), 'capture and freeze bindings');
function fixture() {
  let next = 1, admission = 'ok', observation = 'ok', closeFailure = false;
  const entries = new Map();
  let opened = 0, closed = 0;
  const port = { viewOpen(_kind, _id, _describe, _read, _cancel) {
    opened++; if (admission !== 'ok') return { status: admission };
    const handle = 'a'.repeat(32) + '.' + next++;
    entries.set(handle, true); return { status: 'ok', handle };
  }, viewState(handle) { return entries.has(handle) ? observation : 'closed'; },
  viewClose(handle) { closed++; if (closeFailure) throw Error('fixture close'); const existed = entries.delete(handle); return existed ? 'ok' : 'closed'; } };
  return { port, entries, registry: new ViewRegistry(port), reject(value) { admission = value; }, observe(value) { observation = value; },
    failClose() { closeFailure = true; }, get opened() { return opened; }, get closed() { return closed; } };
}
const f = fixture();
const a = f.registry.open(table), b = f.registry.open(diff), c = f.registry.open(task);
check(f.registry.retained === 3 && new Set([a.handle, b.handle, c.handle]).size === 3, 'independent view handles');
check(Object.isFrozen(a) && a.state === 'ok', 'immutable live handle');
rejects(() => f.registry.open(table), 'VIEW_DUPLICATE'); check(f.opened === 3, 'duplicate rejected before Native call');
a.close(); a.close(); check(f.closed === 1 && a.state === 'closed' && f.registry.retained === 2, 'idempotent local close');
const reopened = f.registry.open(table); check(reopened.handle !== a.handle, 'no handle reuse');
// A host close followed by reopening the same ID must not let the old JS handle
// retire the new registration when its delayed close callback runs.
f.entries.delete(reopened.handle);
const replacement = f.registry.open(table);
reopened.close();
check(f.registry.retained === 3 && replacement.state === 'ok', 'late old close cannot retire replacement');
for (const state of ['unknown', 'denied', 'revoked', 'unsupported']) {
  f.observe(state); check(replacement.state === state, 'do not collapse Native state');
  const rejected = fixture(); rejected.reject(state); rejects(() => rejected.registry.open(table), 'VIEW_' + state.toUpperCase());
  check(rejected.registry.retained === 0, 'failed admission retains no JS handle');
}
f.registry.dispose(); f.registry.dispose();
check(f.registry.retained === 0 && replacement.state === 'closed', 'dispose invalidates all handles');
rejects(() => f.registry.open(table), 'VIEW_CLOSED');
const full = fixture();
for (let i = 0; i < 8; i++) full.registry.open({ ...table, viewId: 'v' + i });
rejects(() => full.registry.open(table), 'VIEW_LIMIT'); check(full.opened === 8, 'finite Native admissions');
const failing = fixture(); failing.registry.open(table); failing.registry.open(diff); failing.failClose();
failing.registry.dispose(); check(failing.closed === 2 && failing.registry.retained === 0, 'one close exception cannot strand other local handles');
console.log('VIEWS CONFORMANCE PASS: ' + checks + ' checks; adapter faults, not Native UI evidence');
