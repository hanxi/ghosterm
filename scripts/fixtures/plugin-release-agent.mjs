// Disposable test signer. Private KeyObjects stay in this process and are never exported.
import { generateKeyPairSync, sign } from 'node:crypto';
import { createServer, createConnection } from 'node:net';
import { chmodSync, writeFileSync } from 'node:fs';

const [mode, endpoint, marker] = process.argv.slice(2);
if (mode === 'server') {
  const { privateKey, publicKey } = generateKeyPairSync('ed25519');
  const server = createServer({ allowHalfOpen: true }, socket => {
    const chunks = []; let bytes = 0;
    socket.on('data', data => { bytes += data.length; if (bytes > 65564) socket.destroy(); else chunks.push(data); });
    socket.on('end', () => { if (!socket.destroyed) socket.end(sign(null, Buffer.concat(chunks), privateKey)); });
    socket.on('error', () => {});
  });
  server.listen(endpoint, () => {
    chmodSync(endpoint, 0o600);
    const jwk = publicKey.export({ format: 'jwk' });
    process.stdout.write(Buffer.from(jwk.x, 'base64url').toString('base64') + '\n');
  });
} else if (mode === 'client') {
  if (marker) writeFileSync(marker, 'entered');
  const socket = createConnection(endpoint);
  const chunks = [];
  process.stdin.on('data', data => chunks.push(data));
  process.stdin.on('end', () => socket.end(Buffer.concat(chunks)));
  socket.on('data', data => process.stdout.write(data));
  socket.on('error', () => { process.exitCode = 1; });
} else if (mode === 'noise') {
  process.stdout.write(Buffer.alloc(4096));
} else if (mode === 'diagnostic') {
  process.stderr.write('DO_NOT_LOG_SIGNER_SECRET');
  process.exitCode = 1;
} else if (mode === 'hold') {
  process.stdin.resume();
  process.stdin.on('end', () => { writeFileSync(endpoint, String(process.pid)); setInterval(() => {}, 1000); });
} else {
  throw new Error('test signer mode');
}
