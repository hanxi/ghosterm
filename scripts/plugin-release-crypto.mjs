// Host developer tool only. No plugin modules or private keys are loaded.
import { readFileSync } from 'node:fs';
import { createPublicKey, verify } from 'node:crypto';

const request = JSON.parse(readFileSync(process.argv[2], 'utf8'));
const raw = Buffer.from(request.publicKey, 'base64');
const signature = Buffer.from(request.signature, 'base64');
const payload = Buffer.from(request.payload, 'base64');
if (raw.length !== 32 || signature.length !== 64 || payload.length > 65536) throw new Error('RELEASE_CRYPTO_INPUT');
const key = createPublicKey({ key: { kty: 'OKP', crv: 'Ed25519', x: raw.toString('base64url') }, format: 'jwk' });
const message = Buffer.concat([Buffer.from('GHOSTERM-PLUGIN-RELEASE-V1\0', 'ascii'), payload]);
process.stdout.write(JSON.stringify({ valid: verify(null, message, key, signature) }));
