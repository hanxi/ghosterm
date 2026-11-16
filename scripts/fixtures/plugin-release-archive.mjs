// Regenerate public Android ZIP conformance vectors. The disposable private
// KeyObject is never exported, logged, or written; no production signer is used.
import { generateKeyPairSync, sign, createHash } from 'node:crypto';
import { readFileSync, readdirSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { deflateRawSync, crc32 } from 'node:zlib';

const root = fileURLToPath(new URL('../../android/app/src/androidTest/assets/plugin-release/', import.meta.url));
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
const vectors = [
  ['stored', archive(0)], ['deflated', archive(8)], ['identity', archive(0)],
  ['integrity', archive(8, true)], ['crc', archive(8, false, true)],
].map(([name, zip]) => {
  const descriptor = JSON.stringify({ ...base, keyId: keyID,
    packageSHA256: name === 'identity' ? '0'.repeat(64) : base.packageSHA256,
    artifact: { format: 'zip-v1', bytes: zip.length, sha256: hash(zip) } });
  const signature = sign(null, Buffer.concat([Buffer.from('GHOSTERM-PLUGIN-RELEASE-V1\0'), Buffer.from(descriptor)]), privateKey);
  return { name, archive: zip.toString('base64'), descriptor, signature: signature.toString('base64') };
});
process.stdout.write(JSON.stringify({ policy, vectors }) + '\n');
