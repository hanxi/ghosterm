// Developer metadata conformance. All catalogs/program facts are synthetic;
// this does not execute plugins/backends or prove any filesystem IO boundary.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

const [root, tools, exampleFile, mutationFlag] = process.argv.slice(2);
const require = createRequire(path.join(tools, 'package.json'));
const Ajv = require('ajv/dist/2020').default;
const semver = require('semver');
const directory = path.join(root, 'packages/plugin-sdk/schema');
const ajv = new Ajv({ strict: true, allErrors: true, validateFormats: false });
for (const name of ['bridge-v1.candidate.json', 'manifest-v1.candidate.json', 'integrity-v1.candidate.json']) {
  ajv.addSchema(JSON.parse(fs.readFileSync(path.join(directory, name))));
}
const validateManifest = ajv.getSchema('https://ghosterm.invalid/plugin-schema/manifest-v1.candidate.json');
const validateIntegrity = ajv.getSchema('https://ghosterm.invalid/plugin-schema/integrity-v1.candidate.json');
const source = path.join(root, 'packages/plugin-sdk/testkit/package-metadata.mjs');
const original = fs.readFileSync(source);
const mark = 'GT_PACKAGE_METADATA_MUTATION';
assert(!original.toString().includes(mark), 'mutation marker found before start');
const module = await import(pathToFileURL(source));
function inspector(api = module) { return api.createPackageInspector({ validateManifest, validateIntegrity, semver }); }

function fixture(change = () => {}) {
  const manifest = {
    schemaVersion: 1, id: 'org.ghosterm.preview', pluginVersion: '0.1.0',
    sdkApiRange: '>=0.1.0 <0.2.0', bridgeProtocolRange: [1], publisher: 'ghosterm', license: 'MIT',
    commands: [{ path: ['preview'], title: '预览', argsSchema: 'schemas/command.json' }],
    client: { runtime: 'portable-js', entry: 'client/index.js', requires: ['ui.resource', 'resource.read'], optional: ['ui.diff'] },
    executor: { variants: [{ id: 'portable', runtime: 'portable-js', entry: 'executor/index.js', requires: ['file.readScoped'] }] },
    permissions: [{ side: 'executor', capability: 'file.readScoped', purpose: '读取显式指定的文件', scope: 'instance' }],
  };
  const host = { side: 'client', sdkVersion: '0.1.1', protocols: [1], runtimes: ['portable-js'],
    capabilities: ['ui.resource', 'resource.read', 'file.readScoped'], os: 'macos', arch: 'aarch64' };
  change(manifest, host);
  const texts = {
    'plugin.json': JSON.stringify(manifest), 'README': 'fixture only', 'LICENSES/MIT.txt': 'fixture license',
    'client/index.js': 'export function activate() {}', 'executor/index.js': 'export function activate() {}',
    'executor/portable/index.js': 'export function activate() {}',
    'schemas/command.json': '{}', 'schemas/version-map.json': '{}', 'bin/helper': 'not executable test data',
    'schemas/codex-version-map.json': '{}',
  };
  const observed = Object.entries(texts).map(([name, text]) => ({ path: name, bytes: Buffer.byteLength(text),
    sha256: createHash('sha256').update(text).digest('hex'), kind: 'regular' }));
  const integrity = { schemaVersion: 1, members: observed.map(({ path, bytes, sha256 }) => ({ path, bytes, sha256 })) };
  const raw = JSON.stringify(integrity);
  observed.push({ path: 'integrity.json', bytes: Buffer.byteLength(raw), sha256: createHash('sha256').update(raw).digest('hex'), kind: 'regular' });
  return { manifest, host, observed, integrity };
}
function execute(input, inspect = inspector()) { return inspect(input.manifest, input.integrity, input.observed, input.host); }
function compatible(input, inspect) {
  const result = execute(input, inspect);
  assert.equal(result.kind, 'compatible');
  assert.equal(result.authorization, 'not-granted');
  assert.equal(result.publisherAuthenticated, false);
  return result;
}
function rejects(code, input, state = 'invalid', inspect) {
  assert.throws(() => execute(input, inspect), error => error.name === 'PackageCheckError' && error.code === code && error.state === state);
}
function modifyCatalog(action) { const value = fixture(); action(value); return value; }
function execFixture(change = () => {}) { return fixture((manifest, host) => { host.side = 'executor'; change(manifest, host); }); }
function nodeFixture(change = () => {}) {
  return execFixture((manifest, host) => {
    manifest.executor.variants = [{ id: 'node', runtime: 'node', entry: 'executor/index.js', runtimeVersionRange: '>=20 <23', requires: ['file.readScoped'] }];
    host.runtimes.push('node'); host.programs = { node: { state: 'available', version: '22.4.0' } };
    change(manifest, host);
  });
}
function processFixture(policy = 'range', change = () => {}) {
  return execFixture((manifest, host) => {
    const variant = manifest.executor.variants[0];
    variant.requires.push('process.spawn');
    variant.dependencies = [{ program: 'tool', versionPolicy: policy,
      ...(policy === 'range' ? { versionRange: '^1.2.0' } : { schema: 'schemas/version-map.json' }) }];
    manifest.permissions.push({ side: 'executor', capability: 'process.spawn', programs: ['tool'], purpose: 'fixture only', scope: 'instance' });
    host.capabilities.push('process.spawn'); host.programs = { tool: { state: 'available', version: '1.2.3' } };
    change(manifest, host);
  });
}
function oversizedPackage() {
  return modifyCatalog(f => {
    for (let i = 0; i < 4; i++) {
      const member = { path: `assets/${i}`, bytes: 16777216, sha256: 'a'.repeat(64) };
      f.integrity.members.push(member);
      f.observed.push({ ...member, kind: 'regular' });
    }
  });
}
const cases = new Map();
function add(name, run) { assert(!cases.has(name)); cases.set(name, run); }
function planFixture(side) {
  const example = JSON.parse(fs.readFileSync(exampleFile));
  const f = fixture((manifest, host) => {
    for (const key of Object.keys(manifest)) delete manifest[key];
    Object.assign(manifest, example);
    host.side = side;
    host.capabilities = side === 'client' ? manifest.client.requires : manifest.executor.variants[0].requires;
  });
  if (side === 'executor') {
    f.host.programs = { codex: { state: 'available', version: '1.2.3' } };
    f.host.adapters = { codex: { state: 'verified', version: '1.2.3',
      schemaDigest: f.observed.find(member => member.path === 'schemas/codex-version-map.json').sha256 } };
  }
  return f;
}
add('plan-example-client', () => compatible(planFixture('client')));
add('plan-example-executor', () => compatible(planFixture('executor')));
add('client-compatible', () => compatible(fixture()));
add('executor-compatible', () => compatible(execFixture()));
add('client-independent-of-executor-runtime', () => compatible(nodeFixture((m, h) => { h.side = 'client'; h.runtimes = ['portable-js']; delete h.programs; })));
add('optional-missing', () => assert.deepEqual(compatible(fixture()).optionalMissing, ['ui.diff']));
add('client-only', () => compatible(fixture(m => { delete m.executor; m.permissions = []; })));
add('executor-only', () => compatible(execFixture(m => { delete m.client; })));
add('node-compatible', () => compatible(nodeFixture()));
add('exec-compatible', () => compatible(execFixture((m, h) => {
  m.executor.variants = [{ id: 'native', runtime: 'exec', executable: 'bin/helper', requires: ['file.readScoped'], os: ['macos'], arch: ['aarch64'] }];
  h.runtimes.push('exec');
})));
add('explicit-variant', () => compatible(execFixture((m, h) => { m.executor.variants.push({ ...m.executor.variants[0], id: 'other' }); h.variantId = 'portable'; })));
add('prerelease-explicit', () => compatible(fixture((m, h) => { m.sdkApiRange = '>=0.1.0-beta.1 <0.2.0'; h.sdkVersion = '0.1.0-beta.2'; })));
add('dependency-range-compatible', () => compatible(processFixture()));
add('adapter-verified-fact', () => {
  const f = processFixture('adapter-verified'); f.host.adapters = { tool: { state: 'verified', version: '1.2.3', schemaDigest: f.observed.find(m => m.path === 'schemas/version-map.json').sha256 } };
  compatible(f);
});
add('repeated-command-token', () => compatible(fixture(m => { m.commands[0].path = ['echo', 'echo']; })));
add('unicode-member-name', () => compatible(modifyCatalog(f => { const d = { path: 'assets/图.txt', bytes: 0, sha256: 'a'.repeat(64) }; f.integrity.members.push(d); f.observed.push({ ...d, kind: 'regular' }); })));
add('signature-presence-not-authentication', () => compatible(modifyCatalog(f => {
  f.observed.push({ path: 'signature', bytes: 3, sha256: 'a'.repeat(64), kind: 'regular' });
})));

const schemaMutations = [
  ['manifest-version', m => { m.schemaVersion = 2; }], ['unknown-field', m => { m.postInstall = 'do not execute'; }],
  ['invalid-id', m => { m.id = 'org/ghosterm'; }], ['invalid-version', m => { m.pluginVersion = '01.0.0'; }],
  ['range-empty', m => { m.sdkApiRange = ''; }], ['no-sides', m => { delete m.client; delete m.executor; }],
  ['client-node', m => { m.client.runtime = 'node'; }], ['missing-client-entry', m => { delete m.client.entry; }],
  ['duplicate-capability', m => { m.client.requires.push(m.client.requires[0]); }],
  ['unknown-permission-field', m => { m.permissions[0].allowForever = true; }],
];
for (const [name, change] of schemaMutations) add(name, () => rejects('MANIFEST_SCHEMA', fixture(change)));
add('node-range-required', () => rejects('MANIFEST_SCHEMA', nodeFixture(m => { delete m.executor.variants[0].runtimeVersionRange; })));
add('exec-target-required', () => rejects('MANIFEST_SCHEMA', execFixture(m => { m.executor.variants = [{ id: 'native', runtime: 'exec', executable: 'bin/helper', requires: [] }]; })));
for (const memberPath of ['/absolute', '../escape', 'a/../escape', 'C:/escape', 'a\\b', 'a//b', 'assets/', 'assets/x.']) {
  add('unsafe-member-' + memberPath, () => rejects('MEMBER_PATH', modifyCatalog(f => { f.observed[0].path = memberPath; })));
}
add('non-nfc', () => rejects('MEMBER_PATH', modifyCatalog(f => { f.observed[0].path = 'assets/e\u0301'; })));
add('unpaired-surrogate', () => rejects('MEMBER_PATH', modifyCatalog(f => { f.observed[0].path = 'assets/\ud800'; })));
add('null-member', () => rejects('MEMBER_SHAPE', modifyCatalog(f => { f.observed.push(null); })));
add('duplicate-member', () => rejects('DUPLICATE_MEMBER', modifyCatalog(f => { f.observed.push({ ...f.observed[0] }); })));
add('case-collision', () => rejects('CASE_COLLISION', modifyCatalog(f => { f.observed.push({ ...f.observed[0], path: 'Plugin.json' }); })));
add('file-directory-collision', () => rejects('FILE_DIRECTORY_COLLISION', modifyCatalog(f => { f.observed.push({ ...f.observed[0], path: 'client' }); })));
for (const kind of ['symlink', 'hardlink', 'directory', 'device']) add('nonregular-' + kind, () => rejects('NONREGULAR_MEMBER', modifyCatalog(f => { f.observed[0].kind = kind; })));
add('too-many-members', () => rejects('MEMBER_COUNT', modifyCatalog(f => { f.observed = Array(1025).fill(f.observed[0]); })));
add('oversize-member', () => rejects('MEMBER_BYTES', modifyCatalog(f => { f.observed[0].bytes = 16777217; })));
add('oversize-integrity-file', () => rejects('MEMBER_BYTES', modifyCatalog(f => { f.observed.find(m => m.path === 'integrity.json').bytes = 16777217; })));
add('oversize-total', () => rejects('PACKAGE_BYTES', oversizedPackage()));
add('invalid-observed-digest', () => rejects('MEMBER_DIGEST', modifyCatalog(f => { f.observed[0].sha256 = 'a'.repeat(63); })));
add('member-hash-mismatch', () => rejects('MEMBER_MISMATCH', modifyCatalog(f => { f.observed[0].sha256 = 'a'.repeat(64); })));
add('member-length-mismatch', () => rejects('MEMBER_MISMATCH', modifyCatalog(f => { f.observed[0].bytes++; })));
add('missing-member', () => rejects('MISSING_MEMBER', modifyCatalog(f => { f.observed.shift(); })));
add('unindexed-member', () => rejects('UNINDEXED_MEMBER', modifyCatalog(f => { f.observed.push({ ...f.observed[0], path: 'extra.js' }); })));
add('integrity-self-reference', () => rejects('INTEGRITY_SELF_REFERENCE', modifyCatalog(f => { const m = f.observed.at(-1); f.integrity.members.push({ path: m.path, bytes: m.bytes, sha256: m.sha256 }); })));
add('duplicate-integrity-path', () => rejects('DUPLICATE_INTEGRITY_MEMBER', modifyCatalog(f => { f.integrity.members.push({ ...f.integrity.members[0], sha256: 'a'.repeat(64) }); })));
add('missing-license', () => rejects('REQUIRED_PACKAGE_MEMBER', modifyCatalog(f => { f.observed = f.observed.filter(m => !m.path.startsWith('LICENSES/')); f.integrity.members = f.integrity.members.filter(m => !m.path.startsWith('LICENSES/')); })));
add('missing-referenced-entry', () => rejects('MISSING_REFERENCED_MEMBER', fixture(m => { m.client.entry = 'missing.js'; })));
add('entry-not-script', () => rejects('SCRIPT_ENTRY', fixture(m => { m.client.entry = 'schemas/command.json'; })));
add('invalid-sdk-range', () => rejects('VERSION_RANGE', fixture(m => { m.sdkApiRange = 'not a range'; })));
add('duplicate-variant', () => rejects('DUPLICATE_VARIANT', fixture(m => { m.executor.variants.push({ ...m.executor.variants[0], entry: 'client/index.js' }); })));
add('duplicate-dependency', () => rejects('DUPLICATE_DEPENDENCY', processFixture('range', m => { m.executor.variants[0].dependencies.push({ program: 'tool', versionPolicy: 'range', versionRange: '^2.0.0' }); })));
add('missing-permission', () => rejects('MISSING_PERMISSION_DECLARATION', fixture(m => { m.permissions = []; })));
add('undeclared-permission', () => rejects('UNDECLARED_PERMISSION', fixture(m => { m.permissions.push({ side: 'client', capability: 'network.connect', purpose: 'fixture', scope: 'instance' }); })));
add('duplicate-permission', () => rejects('DUPLICATE_PERMISSION', fixture(m => { m.permissions.push({ ...m.permissions[0], purpose: 'another purpose' }); })));
add('capability-overlap', () => rejects('CAPABILITY_OVERLAP', fixture(m => { m.client.optional.push(m.client.requires[0]); })));
add('client-process-capability', () => rejects('CLIENT_EXECUTOR_CAPABILITY', fixture((m, h) => { m.client.requires.push('process.spawn'); h.capabilities.push('process.spawn'); m.permissions.push({ side: 'client', capability: 'process.spawn', purpose: 'fixture', scope: 'instance' }); })));
add('reserved-command', () => rejects('RESERVED_COMMAND', fixture(m => { m.commands[0].path = ['plugin', 'install']; })));
add('command-prefix-conflict', () => rejects('COMMAND_CONFLICT', fixture(m => { m.commands.push({ ...m.commands[0], path: ['preview', 'sub'] }); })));
add('sdk-unsupported', () => rejects('SDK_UNSUPPORTED', fixture((m, h) => { h.sdkVersion = '0.2.0'; }), 'unsupported'));
add('prerelease-not-implied', () => rejects('SDK_UNSUPPORTED', fixture((m, h) => { h.sdkVersion = '0.1.1-beta.1'; }), 'unsupported'));
add('host-unknown', () => rejects('HOST_UNKNOWN', fixture((m, h) => { delete h.side; }), 'unknown'));
add('protocol-unknown', () => rejects('PROTOCOL_UNKNOWN', fixture((m, h) => { delete h.protocols; }), 'unknown'));
add('protocol-malformed', () => rejects('PROTOCOL_UNKNOWN', fixture((m, h) => { h.protocols = [1, 'bad']; }), 'unknown'));
add('protocol-unsupported', () => rejects('PROTOCOL_UNSUPPORTED', fixture((m, h) => { h.protocols = [2]; }), 'unsupported'));
add('capabilities-unknown', () => rejects('CAPABILITIES_UNKNOWN', fixture((m, h) => { delete h.capabilities; }), 'unknown'));
add('capabilities-denial-malformed', () => rejects('CAPABILITIES_UNKNOWN', fixture((m, h) => { h.deniedCapabilities = 'ui.resource'; }), 'unknown'));
add('capability-unsupported', () => rejects('CAPABILITY_UNSUPPORTED', fixture((m, h) => { h.capabilities = []; }), 'unsupported'));
add('capability-denied', () => rejects('CAPABILITY_DENIED', fixture((m, h) => { h.deniedCapabilities = ['ui.resource']; }), 'denied'));
add('runtime-unsupported', () => rejects('RUNTIME_UNSUPPORTED', fixture((m, h) => { h.runtimes = []; }), 'unsupported'));
add('side-unsupported', () => rejects('SIDE_UNSUPPORTED', fixture((m, h) => { delete m.client; }), 'unsupported'));
add('variant-id-unsupported', () => rejects('VARIANT_UNSUPPORTED', execFixture((m, h) => { h.variantId = 'missing'; }), 'unsupported'));
add('variant-selection-required', () => rejects('VARIANT_SELECTION_REQUIRED', execFixture((m, h) => { m.executor.variants.push({ ...m.executor.variants[0], id: 'other', runtime: 'node', runtimeVersionRange: '>=20' }); })));
add('platform-unsupported', () => rejects('PLATFORM_UNSUPPORTED', execFixture(m => { m.executor.variants[0].os = ['linux']; }), 'unsupported'));
add('platform-unknown', () => rejects('PLATFORM_UNKNOWN', execFixture((m, h) => { m.executor.variants[0].os = ['macos']; delete h.os; }), 'unknown'));
for (const [name, fact, code, state] of [
  ['unknown', undefined, 'DEPENDENCY_UNKNOWN', 'unknown'], ['missing', { state: 'missing' }, 'DEPENDENCY_UNSUPPORTED', 'unsupported'],
  ['denied', { state: 'denied' }, 'DEPENDENCY_DENIED', 'denied'], ['version', { state: 'available', version: '2.0.0' }, 'DEPENDENCY_VERSION_UNSUPPORTED', 'unsupported']
]) add('dependency-' + name, () => rejects(code, processFixture('range', (m, h) => { h.programs.tool = fact; }), state));
add('node-version-unsupported', () => rejects('RUNTIME_VERSION_UNSUPPORTED', nodeFixture((m, h) => { h.programs.node.version = '23.0.0'; }), 'unsupported'));
add('adapter-unknown', () => rejects('ADAPTER_UNKNOWN', processFixture('adapter-verified'), 'unknown'));
add('adapter-rejected', () => rejects('ADAPTER_UNSUPPORTED', processFixture('adapter-verified', (m, h) => { h.adapters = { tool: { state: 'rejected' } }; }), 'unsupported'));
add('adapter-stale', () => rejects('ADAPTER_UNKNOWN', processFixture('adapter-verified', (m, h) => { h.adapters = { tool: { state: 'verified', version: '1.2.2', schemaDigest: 'a'.repeat(64) } }; }), 'unknown'));
add('registry-leaf-conflict', () => {
  const first = fixture().manifest; const second = structuredClone(first); second.id = 'org.example.other';
  assert.equal(module.commandConflicts([first, second]).length, 1);
  second.commands[0].path = ['other']; assert.equal(module.commandConflicts([first, second]).length, 0);
});

for (const [name, run] of cases) {
  try { run(); } catch (error) { console.error('FAILED metadata fixture', name); throw error; }
}
console.log(`PASS package metadata ${cases.size} named fixtures; no activation or publisher authentication granted`);

let mutations = 0;
if (mutationFlag === '--mutate') {
  const targets = [
    ['hash', "actual.bytes !== member.bytes || actual.sha256 !== member.sha256", "actual.bytes !== member.bytes", () => modifyCatalog(f => { f.observed[0].sha256 = 'a'.repeat(64); }), 'MEMBER_MISMATCH'],
    ['unindexed', "!excluded.has(path) && !declared.has(path)", "false", () => modifyCatalog(f => { f.observed.push({ ...f.observed[0], path: 'extra.js' }); }), 'UNINDEXED_MEMBER'],
    ['unknown-dependency', "if (!fact || fact.state === 'unknown') fail('DEPENDENCY_UNKNOWN', 'unknown');", "if (!fact || fact.state === 'unknown') fail('DEPENDENCY_DENIED', 'denied');", () => processFixture('range', (m, h) => { h.programs.tool = { state: 'unknown', version: '1.2.3' }; }), 'DEPENDENCY_UNKNOWN'],
    ['client-role', "name === 'client' &&", "false &&", () => fixture((m, h) => { m.client.requires.push('process.spawn'); h.capabilities.push('process.spawn'); m.permissions.push({ side: 'client', capability: 'process.spawn', purpose: 'fixture', scope: 'instance' }); }), 'CLIENT_EXECUTOR_CAPABILITY'],
    ['member-type', "if (member.kind !== 'regular') fail('NONREGULAR_MEMBER');", "/* skipped member kind */", () => modifyCatalog(f => { f.observed[0].kind = 'symlink'; }), 'NONREGULAR_MEMBER'],
    ['total-budget', "if (total > 67108864) fail('PACKAGE_BYTES');", "/* skipped aggregate budget */", oversizedPackage, 'PACKAGE_BYTES'],
  ];
  for (const [name, before, after, input, code] of targets) {
    const state = name === 'unknown-dependency' ? 'unknown' : 'invalid';
    rejects(code, input(), state, inspector());
    const text = original.toString(); assert.equal(text.split(before).length, 2, 'mutation anchor drift');
    const copy = path.join(tools, `mutated-${name}.mjs`);
    fs.writeFileSync(copy, text.replace(before, after) + `\n// ${mark}\n`);
    try {
      const altered = await import(pathToFileURL(copy));
      assert.throws(() => rejects(code, input(), state, inspector(altered)), error =>
        error instanceof assert.AssertionError && (!error.actual || error.actual.name === 'PackageCheckError'));
      compatible(fixture(), inspector(altered));
    } finally {
      fs.writeFileSync(copy, original);
      assert(original.equals(fs.readFileSync(copy)) && original.equals(fs.readFileSync(source)), 'metadata source restore mismatch');
    }
    // A new URL forces Node to load the restored bytes instead of reusing the
    // previously imported mutation (or the original module's cached instance).
    const restored = await import(pathToFileURL(copy).href + '?restored=1');
    rejects(code, input(), state, inspector(restored));
    console.log(`PASS metadata mutation ${name}: target assertion red / client compatible / restored green / source bytes unchanged`);
    mutations++;
  }
}
console.log(JSON.stringify({ candidate: true, metadataFixtures: cases.size, mutations, filesystemVerification: false, activation: false }));
