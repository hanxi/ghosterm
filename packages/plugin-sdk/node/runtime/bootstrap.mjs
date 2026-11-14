// Host-owned candidate bootstrap. Native controller verifies and stages this
// whole SDK tree; no plugin entry/argv is read from an original mutable package.
import { openSync, closeSync, readSync, fstatSync, realpathSync, constants } from 'node:fs';
import { createHash } from 'node:crypto';
import * as modules from 'node:module';
import { pathToFileURL, fileURLToPath } from 'node:url';
import path from 'node:path';
import { Console } from 'node:console';
import { setImmediate } from 'node:timers/promises';
import { NodeNativePort } from '../port.js';
import { NodeStdioPipe } from './pipe.mjs';
import { SessionChannel, snapshot, uint64 } from '../../portable/index.js';

let pipe, session, disposable, stopping = false;
process.on('SIGTERM', () => { stopping = true; });
process.on('SIGINT', () => { stopping = true; });
try {
  if (typeof modules.registerHooks !== 'function') { process.exitCode = 77; throw Error('UNSUPPORTED'); }
  if (process.argv.length !== 4 || !/^[0-9a-f]{64}$/.test(process.argv[3])) throw Error('STARTUP');
  const fd = openSync(process.argv[2], constants.O_RDONLY | constants.O_NOFOLLOW);
  let config;
  try {
    const stat = fstatSync(fd);
    if (!stat.isFile() || stat.size < 1 || stat.size > 262144) throw Error('STARTUP');
    const bytes = Buffer.alloc(stat.size); let used = 0;
    while (used < bytes.length) {
      const n = readSync(fd, bytes, used, bytes.length - used, null);
      if (!n) throw Error('STARTUP'); used += n;
    }
    if (createHash('sha256').update(bytes).digest('hex') !== process.argv[3]) throw Error('STARTUP');
    config = JSON.parse(new TextDecoder('utf-8', { fatal: true, ignoreBOM: true }).decode(bytes));
    bytes.fill(0);
  } finally { closeSync(fd); }
  if (config.v !== 1 || config.sdkVersion !== '1.0.0' || !/^[0-9a-f]{32}$/.test(config.instanceId) ||
      uint64(config.epoch, true) !== config.epoch || !path.isAbsolute(config.packageRoot) ||
      !path.isAbsolute(config.entry)) throw Error('STARTUP');
  const root = realpathSync(config.packageRoot), sdkRoot = realpathSync(path.join(path.dirname(fileURLToPath(import.meta.url)), '../..'));
  const within = (file, base) => file === base || file.startsWith(base + path.sep);
  if (!within(realpathSync(config.entry), root)) throw Error('ENTRY');
  modules.registerHooks({ resolve(specifier, context, next) {
    if (specifier === '@ghosterm/plugin-sdk') specifier = pathToFileURL(path.join(sdkRoot, 'portable/index.js')).href;
    const resolved = next(specifier, context);
    if (resolved.url.startsWith('node:')) return resolved;
    if (!resolved.url.startsWith('file:')) throw Error('MODULE_SCOPE');
    const file = realpathSync(fileURLToPath(resolved.url));
    if (!within(file, root) && !within(file, sdkRoot)) throw Error('MODULE_SCOPE');
    return resolved;
  } });
  // Protocol stdout is exclusively owned by the pipe worker. This is routing,
  // not a sandbox against trusted plugin code accessing the raw OS descriptors.
  globalThis.console = new Console({ stdout: process.stderr, stderr: process.stderr });
  pipe = new NodeStdioPipe(); await pipe.ready;
  const port = new NodeNativePort(pipe); session = new SessionChannel(port);
  const context = Object.freeze({ instanceId: config.instanceId, epoch: config.epoch, side: 'executor',
    invocation: snapshot(config.invocation), session,
    capabilities: Object.freeze({ query: name => port.access(name) }),
    cancellation: Object.freeze({ get cancelled() {
      if (stopping || port.broken) return true;
      try { return port.access('session.rpc') === 'revoked'; } catch { return true; }
    } }),
  });
  const plugin = await import(pathToFileURL(config.entry).href);
  if (typeof plugin.activate !== 'function') throw Error('ACTIVATE');
  let active = false, failed = false, announced = false;
  void Promise.resolve().then(() => plugin.activate(context)).then(value => {
    if (!value || typeof value.dispose !== 'function' || (value.step !== undefined && typeof value.step !== 'function')) {
      failed = true; return;
    }
    disposable = value; active = true;
  }, () => { failed = true; });
  while (!context.cancellation.cancelled) {
    if (failed) throw Error('ACTIVATE');
    session.step();
    if (active && !announced) { port.activated(); announced = true; }
    if (active) disposable.step?.();
    await setImmediate();
  }
} catch {
  if (process.exitCode !== 77) process.exitCode = 1;
} finally {
  try { session?.dispose(); } catch { process.exitCode ||= 1; }
  try { disposable?.dispose(); } catch { process.exitCode ||= 1; }
  await pipe?.close();
}
