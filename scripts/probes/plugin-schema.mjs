// P0 conformance only. Ajv/Node are developer tools, never a Client runtime.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';

const [schemaFile, casesFile, tools, mutate] = process.argv.slice(2);
const require = createRequire(path.join(tools, 'package.json'));
const Ajv = require('ajv/dist/2020').default;
const bytes = fs.readFileSync(schemaFile);
const marker = 'GT_PLUGIN_SCHEMA_MUTATION';
assert(!bytes.toString().includes(marker), 'mutation marker present before start');
const schema = JSON.parse(bytes);
const cases = JSON.parse(fs.readFileSync(casesFile));

function compile(document) {
  return new Ajv({ strict: true, allErrors: true, validateFormats: false }).compile(document);
}
const fixtures = new Map();
for (const [name, message] of Object.entries(cases.messages)) {
  fixtures.set('valid-' + name, { frame: { ...structuredClone(cases.base), ...message }, valid: true });
}
for (const patch of cases.patches) {
  assert(Object.hasOwn(cases.messages, patch.from), 'fixture source is missing');
  const frame = { ...structuredClone(cases.base), ...structuredClone(cases.messages[patch.from]) };
  let target = frame;
  for (const segment of patch.path.slice(0, -1)) {
    assert(Object.hasOwn(target, segment), 'patch parent missing');
    target = target[segment];
  }
  const key = patch.path.at(-1);
  if (patch.remove) {
    assert(Object.hasOwn(target, key), 'patch removal target missing');
    delete target[key];
  } else {
    Object.defineProperty(target, key, { value: structuredClone(patch.value), enumerable: true, configurable: true });
  }
  assert(!fixtures.has(patch.name), 'duplicate fixture name');
  fixtures.set(patch.name, { frame, valid: patch.valid });
}

function accepts(validator, frame) {
  return Buffer.byteLength(JSON.stringify(frame), 'utf8') <= 65536 && validator(frame);
}
function check(validator, name) {
  const { frame, valid } = fixtures.get(name);
  const observed = accepts(validator, frame);
  assert.equal(observed, valid, 'schema fixture mismatch/' + name + ': ' + JSON.stringify(validator.errors));
}
const validator = compile(schema);
for (const name of fixtures.keys()) check(validator, name);
// The schema describes JSON instances; the owner must enforce raw UTF-8 byte
// budgets BEFORE parsing too. This harness only checks serialized instances.
const large = structuredClone(fixtures.get('valid-request').frame);
large.payload.input = '界'.repeat(22000);
assert(Buffer.byteLength(JSON.stringify(large), 'utf8') > 65536);
assert(validator(large), 'large generic plugin payload should be limited by its envelope/owner, not silently truncated');
assert(!accepts(validator, large), 'UTF-8 envelope budget not enforced');
const offerBytes = Buffer.byteLength(JSON.stringify(fixtures.get('valid-offer').frame), 'utf8');
assert(offerBytes <= 768, 'candidate offer does not fit probe fragment');
console.log(`PASS schema ${fixtures.size} instances; UTF-8 envelope budget; offer ${offerBytes} bytes`);

let mutationCount = 0;
if (mutate === '--mutate') {
  const mutations = [
    ['unknown-root-field', document => { document.additionalProperties = true; }],
    ['wire-version', document => { document.properties.wireVersion = { type: 'integer' }; }],
    ['leading-zero-epoch', document => { document.$defs.counter.pattern = '^[0-9]+$'; }],
    ['valid-response-unknown', document => { document.$defs.outcome.oneOf.pop(); }],
  ];
  for (const [target, change] of mutations) {
    const copy = structuredClone(schema);
    copy.$comment = marker;
    change(copy);
    const altered = compile(copy); // A schema/compiler failure is never target red.
    let hit = false;
    try { check(altered, target); }
    catch (error) {
      assert(error instanceof assert.AssertionError && error.message.startsWith('schema fixture mismatch/' + target + ':'), error);
      hit = true;
    }
    assert(hit, 'schema mutation survived/' + target);
    check(altered, 'valid-offer');
    check(compile(schema), target);
    assert(bytes.equals(fs.readFileSync(schemaFile)), 'schema source changed');
    console.log(`PASS schema mutation ${target}: named assertion red / offer green / restored green / unchanged bytes`);
    mutationCount++;
  }
}
console.log(JSON.stringify({ candidate: true, schemaDraft: '2020-12', instances: fixtures.size,
  mutations: mutationCount, offerBytes, lexicalOrChannelConformance: false }));
