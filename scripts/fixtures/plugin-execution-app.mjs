// Owned no-account conformance Executor, never a business plugin.
import { object, text, integer } from '@ghosterm/plugin-sdk';
import { writeFileSync } from 'node:fs';
import { createHash } from 'node:crypto';
import { appProcess } from './process.mjs';
const observation = process.env.GT_EXECUTION_APP_FIXTURE;
if (!observation || process.env.GT_EXECUTION_AMBIENT !== undefined) throw Error('fixture environment');
writeFileSync(observation + '/entered', String(process.pid), { flag: 'wx' });
writeFileSync(observation + '/runtime-tty', JSON.stringify([Boolean(process.stdin.isTTY), Boolean(process.stdout.isTTY), Boolean(process.stderr.isTTY)]), { flag: 'wx' });
writeFileSync(observation + '/environment', process.env.TMPDIR, { flag: 'wx' });
writeFileSync(observation + '/activated', String(process.pid), { flag: 'wx' });
export function activate(ctx) {
  if (ctx.invocation.command !== 'fixture' || ctx.invocation.arguments.length !== 3 ||
      ctx.invocation.arguments[0] !== '中文😀' || ctx.invocation.arguments[1] !== '' ||
      ctx.invocation.arguments[2] !== "literal'$(owned)") throw Error('fixture argv');
  const codec = object({ text: text(512), count: integer(0, 1048576) });
  ctx.session.register({ name: 'fixture.echo', input: codec, output: codec }, async (value, call) => {
    call.check();
    if (value.text === 'CLIENT_RESOURCE') {
      let incoming;
      for (;;) {
        call.check(); incoming = ctx.resources.take();
        if (incoming.status !== 'wait') break;
        await new Promise(resolve => setTimeout(resolve, 1));
      }
      if (incoming.status !== 'ok' || !incoming.ref) throw Error('Client resource missing');
      const ref = incoming.ref;
      if (ref.displayName !== 'upload.bin' || ref.mediaType !== 'application/octet-stream' ||
          ref.size.kind !== 'known' || ref.size.bytes !== '262144') throw Error('Client metadata');
      const opened = ctx.resources.open(ref, { byteLimit: '262144', timeoutMs: 10000 });
      if (opened.status !== 'ok' || !opened.stream) throw Error('Client open');
      const hash = createHash('sha256'); let count = 0;
      for (;;) {
        call.check(); const read = await opened.stream.read();
        if (read.kind !== 'success') throw Error('Client read');
        const bytes = read.value.bytes;
        for (let i = 0; i < bytes.length; i++) if (bytes[i] !== ((count + i) * 7 + 3) % 251) throw Error('Client byte mismatch');
        count += bytes.length; hash.update(bytes);
        if (read.value.done) break;
      }
      const digest = hash.digest('hex');
      if (count !== 262144 || digest !== ref.sha256 || ctx.resources.release(ref) !== 'ok' ||
          ctx.resources.release(ref) !== 'invalid') throw Error('Client receipt/release');
      writeFileSync(observation + '/client-received', digest + ' ' + count, { flag: 'wx' });
    }
    if (value.text.startsWith('PROCESS')) return await appProcess(ctx, value.text, observation);
    if (value.text === 'WAIT') {
      writeFileSync(observation + '/handler', 'started');
      return await new Promise(() => {});
    }
    if (value.text.startsWith('RESOURCE') || value.text.startsWith('CLIENT_')) {
      if (!ctx.resources) throw Error('resource service missing');
      const selected = value.text.startsWith('CLIENT_') ? ['workspace', 'file.bin', null] : { RESOURCE: ['workspace', 'file.bin', null], RESOURCE_ROOT: ['outside', 'file.bin', 'DENIED'],
        RESOURCE_PARENT: ['workspace', '../outside.bin', 'DENIED'], RESOURCE_LINK: ['workspace', 'link.bin', 'UNSUPPORTED'] }[value.text];
      if (!selected) throw Error('resource case');
      const [root, relativePath, error] = selected;
      const result = await ctx.resources.publish({ root, relativePath,
        mediaType: 'application/octet-stream' }, { byteLimit: '262144', timeoutMs: 10000 }).outcome;
      writeFileSync(observation + '/resource-result', JSON.stringify(result), { flag: 'wx' });
      if (error) {
        if (result.kind !== 'failed' || result.error.code !== error || result.error.effect !== 'none') throw Error('resource rejection');
        writeFileSync(observation + '/resource-error', error, { flag: 'wx' });
      } else {
        if (result.kind !== 'success' || result.value.size.kind !== 'known' ||
            result.value.size.bytes !== '262144') throw Error('resource publication failed');
        writeFileSync(observation + '/resource-receipt', result.value.sha256 + ' ' + result.value.size.bytes,
          { flag: 'wx' });
      }
    }
    return value;
  });
  return { dispose() {} };
}
