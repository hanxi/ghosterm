#!/usr/bin/env node
// Developer-only reproducible generator. Normal Core builds need no Node/npm.
// Regenerate with npm-ci tools from scripts/fixtures/plugin-package-tools.
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import vm from 'node:vm';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
assert(args.length === 2 || (args.length === 3 && args[2] === '--check'), 'usage: --tools directory [--check]');
assert.equal(args[0], '--tools');
const tools = path.resolve(args[1]);
const check = args[2] === '--check';
const fixture = path.join(root, 'scripts/fixtures/plugin-package-tools');
for (const name of ['package.json', 'package-lock.json']) assert.deepEqual(fs.readFileSync(path.join(tools, name)), fs.readFileSync(path.join(fixture, name)), 'tool lock differs');
const require = createRequire(path.join(tools, 'package.json'));
const Ajv = require('ajv/dist/2020').default;
const standalone = require('ajv/dist/standalone').default;
const ajv = new Ajv({ strict: true, allErrors: false, validateFormats: false, code: { source: true } });
const schemaFiles = ['bridge-v1.candidate.json', 'manifest-v1.candidate.json', 'integrity-v1.candidate.json'].map(n => 'packages/plugin-sdk/schema/' + n);
for (const name of schemaFiles) ajv.addSchema(JSON.parse(fs.readFileSync(path.join(root, name))));
const generated = standalone(ajv, { manifest: 'https://ghosterm.invalid/plugin-schema/manifest-v1.candidate.json', integrity: 'https://ghosterm.invalid/plugin-schema/integrity-v1.candidate.json' });
const modules = new Map();
const depFiles = new Map();
const digest = data => createHash('sha256').update(data).digest('hex');
function add(file, content) {
  const id = file === '@schemas' ? file : path.relative(path.join(tools, 'node_modules'), file).split(path.sep).join('/');
  if (modules.has(id)) return id;
  assert(file === '@schemas' || (!id.startsWith('../') && !path.isAbsolute(id)), 'external module');
  const source = content ?? fs.readFileSync(file, 'utf8');
  assert(!source.includes('\0'), 'NUL source');
  const record = { source, dependencies: {} };
  modules.set(id, record);
  if (file !== '@schemas') depFiles.set(id, digest(source));
  const refs = [...source.matchAll(/\brequire\s*\(\s*(['"])([^'"]+)\1\s*\)/g)];
  assert.equal(refs.length, [...source.matchAll(/\brequire\s*\(/g)].length, 'nonliteral require in ' + id);
  for (const ref of refs) {
    const target = file === '@schemas' ? require.resolve(ref[2]) : createRequire(file).resolve(ref[2]);
    assert(path.isAbsolute(target), 'builtin module rejected');
    record.dependencies[ref[2]] = add(target);
  }
  return id;
}
const schemas = add('@schemas', generated);
const valid = add(require.resolve('semver/functions/valid'));
const validRange = add(require.resolve('semver/ranges/valid'));
const satisfies = add(require.resolve('semver/functions/satisfies'));
const checkerFile = 'packages/plugin-sdk/testkit/package-metadata.mjs';
const checker = fs.readFileSync(path.join(root, checkerFile), 'utf8').replace(/^export (?=(?:class|function) )/gm, '');
assert(!/^export /m.test(checker), 'unreviewed ESM export');
const byteOrder = ([a], [b]) => a < b ? -1 : a > b ? 1 : 0;
const factories = [...modules].sort(byteOrder).map(([id, m]) =>
  `${JSON.stringify(id)}:[function(module,exports,require){\n${m.source}\n},${JSON.stringify(m.dependencies)}]`).join(',\n');
const bundle = `"use strict";(()=>{\nconst factories={${factories}};const cache=Object.create(null);\nfunction load(id){if(cache[id])return cache[id].exports;const record=factories[id];if(!record)throw Error('internal module');const module={exports:{}};cache[id]=module;record[0](module,module.exports,key=>{if(!Object.prototype.hasOwnProperty.call(record[1],key))throw Error('external module');return load(record[1][key])});return module.exports}\nconst validators=load(${JSON.stringify(schemas)});const semver={valid:load(${JSON.stringify(valid)}),validRange:load(${JSON.stringify(validRange)}),satisfies:load(${JSON.stringify(satisfies)})};\n${checker}\nconst inspect=createPackageInspector({validateManifest:validators.manifest,validateIntegrity:validators.integrity,semver});\nreturn Object.freeze({check(manifest,integrity,observed,host,registry){try{const result=inspect(manifest,integrity,observed,host);if(!Array.isArray(registry)||registry.length>64||registry.some(m=>!validators.manifest(m)))throw new PackageCheckError('REGISTRY_UNKNOWN','unknown');const conflicts=commandConflicts([...registry,manifest]).filter(c=>c.first===manifest.id||c.second===manifest.id);const selected=result.side==='client'?manifest.client:manifest.executor.variants.find(v=>v.id===result.variantId);return {ok:true,result,identity:{id:manifest.id,version:manifest.pluginVersion,publisher:manifest.publisher,license:manifest.license,runtime:selected.runtime,requires:selected.requires},associationRequired:conflicts.length>0}}catch(e){if(e instanceof PackageCheckError)return {ok:false,code:e.code,state:e.state};throw e}}});\n})()\n`;
// Smoke the exact closed bundle in a realm without process/require/fs/network.
const api = new vm.Script(bundle).runInNewContext(Object.create(null), { timeout: 5000 });
assert.equal(typeof api.check, 'function');
const smoke = api.check({}, {}, [], {}, []);
assert.equal(smoke.ok, false);assert.equal(smoke.code, 'MANIFEST_SCHEMA');
const bytes = Buffer.from(bundle);
function cString(b) {
  let text = '"';
  for (const c of b) text += c === 34 ? '\\"' : c === 92 ? '\\\\' : c >= 32 && c < 127 ? String.fromCharCode(c) : '\\' + c.toString(8).padStart(3, '0');
  return text + '"';
}
const chunks = [];
for (let at = 0; at < bytes.length; at += 512) chunks.push(cString(bytes.subarray(at, at + 512)));
const output = `/* Generated by scripts/generate-plugin-package-validator.mjs. Do not edit.\n * Trusted metadata-only code; not plugin code. Third-party notices alongside.\n * JS SHA256 ${digest(bytes)} */\n#define GT_PACKAGE_VALIDATOR_BYTES ${bytes.length}u\nstatic const char *const package_validator_chunks[]={\n${chunks.join(',\n')}\n};\n`;
const licenses = ['ajv/LICENSE', 'fast-deep-equal/LICENSE', 'semver/LICENSE'].map(name =>
  `===== ${name} =====\n${fs.readFileSync(path.join(tools, 'node_modules', name), 'utf8').trim()}\n`).join('\n');
const base = 'core/src/extension/package-validator';
const outputs = { [base + '-code.inc']: output, [base + '-LICENSES.txt']: licenses };
const inputs = [...schemaFiles, checkerFile, 'scripts/generate-plugin-package-validator.mjs', 'scripts/fixtures/plugin-package-tools/package.json', 'scripts/fixtures/plugin-package-tools/package-lock.json'];
const manifest = { format: 1, purpose: 'closed metadata validator only; no Node/OS/network services',
  inputs: Object.fromEntries(inputs.map(name => [name, digest(fs.readFileSync(path.join(root, name)))])),
  outputs: Object.fromEntries(Object.entries(outputs).map(([name, data]) => [name, digest(data)])),
  modules: Object.fromEntries([...depFiles].sort(byteOrder)),
  versions: { ajv: '8.17.1', semver: '7.7.3', 'fast-deep-equal': '3.1.3' }, bundleSHA256: digest(bytes), bundleBytes: bytes.length };
outputs[base + '-inputs.json'] = JSON.stringify(manifest, null, 2) + '\n';
for (const [name, data] of Object.entries(outputs)) {
  if (check) assert.equal(fs.readFileSync(path.join(root, name), 'utf8'), data, 'generated validator drift: ' + name);
  else fs.writeFileSync(path.join(root, name), data);
}
console.log(`PACKAGE VALIDATOR ${check ? 'CHECK' : 'GENERATED'} PASS: ${modules.size} closed modules / ${bytes.length} JS bytes`);
