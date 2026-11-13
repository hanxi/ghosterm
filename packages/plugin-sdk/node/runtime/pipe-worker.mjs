// Sole stdin/stdout pipe owner. No plugin modules or business callbacks here.
import { parentPort, workerData } from 'node:worker_threads';
import { Socket } from 'node:net';
import { Buffer } from 'node:buffer';

const header = new Int32Array(workerData.shared, 0, 4);
const shared = new Uint8Array(workerData.shared);
// uv pipe polling, not a blocking fs.read queued on the thread pool. The worker
// owns these fds; termination must release an idle read without waiting for EOF.
const input = new Socket({ fd: 0, readable: true, writable: false, highWaterMark: 8196 });
const output = new Socket({ fd: 1, readable: false, writable: true, highWaterMark: 8196 });
let active = false, used = 0, length = 0;
const frame = Buffer.alloc(8196);
function fail() {
  Atomics.store(header, 0, 3); Atomics.notify(header, 0);
  input.destroy(); output.destroy(); parentPort.close();
}
input.on('error', fail); input.on('end', fail); output.on('error', fail);
parentPort.on('message', message => {
  if (message !== 'exchange' || active || Atomics.load(header, 0) !== 1) return fail();
  const n = Atomics.load(header, 1);
  if (n < 1 || n > 8192) return fail();
  active = true;
  const payload = Buffer.alloc(n + 4);
  payload.writeUInt32BE(n, 0); payload.set(shared.subarray(16, 16 + n), 4);
  // At most one bounded frame can be queued. The next exchange cannot begin
  // until this one has a full reply; no unbounded write queue/retry exists.
  output.write(payload, error => { payload.fill(0); if (error) fail(); });
});
input.on('data', chunk => {
  if (!active || Atomics.load(header, 0) !== 1 || chunk.length > frame.length - used) return fail();
  frame.set(chunk, used); used += chunk.length;
  if (used < 4) return;
  length = frame.readUInt32BE(0);
  if (!length || length > 8192 || used > length + 4) return fail();
  if (used !== length + 4) return;
  shared.set(frame.subarray(4, used), 16 + 8192);
  Atomics.store(header, 2, length);
  frame.fill(0); used = length = 0; active = false;
  Atomics.store(header, 0, 2); Atomics.notify(header, 0);
});
parentPort.postMessage('ready');
