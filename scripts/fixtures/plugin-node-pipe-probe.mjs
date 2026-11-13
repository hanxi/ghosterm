// Only transport fault tests use a synthetic parent. No native grant proof.
import assert from 'node:assert/strict';
import path from 'node:path';
import { pathToFileURL } from 'node:url';
const { NodeStdioPipe } = await import(pathToFileURL(path.join(process.argv[2], 'node/runtime/pipe.mjs')));
const mode = process.argv[3];
const pipe = new NodeStdioPipe({ timeoutMs: mode === 'timeout' ? 200 : 5000 });
try {
  try {
    await pipe.ready;
    const request = '{"v":1,"id":"1","op":"access","capability":"ui.form"}';
    if (mode === 'fragmented') assert.equal(JSON.parse(pipe.exchange(request)).access, 'allowed');
    else {
      const expected = mode === 'timeout' ? /CHILD_PIPE_TIMEOUT/ : mode.startsWith('eof') ? /CHILD_PIPE_LOST/ : undefined;
      if (expected) assert.throws(() => pipe.exchange(request), expected);
      else assert.throws(() => pipe.exchange(request));
      assert.throws(() => pipe.exchange(request), /CHILD_PIPE_CLOSED/);
    }
  } finally { await pipe.close(); }
} catch (error) {
  console.error((error?.name === 'AssertionError' ? 'ASSERT ' : 'ERROR ') + (error?.stack ?? error));
  process.exitCode = error?.name === 'AssertionError' ? 1 : 2;
}
