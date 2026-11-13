// Trusted Node host transport. Import only in the dedicated plugin worker,
// never an App/UI thread. One 16 KiB shared mailbox and one in-flight exchange.
import { Worker } from 'node:worker_threads';
import { performance } from 'node:perf_hooks';
import { Buffer } from 'node:buffer';
import { parseMessage } from '../message.js';

export class NodeStdioPipe {
  #worker;
  #shared;
  #header;
  #bytes;
  #timeout;
  #closed = false;
  #busy = false;
  #termination;
  #readyReject;
  #readyTimer;
  ready;
  constructor({ timeoutMs = 5000 } = {}) {
    if (!Number.isInteger(timeoutMs) || timeoutMs < 1 || timeoutMs > 30000) throw Error('CHILD_PIPE_LIMIT');
    this.#shared = new SharedArrayBuffer(16 + 8192 * 2);
    this.#header = new Int32Array(this.#shared, 0, 4);
    this.#bytes = new Uint8Array(this.#shared);
    this.#timeout = timeoutMs;
    this.#worker = new Worker(new URL('./pipe-worker.mjs', import.meta.url), {
      workerData: { shared: this.#shared }, execArgv: [],
    });
    this.ready = new Promise((resolve, reject) => {
      this.#readyReject = reject;
      this.#readyTimer = setTimeout(() => this.abort(), 5000);
      this.#worker.on('message', value => {
        if (value !== 'ready' || this.#closed) return this.abort();
        clearTimeout(this.#readyTimer); resolve();
      });
    });
    // Synchronous callers can let their first exchange cover startup. Preserve
    // rejection for awaiters without creating an unhandled Promise rejection.
    void this.ready.catch(() => {});
    this.#worker.on('error', () => this.abort());
    this.#worker.on('exit', () => this.abort());
  }
  exchange(json) {
    if (this.#closed || this.#busy || Atomics.load(this.#header, 0) !== 0) throw Error('CHILD_PIPE_CLOSED');
    parseMessage(json);
    const payload = Buffer.from(json, 'utf8');
    if (!payload.length || payload.length > 8192) throw Error('CHILD_PIPE_LIMIT');
    this.#busy = true;
    try {
      this.#bytes.set(payload, 16);
      Atomics.store(this.#header, 1, payload.length);
      Atomics.store(this.#header, 2, 0);
      Atomics.store(this.#header, 0, 1);
      this.#worker.postMessage('exchange');
      const deadline = performance.now() + this.#timeout;
      for (;;) {
        const state = Atomics.load(this.#header, 0);
        if (state === 2) break;
        if (state !== 1) throw Error('CHILD_PIPE_LOST');
        const left = deadline - performance.now();
        if (left <= 0 || Atomics.wait(this.#header, 0, 1, left) === 'timed-out') throw Error('CHILD_PIPE_TIMEOUT');
      }
      const n = Atomics.load(this.#header, 2);
      if (n < 1 || n > 8192) throw Error('CHILD_PIPE_LIMIT');
      const text = new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(this.#bytes.subarray(16 + 8192, 16 + 8192 + n));
      parseMessage(text);
      if (Atomics.load(this.#header, 0) !== 2) throw Error('CHILD_PIPE_LOST');
      this.#bytes.fill(0, 16);
      Atomics.store(this.#header, 0, 0);
      return text;
    } catch (error) { this.abort(); throw error; }
    finally { this.#busy = false; }
  }
  abort() {
    if (this.#closed) return;
    this.#closed = true;
    clearTimeout(this.#readyTimer);
    this.#readyReject(Error('CHILD_PIPE_CLOSED'));
    Atomics.store(this.#header, 0, 3);
    Atomics.notify(this.#header, 0);
    this.#termination = this.#worker.terminate();
  }
  async close() {
    this.abort();
    await this.#termination;
    this.#bytes.fill(0);
  }
}
