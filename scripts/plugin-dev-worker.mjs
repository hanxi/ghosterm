// Locked compiler / metadata worker. Never imports or evaluates plugin modules.
import fs from 'node:fs';
import path from 'node:path';
import { createHash } from 'node:crypto';
import { builtinModules, createRequire } from 'node:module';
import { pathToFileURL } from 'node:url';

function demand(ok, code) { if (!ok) throw new Error(code); }
const request = JSON.parse(fs.readFileSync(process.argv[2], 'utf8'));
const { root, source, output, host, registry } = request;
const require = createRequire(path.join(request.tools, 'package.json'));
const ts = require('typescript'), Ajv = require('ajv/dist/2020').default, semver = require('semver');
demand(ts.version === '5.9.3' && require('ajv/package.json').version === '8.17.1' && require('semver/package.json').version === '7.7.3', 'TOOLS_VERSION');
const { canonicalMemberPath, commandConflicts, createPackageInspector } = await import(pathToFileURL(path.join(root, 'packages/plugin-sdk/testkit/package-metadata.mjs')));
const digest = value => createHash('sha256').update(value).digest('hex');
const ajv = new Ajv({ strict: true, allErrors: true, validateFormats: false });
ajv.addKeyword({ keyword: 'x-ghosterm-cli', schemaType: 'object', valid: true });
for (const name of ['bridge-v1.candidate.json', 'manifest-v1.candidate.json', 'integrity-v1.candidate.json'])
  ajv.addSchema(JSON.parse(fs.readFileSync(path.join(root, 'packages/plugin-sdk/schema', name), 'utf8')));
const validateManifest = ajv.getSchema('https://ghosterm.invalid/plugin-schema/manifest-v1.candidate.json');
const inspect = createPackageInspector({ validateManifest, validateIntegrity: ajv.getSchema('https://ghosterm.invalid/plugin-schema/integrity-v1.candidate.json'), semver });
const nodeBuiltin = name => name.startsWith('node:') && (builtinModules.includes(name) || builtinModules.includes(name.slice(5)));
const contained = (parent, file) => file.startsWith(parent + path.sep);

function json(filename) {
  // Python already validated the complete fixed snapshot, including duplicate
  // keys and Unicode. Keep compiler/metadata reads on these copied bytes.
  return JSON.parse(fs.readFileSync(filename, 'utf8'));
}
function files(directory) {
  const result = new Map();
  function walk(current) {
    for (const item of fs.readdirSync(current, { withFileTypes: true })) {
      const filename = path.join(current, item.name);
      demand(!item.isSymbolicLink(), 'SYMLINK');
      if (item.isDirectory()) walk(filename);
      else { demand(item.isFile(), 'NONREGULAR_MEMBER'); result.set(canonicalMemberPath(path.relative(directory, filename).split(path.sep).join('/')), fs.readFileSync(filename)); }
    }
  }
  walk(directory); return result;
}
function imports(filename, text) {
  const tree = ts.createSourceFile(filename, text, ts.ScriptTarget.ES2022, true);
  demand(!tree.referencedFiles.length && !tree.typeReferenceDirectives.length && !tree.libReferenceDirectives.length, 'REFERENCE_DIRECTIVE');
  const values = [];
  function visit(node) {
    demand(node.kind !== ts.SyntaxKind.AnyKeyword, 'EXPLICIT_ANY');
    if (ts.isCallExpression(node)) demand(node.expression.kind !== ts.SyntaxKind.ImportKeyword && !(ts.isIdentifier(node.expression) && node.expression.text === 'require'), 'DYNAMIC_OR_REQUIRE_IMPORT');
    demand(!ts.isImportEqualsDeclaration(node), 'IMPORT_EQUALS');
    if (ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) {
      if (node.moduleSpecifier) { demand(ts.isStringLiteral(node.moduleSpecifier), 'IMPORT_LITERAL'); values.push(node.moduleSpecifier.text); }
    }
    if (ts.isImportTypeNode(node)) {
      demand(ts.isLiteralTypeNode(node.argument) && ts.isStringLiteral(node.argument.literal), 'IMPORT_TYPE_LITERAL');
      values.push(node.argument.literal.text);
    }
    ts.forEachChild(node, visit);
  }
  demand(!/@ts-(?:ignore|nocheck)\b/.test(text), 'TYPECHECK_BYPASS');
  visit(tree); return values;
}
function project() {
  const config = json(path.join(source, 'plugin.project.json'));
  demand(Object.keys(config).sort().join(',') === 'assets,schemaVersion,sources' && config.schemaVersion === 1 && Array.isArray(config.sources) && Array.isArray(config.assets), 'PROJECT_CONFIG');
  demand(config.sources.length > 0 && config.sources.length <= 64 && config.assets.length <= 1024 &&
    [...config.sources, ...config.assets].every(n => typeof n === 'string') && new Set([...config.sources, ...config.assets]).size === config.sources.length + config.assets.length, 'PROJECT_MEMBERS');
  const manifest = json(path.join(source, 'plugin.json'));
  demand(validateManifest(manifest) && manifest.client?.runtime === 'portable-js', 'BUILD_MANIFEST');
  const sides = [manifest.client, ...(manifest.executor?.variants ?? [])];
  const allSeen = new Set(), emitted = new Map();
  fs.mkdirSync(output);
  for (const side of sides) {
    if (side.runtime === 'exec') continue;
    const entrySource = side.entry.replace(/\.js$/, '.ts');
    if (!config.sources.includes(entrySource)) {
      demand(side.runtime === 'node' && config.assets.includes(side.entry), 'ENTRY_SOURCE');
      continue;
    }
    const seen = new Set();
    function collect(name) {
      canonicalMemberPath(name); demand(name.endsWith('.ts') && !name.endsWith('.d.ts'), 'TYPESCRIPT_SOURCE');
      if (seen.has(name)) return;
      demand(seen.size < 64, 'SOURCE_MODULE_COUNT'); seen.add(name); allSeen.add(name);
      const filename = path.resolve(source, name); demand(contained(source, filename), 'SOURCE_ESCAPE');
      const data = fs.readFileSync(filename, 'utf8'); demand(Buffer.byteLength(data) <= 131072, 'SOURCE_MODULE_BYTES');
      for (const specifier of imports(filename, data)) {
        if (specifier === '@ghosterm/plugin-sdk') continue;
        if (side.runtime === 'node' && nodeBuiltin(specifier)) continue;
        demand(specifier.startsWith('.') && !specifier.includes('\\'), 'PORTABLE_IMPORT');
        collect(path.relative(source, path.resolve(path.dirname(filename), specifier.replace(/\.js$/, '.ts'))).split(path.sep).join('/'));
      }
    }
    collect(entrySource);
    const options = { target: ts.ScriptTarget.ES2022, module: ts.ModuleKind.ES2022, moduleResolution: ts.ModuleResolutionKind.Bundler,
      lib: ['lib.es2022.d.ts'], types: side.runtime === 'node' ? ['node'] : [],
      typeRoots: [path.join(request.tools, 'node_modules/@types')],
      strict: true, noUncheckedIndexedAccess: true, exactOptionalPropertyTypes: true,
      noUnusedLocals: true, noUnusedParameters: true, noEmitOnError: true, baseUrl: source,
      paths: { '@ghosterm/plugin-sdk': [path.join(root, 'packages/plugin-sdk/portable/index.ts')] } };
    if (side.runtime === 'node') demand(require('@types/node/package.json').version === '22.19.7', 'NODE_TYPES_VERSION');
    const program = ts.createProgram([...seen].map(n => path.join(source, n)), options);
    const diagnostics = ts.getPreEmitDiagnostics(program);
    demand(!diagnostics.length, 'TYPESCRIPT: ' + diagnostics.map(d => ts.flattenDiagnosticMessageText(d.messageText, '\n')).join('\n'));
    const outputs = new Set();
    const result = program.emit(undefined, (_name, text, _bom, _error, origins) => {
      if (!origins || origins.length !== 1 || !contained(source, origins[0].fileName)) return;
      const relative = path.relative(source, origins[0].fileName).replace(/\.ts$/, '.js').split(path.sep).join('/');
      canonicalMemberPath(relative); demand(!outputs.has(relative) && Buffer.byteLength(text) <= 131072, 'EMITTED_MODULE'); outputs.add(relative);
      demand(!emitted.has(relative) || emitted.get(relative) === text, 'SHARED_EMIT_MISMATCH'); emitted.set(relative, text);
    });
    demand(!result.emitSkipped && !result.diagnostics.length && outputs.size === seen.size, 'EMIT_FAILED');
  }
  demand(config.sources.every(n => allSeen.has(n)) && allSeen.size <= 64, 'UNREACHABLE_SOURCE');
  for (const [name, text] of emitted) {
    const filename = path.join(output, name); fs.mkdirSync(path.dirname(filename), { recursive: true }); fs.writeFileSync(filename, text);
  }
  for (const name of ['plugin.json', ...config.assets]) {
    canonicalMemberPath(name); demand(!emitted.has(name) && name !== 'integrity.json' && name !== 'signature', 'ASSET_COLLISION');
    const target = path.join(output, name); fs.mkdirSync(path.dirname(target), { recursive: true }); fs.copyFileSync(path.join(source, name), target);
  }
  const catalog = files(output);
  const integrity = { schemaVersion: 1, members: [...catalog].sort(([a], [b]) => a < b ? -1 : a > b ? 1 : 0).map(([name, bytes]) => ({ path: name, bytes: bytes.length, sha256: digest(bytes) })) };
  fs.writeFileSync(path.join(output, 'integrity.json'), JSON.stringify(integrity) + '\n');
}

try {
  if (output) project();
  const directory = output ?? source, catalog = files(directory);
  const manifest = json(path.join(directory, 'plugin.json')), integrity = json(path.join(directory, 'integrity.json'));
  const sdkDirectory = path.join(root, 'packages/plugin-sdk');
  const sdkConfig = ts.readConfigFile(path.join(sdkDirectory, 'tsconfig.json'), ts.sys.readFile);
  demand(!sdkConfig.error, 'SDK_CONFIG');
  const sdkOptions = ts.parseJsonConfigFileContent(sdkConfig.config, ts.sys, sdkDirectory);
  const sdkProgram = ts.createProgram(sdkOptions.fileNames, sdkOptions.options), sdkModules = new Map();
  demand(!ts.getPreEmitDiagnostics(sdkProgram).length, 'SDK_TYPES');
  const sdkEmit = sdkProgram.emit(undefined, (filename, text) => { if (filename.endsWith('.js')) sdkModules.set(path.basename(filename), text); });
  demand(!sdkEmit.emitSkipped, 'SDK_EMIT');
  const schemas = Object.create(null);
  demand(validateManifest(manifest), 'MANIFEST_SCHEMA');
  for (const command of manifest.commands) {
    canonicalMemberPath(command.argsSchema);
    const schema = schemas[command.argsSchema] = json(path.join(directory, command.argsSchema));
    ajv.compile(schema);
  }
  demand(manifest.client?.runtime === 'portable-js', 'CLIENT_PORTABLE_ONLY');
  // Check the emitted import graph using the TypeScript parser, never regexes.
  // SDK imports are resolved by the real Core during native test activation.
  const visited = new Set(), active = new Set();
  function closure(name, sdk = false, runtime = 'portable-js') {
    canonicalMemberPath(name); demand((sdk ? sdkModules : catalog).has(name) && /\.(?:mjs|js)$/.test(name), 'MISSING_SCRIPT');
    const key = (sdk ? 'sdk:' : 'plugin:') + name;
    demand(!active.has(key), 'IMPORT_CYCLE'); if (visited.has(key)) return;
    active.add(key); visited.add(key);
    const body = sdk ? sdkModules.get(name) : catalog.get(name).toString('utf8');
    for (const specifier of sdk ? (() => {
      const tree = ts.createSourceFile(name, body, ts.ScriptTarget.ES2022, true); const result = [];
      for (const node of tree.statements) if ((ts.isImportDeclaration(node) || ts.isExportDeclaration(node)) && node.moduleSpecifier) result.push(node.moduleSpecifier.text);
      return result;
    })() : imports(name, body)) {
      if (specifier === '@ghosterm/plugin-sdk' && !sdk) { closure('index.js', true, runtime); continue; }
      if (!sdk && runtime === 'node' && nodeBuiltin(specifier)) continue;
      demand(specifier.startsWith('.') && !specifier.includes('\\'), 'PORTABLE_IMPORT');
      closure(path.posix.normalize(path.posix.join(path.posix.dirname(name), specifier)), sdk, runtime);
    }
    active.delete(key);
  }
  for (const side of [manifest.client, ...(manifest.executor?.variants ?? [])]) {
    if (side.runtime === 'exec') {
      demand(catalog.has(side.executable) && catalog.get(side.executable).length > 0, 'EXECUTABLE_MEMBER');
      continue;
    }
    demand(side.runtime === 'portable-js' || side.runtime === 'node', 'CHECK_RUNTIME');
    if (side.runtime === 'node') demand(catalog.has('package.json') && json(path.join(directory, 'package.json')).type === 'module', 'NODE_ESM_PACKAGE');
    visited.clear(); active.clear(); closure(side.entry, false, side.runtime);
    demand(visited.size <= 64 && [...visited].reduce((n, name) => n + (name.startsWith('sdk:') ? Buffer.byteLength(sdkModules.get(name.slice(4))) : catalog.get(name.slice(7)).length), 0) <= 1048576, 'RUNTIME_MODULE_LIMIT');
  }
  const conflict = commandConflicts([manifest, ...registry.map(dir => json(path.join(dir, 'plugin.json')))]);
  demand(conflict.length === 0, 'COMMAND_CONFLICT');
  const observed = [...catalog].map(([name, bytes]) => ({ path: name, bytes: bytes.length, sha256: digest(bytes), kind: 'regular' }));
  let compatibility = 'compatible', facts, reason;
  try { facts = inspect(manifest, integrity, observed, host, schemas); }
  catch (error) {
    if (['unknown', 'unsupported', 'denied'].includes(error.state)) { compatibility = error.state; reason = error.code; }
    else throw error;
  }
  if (host && request.testProfile) {
    const selected = host.side === 'client' ? manifest.client :
      manifest.executor?.variants?.find(v => v.id === facts?.variantId) ?? manifest.executor?.variants?.[0];
    demand(selected?.requires.includes('session.rpc'), 'TEST_REQUIRES_DECLARED_RPC');
    if (request.testResources) demand(['resource.receive', ...(host.side === 'executor' ? ['file.readScoped'] : [])].every(c => selected.requires.includes(c)), 'TEST_REQUIRES_DECLARED_RESOURCES');
    if (request.testViews && host.side === 'client') demand(request.testViews.kinds.every(k => selected.requires.includes('ui.' + k)), 'TEST_REQUIRES_DECLARED_VIEWS');
    if (request.testProcesses && host.side === 'executor') demand(['process.spawn', 'process.pipes'].every(c => selected.requires.includes(c)), 'TEST_REQUIRES_DECLARED_PROCESSES');
    if (compatibility === 'compatible' && selected.runtime !== 'portable-js') {
      if (request.testRuntime !== selected.runtime) {
        compatibility = 'unsupported'; reason = 'TEST_RUNTIME_UNSUPPORTED';
      }
    }
  }
  console.log(JSON.stringify({ structural: true, compatibility, reason, exitCode: { compatible: 0, unknown: 3, unsupported: 4, denied: 6 }[compatibility],
    runtimes: [...new Set([manifest.client.runtime, ...(manifest.executor?.variants ?? []).map(v => v.runtime)])],
    id: manifest.id, packageSHA256: digest(catalog.get('integrity.json')), authorization: 'not-granted', publisherAuthenticated: false, facts }));
} catch (error) {
  console.error(String(error.message)); process.exitCode = 2;
}
