// Physical fixtures + independent Node/Ajv/npm-semver oracle for the native C
// checker. All files/facts are test-owned; never executes a plugin/backend.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';
const [root, tools, out] = process.argv.slice(2);
const require = createRequire(path.join(tools, 'package.json'));
const Ajv = require('ajv/dist/2020').default;
const ajv = new Ajv({ strict: true, allErrors: true, validateFormats: false });
for (const name of ['bridge', 'manifest', 'integrity']) ajv.addSchema(JSON.parse(fs.readFileSync(path.join(root, `packages/plugin-sdk/schema/${name}-v1.candidate.json`))));
const validateManifest = ajv.getSchema('https://ghosterm.invalid/plugin-schema/manifest-v1.candidate.json');
const validateIntegrity = ajv.getSchema('https://ghosterm.invalid/plugin-schema/integrity-v1.candidate.json');
const { createPackageInspector, commandConflicts, PackageCheckError } = await import(pathToFileURL(path.join(root, 'packages/plugin-sdk/testkit/package-metadata.mjs')));
const inspect = createPackageInspector({ validateManifest, validateIntegrity, semver: require('semver') });
const hash = data => createHash('sha256').update(data).digest('hex');
const vectors = [];
function fixture() {
  return { manifest: { schemaVersion: 1, id: 'org.ghosterm.fixture', pluginVersion: '0.1.0', sdkApiRange: '^0.1.0', bridgeProtocolRange: [1], publisher: 'fixture', license: 'MIT',
    commands: [{ path: ['fixture'], title: 'Fixture', argsSchema: 'schemas/input.json' }],
    client: { runtime: 'portable-js', entry: 'client/index.js', requires: ['ui.resource'], optional: ['ui.diff'] },
    executor: { variants: [{ id: 'portable', runtime: 'portable-js', entry: 'executor/index.js', requires: ['file.readScoped'] }] },
    permissions: [{ side: 'executor', capability: 'file.readScoped', purpose: 'fixture only', scope: 'instance' }] },
    host: { side: 'client', sdkVersion: '0.1.1', protocols: [1], runtimes: ['portable-js'], capabilities: ['ui.resource', 'file.readScoped'], os: 'macos', arch: 'aarch64' }, registry: [],
    files: { README: 'fixture only', 'LICENSES/MIT.txt': 'fixture license', 'client/index.js': "throw Error('MUST_NOT_EXECUTE');export function activate(){}", 'executor/index.js': "throw Error('MUST_NOT_EXECUTE');export function activate(){}", 'schemas/input.json': '{"type":"object"}', 'schemas/version.json': '{}', 'bin/helper': 'NOT AN EXECUTABLE' } };
}
function node(f) { f.host.side = 'executor'; f.host.runtimes.push('node'); f.host.programs = { node: { state: 'available', version: '22.1.0' } }; Object.assign(f.manifest.executor.variants[0], { runtime: 'node', runtimeVersionRange: '^22.0.0' }); }
function dependency(f, policy = 'range') { f.host.side = 'executor'; f.host.capabilities.push('process.spawn'); f.manifest.executor.variants[0].requires.push('process.spawn'); f.manifest.permissions.push({ side: 'executor', capability: 'process.spawn', programs: ['tool'], purpose: 'fixture only', scope: 'instance' }); f.manifest.executor.variants[0].dependencies = [{ program: 'tool', versionPolicy: policy, ...(policy === 'range' ? { versionRange: '^1.2.0' } : { schema: 'schemas/version.json' }) }]; f.host.programs = { tool: { state: 'available', version: '1.2.3' } }; }
function add(name, change = () => {}, code = 'COMPATIBLE', state = 'invalid', native = false) {
  const f = fixture(); change(f); f.files['plugin.json'] = f.rawManifest ?? JSON.stringify(f.manifest);
  const members = Object.entries(f.files).filter(([p]) => !['integrity.json', 'signature'].includes(p)).map(([p, b]) => ({ path: p, bytes: Buffer.byteLength(b), sha256: hash(b) }));
  const integrity = { schemaVersion: 1, members }; f.integrity?.(integrity, f); f.files['integrity.json'] = f.rawIntegrity ?? JSON.stringify(integrity); f.afterIndex?.(f);
  const observed = Object.entries(f.files).map(([p, b]) => ({ path: p, bytes: Buffer.byteLength(b), sha256: hash(b), kind: 'regular' }));
  let result;
  if (!native) {
    try {
      result = inspect(JSON.parse(f.files['plugin.json']), JSON.parse(f.files['integrity.json']), observed, f.host);
      if (!Array.isArray(f.registry) || f.registry.length > 64 || f.registry.some(m => !validateManifest(m))) throw new PackageCheckError('REGISTRY_UNKNOWN', 'unknown');
      result.associationRequired = commandConflicts([...f.registry, f.manifest]).some(c => c.first === f.manifest.id || c.second === f.manifest.id);
      assert.equal(code, 'COMPATIBLE', name + ': oracle unexpectedly compatible');
    } catch (error) {
      assert.equal(error.name, 'PackageCheckError', name + ': ' + error.stack);
      assert.equal(error.code, code, name); assert.equal(error.state, state, name);
      result = undefined;
    }
  }
  const directory = path.join(out, String(vectors.length).padStart(3, '0') + '-' + name); const pkg = path.join(directory, 'pkg'); fs.mkdirSync(pkg, { recursive: true });
  for (const [p, data] of Object.entries(f.files)) { const target = path.join(pkg, p); assert(target.startsWith(pkg + path.sep)); fs.mkdirSync(path.dirname(target), { recursive: true }); fs.writeFileSync(target, data); }
  const hostFile = f.host === null ? '-' : path.join(directory, 'host.json'); if (hostFile !== '-') fs.writeFileSync(hostFile, f.rawHost ?? JSON.stringify(f.host));
  const registryFile = path.join(directory, 'registry.json'); fs.writeFileSync(registryFile, f.rawRegistry ?? JSON.stringify(f.registry));
  const status = code === 'COMPATIBLE' ? 0 : ({ invalid: 1, unknown: 2, unsupported: 3, denied: 4, limit: 5 })[state]; assert(Number.isInteger(status), name);
  vectors.push({ name, directory: pkg, hostFile, registryFile, status, code, nativeOnly: native,
    ...(result ? { expected: { id: f.manifest.id, version: f.manifest.pluginVersion, entry: result.entry, runtime: f.host.side === 'client' ? f.manifest.client.runtime : f.manifest.executor.variants.find(v => v.id === result.variantId).runtime, variant: result.variantId ?? '', executor: f.host.side === 'executor', protocols: result.protocols, optionalMissing: result.optionalMissing, associationRequired: result.associationRequired, authorizationGranted: false, publisherAuthenticated: false, packageDigest: hash(f.files['integrity.json']), manifestDigest: hash(f.files['plugin.json']) } } : {}) });
}
add('client'); add('executor', f => { f.host.side = 'executor'; });
add('client-only', f => { delete f.manifest.executor; f.manifest.permissions = []; });
add('executor-only', f => { delete f.manifest.client; f.host.side = 'executor'; });
add('node', f => node(f));
add('exec', f => { f.host.side = 'executor'; f.host.runtimes.push('exec'); f.manifest.executor.variants = [{ id: 'native', runtime: 'exec', executable: 'bin/helper', requires: [], os: ['macos'], arch: ['aarch64'] }]; f.manifest.permissions = []; });
add('side-independent', f => { node(f); f.host.side = 'client'; f.host.runtimes = ['portable-js']; delete f.host.programs; });
add('optional-available', f => { f.host.capabilities.push('ui.diff'); });
add('optional-denied', f => { f.host.capabilities.push('ui.diff'); f.host.deniedCapabilities = ['ui.diff']; });
add('prerelease', f => { f.manifest.sdkApiRange = '>=0.1.0-beta.1 <0.2.0'; f.host.sdkVersion = '0.1.0-beta.2'; });
add('nfc-chinese', f => { f.manifest.client.entry = 'client/文件.js'; f.files['client/文件.js'] = f.files['client/index.js']; });
add('signature', f => { f.files.signature = 'NOT A VERIFIED SIGNATURE'; });
add('binary-entry', f => { f.files['client/index.js'] = Buffer.from([0, 255, 0]); });
add('empty-entry', f => { f.files['client/index.js'] = ''; });
add('many-members', f => { for (let i = Object.keys(f.files).length + 1; i < 1023; i++) f.files[`assets/${i}.txt`] = 'x'; });
add('large-metadata', f => { f.rawManifest = JSON.stringify(f.manifest).padEnd(16777216, ' '); });
add('explicit-variant', f => { f.host.side = 'executor'; f.manifest.executor.variants.push({ ...f.manifest.executor.variants[0], id: 'other' }); f.host.variantId = 'portable'; });
add('dependency', f => dependency(f));
add('adapter', f => { dependency(f, 'adapter-verified'); f.host.adapters = { tool: { state: 'verified', version: '1.2.3', schemaDigest: hash(f.files['schemas/version.json']) } }; });
add('registry-conflict', f => { const other = structuredClone(f.manifest); other.id = 'org.other.fixture'; f.registry = [other]; });
add('manifest-version', f => { f.manifest.schemaVersion = 2; }, 'MANIFEST_SCHEMA');
add('manifest-unknown', f => { f.manifest.postInstall = 'MUST_NOT_EXECUTE'; }, 'MANIFEST_SCHEMA');
add('version-invalid', f => { f.manifest.pluginVersion = '01.0.0'; }, 'MANIFEST_SCHEMA');
add('path-traversal', f => { f.manifest.client.entry = '../escape.js'; }, 'MANIFEST_SCHEMA');
add('client-executor-cap', f => { f.manifest.client.requires.push('file.readScoped'); }, 'CLIENT_EXECUTOR_CAPABILITY');
add('missing-permission', f => { f.manifest.permissions = []; }, 'MISSING_PERMISSION_DECLARATION');
add('capability-overlap', f => { f.manifest.client.optional.push('ui.resource'); }, 'CAPABILITY_OVERLAP');
add('duplicate-permission', f => { f.manifest.permissions.push({ ...f.manifest.permissions[0], purpose: 'different' }); }, 'DUPLICATE_PERMISSION');
add('undeclared-permission', f => { f.manifest.permissions.push({ side: 'executor', capability: 'network.fetch', purpose: 'fixture', scope: 'instance' }); }, 'UNDECLARED_PERMISSION');
add('reserved-command', f => { f.manifest.commands[0].path = ['plugin']; }, 'RESERVED_COMMAND');
add('command-prefix', f => { f.manifest.commands.push({ ...f.manifest.commands[0], path: ['fixture', 'child'] }); }, 'COMMAND_CONFLICT');
add('missing-entry', f => { f.manifest.client.entry = 'client/missing.js'; }, 'MISSING_REFERENCED_MEMBER');
add('entry-not-js', f => { f.manifest.client.entry = 'README'; }, 'SCRIPT_ENTRY');
add('bad-range', f => { f.manifest.sdkApiRange = 'NOT A RANGE'; }, 'VERSION_RANGE');
add('hash-mismatch', f => { f.integrity = i => { i.members[0].sha256 = 'a'.repeat(64); }; }, 'MEMBER_MISMATCH');
add('size-mismatch', f => { f.integrity = i => { i.members[0].bytes++; }; }, 'MEMBER_MISMATCH');
add('missing-member', f => { f.afterIndex = f => { delete f.files.README; }; }, 'MISSING_MEMBER');
add('unindexed-member', f => { f.afterIndex = f => { f.files['extra.js'] = 'x'; }; }, 'UNINDEXED_MEMBER');
add('missing-license', f => { delete f.files['LICENSES/MIT.txt']; }, 'REQUIRED_PACKAGE_MEMBER');
add('integrity-self-reference', f => { f.integrity = i => { i.members.push({ path: 'integrity.json', bytes: 0, sha256: 'a'.repeat(64) }); }; }, 'INTEGRITY_SELF_REFERENCE');
add('duplicate-integrity', f => { f.integrity = i => { i.members.push({ ...i.members[0], sha256: 'a'.repeat(64) }); }; }, 'DUPLICATE_INTEGRITY_MEMBER');
add('reserved-case', f => { f.files.Signature = 'x'; }, 'RESERVED_MEMBER_CASE');
add('unknown-host', f => { f.host = null; }, 'HOST_UNKNOWN', 'unknown');
add('sdk-missing', f => { delete f.host.sdkVersion; }, 'HOST_UNKNOWN', 'unknown');
add('sdk-version', f => { f.host.sdkVersion = '0.2.0'; }, 'SDK_UNSUPPORTED', 'unsupported');
add('protocol-unknown', f => { delete f.host.protocols; }, 'PROTOCOL_UNKNOWN', 'unknown');
add('protocol-unsupported', f => { f.host.protocols = [2]; }, 'PROTOCOL_UNSUPPORTED', 'unsupported');
add('capabilities-unknown', f => { delete f.host.capabilities; }, 'CAPABILITIES_UNKNOWN', 'unknown');
add('capability-unsupported', f => { f.host.capabilities = []; }, 'CAPABILITY_UNSUPPORTED', 'unsupported');
add('capability-denied', f => { f.host.deniedCapabilities = ['ui.resource']; }, 'CAPABILITY_DENIED', 'denied');
add('runtime-unsupported', f => { f.host.runtimes = []; }, 'RUNTIME_UNSUPPORTED', 'unsupported');
add('variant-choice', f => { f.host.side = 'executor'; f.manifest.executor.variants.push({ ...f.manifest.executor.variants[0], id: 'other' }); }, 'VARIANT_SELECTION_REQUIRED');
add('variant-missing', f => { f.host.side = 'executor'; f.host.variantId = 'missing'; }, 'VARIANT_UNSUPPORTED', 'unsupported');
add('variant-duplicate', f => { f.manifest.executor.variants.push({ ...f.manifest.executor.variants[0], entry: 'client/index.js' }); }, 'DUPLICATE_VARIANT');
add('platform-unknown', f => { f.host.side = 'executor'; f.manifest.executor.variants[0].os = ['macos']; delete f.host.os; }, 'PLATFORM_UNKNOWN', 'unknown');
add('platform-unsupported', f => { f.host.side = 'executor'; f.manifest.executor.variants[0].os = ['linux']; }, 'PLATFORM_UNSUPPORTED', 'unsupported');
add('node-unknown', f => { node(f); delete f.host.programs; }, 'DEPENDENCY_UNKNOWN', 'unknown');
add('node-missing', f => { node(f); f.host.programs.node.state = 'missing'; }, 'DEPENDENCY_UNSUPPORTED', 'unsupported');
add('node-denied', f => { node(f); f.host.programs.node.state = 'denied'; }, 'DEPENDENCY_DENIED', 'denied');
add('node-version', f => { node(f); f.host.programs.node.version = '20.0.0'; }, 'RUNTIME_VERSION_UNSUPPORTED', 'unsupported');
add('dependency-version', f => { dependency(f); f.host.programs.tool.version = '2.0.0'; }, 'DEPENDENCY_VERSION_UNSUPPORTED', 'unsupported');
add('adapter-unknown', f => { dependency(f, 'adapter-verified'); }, 'ADAPTER_UNKNOWN', 'unknown');
add('adapter-rejected', f => { dependency(f, 'adapter-verified'); f.host.adapters = { tool: { state: 'rejected' } }; }, 'ADAPTER_UNSUPPORTED', 'unsupported');
add('registry-unknown', f => { f.registry = [{}]; }, 'REGISTRY_UNKNOWN', 'unknown');
add('json-duplicate', f => { f.rawManifest = '{"schemaVersion":1,' + JSON.stringify(f.manifest).slice(1); }, 'JSON_DUPLICATE', 'invalid', true);
add('json-escaped-duplicate', f => { f.rawManifest = '{"schema\\u0056ersion":1,' + JSON.stringify(f.manifest).slice(1); }, 'JSON_DUPLICATE', 'invalid', true);
add('json-fractional', f => { f.rawManifest = JSON.stringify(f.manifest).replace('"schemaVersion":1', '"schemaVersion":1.00000000000000001'); }, 'JSON_INTEGER', 'invalid', true);
add('json-bom', f => { f.rawManifest = '\ufeff' + JSON.stringify(f.manifest); }, 'JSON_SYNTAX', 'invalid', true);
add('json-surrogate', f => { f.rawManifest = JSON.stringify(f.manifest).replace('"publisher":"fixture"', '"publisher":"\\ud800"'); }, 'JSON_UTF8', 'invalid', true);
add('json-raw-utf8', f => { f.rawManifest = Buffer.from([123, 34, 112, 34, 58, 34, 237, 160, 128, 34, 125]); }, 'JSON_UTF8', 'invalid', true);
add('fact-duplicate', f => { f.rawHost = '{"side":"client",' + JSON.stringify(f.host).slice(1); }, 'FACT_JSON', 'invalid', true);
add('registry-duplicate', f => { f.rawRegistry = '[{"id":"x","id":"y"}]'; }, 'FACT_JSON', 'invalid', true);
fs.writeFileSync(path.join(out, 'vectors.json'), JSON.stringify({ fixtures: vectors, nativeOnly: vectors.filter(v => v.nativeOnly).length }, null, 2) + '\n');
console.log(`PACKAGE NATIVE FIXTURES PASS: ${vectors.length} actual directories / ${vectors.filter(v => !v.nativeOnly).length} Node/Ajv/semver oracle cases`);
