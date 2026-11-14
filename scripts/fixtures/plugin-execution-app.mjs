// Owned no-account conformance Executor, never a business plugin.
import { object, text, integer } from '@ghosterm/plugin-sdk';
import { writeFileSync } from 'node:fs';
const observation = process.env.GT_EXECUTION_APP_FIXTURE;
if (!observation || process.env.GT_EXECUTION_AMBIENT !== undefined) throw Error('fixture environment');
writeFileSync(observation + '/entered', String(process.pid), { flag: 'wx' });
writeFileSync(observation + '/environment', process.env.TMPDIR, { flag: 'wx' });
writeFileSync(observation + '/activated', String(process.pid), { flag: 'wx' });
export function activate(ctx) {
  if (ctx.invocation.command !== 'fixture' || ctx.invocation.arguments.length !== 3 ||
      ctx.invocation.arguments[0] !== '中文😀' || ctx.invocation.arguments[1] !== '' ||
      ctx.invocation.arguments[2] !== "literal'$(owned)") throw Error('fixture argv');
  const codec = object({ text: text(512), count: integer(0, 1000) });
  ctx.session.register({ name: 'fixture.echo', input: codec, output: codec }, async (value, call) => {
    call.check();
    if (value.text === 'WAIT') {
      writeFileSync(observation + '/handler', 'started');
      return await new Promise(() => {});
    }
    if (value.text.startsWith('RESOURCE')) {
      if (!ctx.resources) throw Error('resource service missing');
      const selected = { RESOURCE: ['workspace', 'file.bin', null], RESOURCE_ROOT: ['outside', 'file.bin', 'DENIED'],
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
