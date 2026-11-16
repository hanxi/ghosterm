// Regenerate public Android ZIP conformance vectors. The disposable private
// KeyObject is never exported, logged, or written; no production signer is used.
import { generateKeyPairSync, sign, createHash } from 'node:crypto';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { deflateRawSync, crc32 } from 'node:zlib';

const root = fileURLToPath(new URL('../../android/app/src/androidTest/assets/plugin-release/', import.meta.url));
const capabilityChanges = process.argv.includes('--capability-versions');
const versionFamily = process.argv.includes('--versions') || capabilityChanges;
if (capabilityChanges && process.argv.includes('--versions')) throw new Error('Choose one version fixture family');
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const base = JSON.parse(JSON.parse(readFileSync(root + 'controller.json', 'utf8')).descriptor);
const members = [];
function walk(path, relative = '') {
  for (const entry of readdirSync(path, { withFileTypes: true }).sort((a, b) => a.name.localeCompare(b.name, 'en'))) {
    const name = relative + entry.name;
    if (entry.isDirectory()) walk(path + entry.name + '/', name + '/');
    else if (entry.isFile()) members.push([name, readFileSync(path + entry.name)]);
    else throw new Error('fixture must contain only ordinary files/directories');
  }
}
walk(root + 'package/');
if (process.argv.includes('--declarations') || versionFamily) {
  const manifest = JSON.parse(members.find(([name]) => name === 'plugin.json')[1]);
  manifest.executor = { variants: [{ id: 'portable', runtime: 'portable-js', entry: 'client/index.js', requires: ['process.spawn'],
    dependencies: [{ program: 'fixture-worker', versionPolicy: 'range', versionRange: '^1.2.0' }] }] };
  manifest.permissions = [
    { side: 'client', capability: 'session.rpc', purpose: 'Review the selected ZIP package without starting a session.', scope: 'instance' },
    { side: 'executor', capability: 'process.spawn', purpose: 'Run only the separately authorized fixture worker for this instance.', scope: 'instance', programs: ['fixture-worker'] },
  ];
  members.find(([name]) => name === 'plugin.json')[1] = Buffer.from(JSON.stringify(manifest));
  const integrity = JSON.parse(members.find(([name]) => name === 'integrity.json')[1]);
  for (const member of integrity.members) {
    const data = members.find(([name]) => name === member.path)[1];
    member.bytes = data.length; member.sha256 = hash(data);
  }
  const bytes = Buffer.from(JSON.stringify(integrity));
  members.find(([name]) => name === 'integrity.json')[1] = bytes;
  base.packageSHA256 = hash(bytes);
}
function archive(method, corruptIntegrity = false, corruptCRC = false) {
  let offset = 0;
  const locals = [], central = [];
  for (const [path, original] of members) {
    const bytes = corruptIntegrity && path === 'README' ? Buffer.from('changed fixture') : original;
    const raw = Buffer.from(path), data = method === 8 ? deflateRawSync(bytes) : bytes;
    const crc = (crc32(bytes) ^ (corruptCRC && path === 'README' ? 1 : 0)) >>> 0;
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50); local.writeUInt16LE(20, 4); local.writeUInt16LE(0x800, 6);
    local.writeUInt16LE(method, 8); local.writeUInt32LE(crc, 14); local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(bytes.length, 22); local.writeUInt16LE(raw.length, 26);
    const header = Buffer.alloc(46);
    header.writeUInt32LE(0x02014b50); header.writeUInt16LE(0x314, 4); header.writeUInt16LE(20, 6);
    header.writeUInt16LE(0x800, 8); header.writeUInt16LE(method, 10); header.writeUInt32LE(crc, 16);
    header.writeUInt32LE(data.length, 20); header.writeUInt32LE(bytes.length, 24); header.writeUInt16LE(raw.length, 28);
    header.writeUInt32LE((0o100444 * 65536) >>> 0, 38); header.writeUInt32LE(offset, 42);
    locals.push(local, raw, data); central.push(header, raw); offset += local.length + raw.length + data.length;
  }
  const directory = Buffer.concat(central), end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50); end.writeUInt16LE(members.length, 8); end.writeUInt16LE(members.length, 10);
  end.writeUInt32LE(directory.length, 12); end.writeUInt32LE(offset, 16);
  return Buffer.concat([...locals, directory, end]);
}
const { privateKey, publicKey } = generateKeyPairSync('ed25519');
const publicBytes = Buffer.from(publicKey.export({ format: 'jwk' }).x, 'base64url');
const keyID = hash(publicBytes);
const policy = JSON.stringify({ schemaVersion: 1, channel: 'fixture', keys: [{ id: keyID, algorithm: 'ed25519',
  publicKey: publicBytes.toString('base64'), publisher: 'fixture', pluginIds: [base.pluginId] }], revokedKeyIds: [] });
let inputs;
if (versionFamily) {
  // Two actual Core package digests under one disposable public policy.
  // --versions writes versions.json; --capability-versions writes a separate
  // capability-versions.json. Keep the original ZIP vector family unchanged.
  if (process.argv.includes('--declarations')) throw new Error('Choose one fixture family');
  inputs = [];
  for (const version of ['1.0.0', '2.0.0']) {
    const manifest = JSON.parse(members.find(([name]) => name === 'plugin.json')[1]);
    manifest.pluginVersion = version;
    manifest.permissions[0].purpose = `Allow fixture RPC for version ${version}.`;
    manifest.permissions[1].programs = [version === '1.0.0' ? 'fixture-worker' : 'fixture-worker-v2'];
    if (capabilityChanges) {
      const capability = version === '1.0.0' ? 'ui.diff' : 'ui.table';
      manifest.client.requires = ['session.rpc', capability];
      manifest.client.optional = ['ui.chat'];
      manifest.permissions = manifest.permissions.filter(permission => permission.capability !== 'ui.diff' && permission.capability !== 'ui.table');
      manifest.permissions.push({ side: 'client', capability, purpose: `Review ${capability} for version ${version}.`, scope: 'instance' });
      manifest.executor.variants[0].id = version === '1.0.0' ? 'portable' : 'portable-reviewed';
    }
    const entry = capabilityChanges ? `throw new Error('SELECTION_PREVIEW_EXECUTED');
export function activate(){throw new Error('SELECTION_PREVIEW_ACTIVATED');}
` : `import {object,text,integer} from '@ghosterm/plugin-sdk';
export function activate(ctx){
  const codec=object({text:text(40),count:integer(0,100)});
  const echo={name:'fixture.echo',input:codec,output:codec};
  const done={name:'fixture.done',input:codec,output:codec};
  ctx.session.request(echo,{text:'bridge',count:17},10000).outcome.then(result=>{
    if(result.kind==='success'&&result.value.text==='bridge'&&result.value.count===17)
      ctx.session.request(done,result.value,10000);
  });
  return {dispose(){}};
}// version ${version}\n`;
    members.find(([name]) => name === 'plugin.json')[1] = Buffer.from(JSON.stringify(manifest));
    members.find(([name]) => name === 'client/index.js')[1] = Buffer.from(entry);
    const integrity = JSON.parse(members.find(([name]) => name === 'integrity.json')[1]);
    for (const member of integrity.members) {
      const bytes = members.find(([name]) => name === member.path)[1];
      member.bytes = bytes.length; member.sha256 = hash(bytes);
    }
    const integrityBytes = Buffer.from(JSON.stringify(integrity));
    members.find(([name]) => name === 'integrity.json')[1] = integrityBytes;
    inputs.push([version, archive(8), { ...base, pluginVersion: version, packageSHA256: hash(integrityBytes) }]);
  }
  inputs.push(['identity', inputs[0][1], { ...inputs[0][2], pluginVersion: '9.0.0' }]);
} else inputs = [
  ['stored', archive(0)], ['deflated', archive(8)], ['identity', archive(0)],
  ['integrity', archive(8, true)], ['crc', archive(8, false, true)],
];
const vectors = inputs.map(([name, zip, version = base]) => {
  const descriptor = JSON.stringify({ ...version, keyId: keyID,
    packageSHA256: name === 'identity' && !versionFamily ? '0'.repeat(64) : version.packageSHA256,
    artifact: { format: 'zip-v1', bytes: zip.length, sha256: hash(zip) } });
  const signature = sign(null, Buffer.concat([Buffer.from('GHOSTERM-PLUGIN-RELEASE-V1\0'), Buffer.from(descriptor)]), privateKey);
  return { name, archive: zip.toString('base64'), descriptor, signature: signature.toString('base64') };
});
process.stdout.write(JSON.stringify({ policy, vectors }) + '\n');
