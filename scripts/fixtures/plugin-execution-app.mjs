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
    return value;
  });
  return { dispose() {} };
}
