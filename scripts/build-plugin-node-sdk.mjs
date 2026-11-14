#!/usr/bin/env node
// Candidate trusted Node host bundle; portable SDK and its generated C identity
// remain unchanged. Developer compiler only, no dependency/runtime downloads.
import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import { createRequire } from 'node:module';
import { createHash } from 'node:crypto';
import { fileURLToPath } from 'node:url';
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const args = process.argv.slice(2);
assert(args.length === 4 && args[0] === '--tools' && args[2] === '--out');
const tools = path.resolve(args[1]), out = path.resolve(args[3]);
const parent = fs.realpathSync(path.dirname(out));
assert(parent === path.join(root, 'tmp') || parent.startsWith(path.join(root, 'tmp') + path.sep), 'use owned tmp output');
assert(!fs.existsSync(out) || (fs.lstatSync(out).isDirectory() && fs.readdirSync(out).length === 0), 'output must be new or empty');
for (const name of ['package.json', 'package-lock.json']) {
  assert.deepEqual(fs.readFileSync(path.join(tools, name)), fs.readFileSync(path.join(root, 'scripts/fixtures/plugin-sdk-tools', name)));
}
const require = createRequire(path.join(tools, 'package.json')), ts = require('typescript');
assert.equal(ts.version, '5.9.3');
const configPath = path.join(root, 'packages/plugin-sdk/node/tsconfig.json');
const config = ts.readConfigFile(configPath, ts.sys.readFile); assert(!config.error);
const parsed = ts.parseJsonConfigFileContent(config.config, ts.sys, path.dirname(configPath), { outDir: out });
const program = ts.createProgram(parsed.fileNames, parsed.options);
const errors = ts.getPreEmitDiagnostics(program);
assert.equal(errors.length, 0, ts.formatDiagnosticsWithColorAndContext(errors, {
  getCurrentDirectory: () => root, getCanonicalFileName: name => name, getNewLine: () => '\n',
}));
const outputs = new Map();
const emit = program.emit(undefined, (name, text) => {
  const relative = path.relative(out, name).split(path.sep).join('/');
  assert(!relative.startsWith('../') && !path.isAbsolute(relative));
  outputs.set(relative, Buffer.from(text));
});
assert(!emit.emitSkipped && !emit.diagnostics.length);
for (const name of ['pipe.mjs', 'pipe-worker.mjs', 'bootstrap.mjs']) outputs.set('node/runtime/' + name,
  fs.readFileSync(path.join(root, 'packages/plugin-sdk/node/runtime', name)));
outputs.set('package.json', Buffer.from('{"type":"module","private":true,"license":"MIT"}\n'));
outputs.set('LICENSE', fs.readFileSync(path.join(root, 'LICENSE')));
const sha = bytes => createHash('sha256').update(bytes).digest('hex');
const records = [...outputs].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0).map(([name, bytes]) => ({ name, bytes: bytes.length, sha256: sha(bytes) }));
const identity = JSON.parse(fs.readFileSync(path.join(root, 'packages/plugin-sdk/sdk-version.json')));
const metadata = { candidate: true, sdkApiVersion: identity.sdkApiVersion, processProtocolCandidate: 1,
  sha256: sha(JSON.stringify(records)), members: records };
outputs.set('node-sdk-build.json', Buffer.from(JSON.stringify(metadata, null, 2) + '\n'));
const catalog = createHash('sha256');
for (const [name, bytes] of [...outputs].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0)) {
  const header = Buffer.alloc(12); header.writeUInt32BE(Buffer.byteLength(name));
  header.writeBigUInt64BE(BigInt(bytes.length), 4);
  catalog.update(header); catalog.update(name); catalog.update(sha(bytes));
}
fs.mkdirSync(out, { recursive: true });
for (const [name, bytes] of outputs) {
  const target = path.join(out, name); fs.mkdirSync(path.dirname(target), { recursive: true });
  fs.writeFileSync(target, bytes, { flag: 'wx' });
}
console.log(`NODE SDK BUILD PASS: ${records.length} members / ${metadata.sha256} / catalog=${catalog.digest('hex')}`);
