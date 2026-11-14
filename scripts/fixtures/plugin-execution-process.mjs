// Real native ProcessService effects, invoked only by the owned SSH fixture.
import { createHash } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import { constants } from 'node:os';
const pause = () => new Promise(resolve => setTimeout(resolve, 1));
const check = (value, label) => { if (!value) throw Error(label); };
export async function appProcess(ctx, kind, observation) {
  check(ctx.process && ctx.invocation.workspaceHandle?.id === 'workspace', 'business service');
  let cancelled = false;
  const cancellation = { get cancelled() { return cancelled; } };
  const marker = 'ghosterm-business-' + process.pid;
  const spawn = (program = 'cat', root = 'workspace', argv = [marker]) => ctx.process.spawn({
    program, argv, cwd: { id: root }, envPolicy: 'selected-user-environment', cancellation,
  });
  if (kind === 'PROCESS_ROOT' || kind === 'PROCESS_PROGRAM') {
    let error = '';
    try { await spawn(kind === 'PROCESS_PROGRAM' ? 'missing' : 'cat', kind === 'PROCESS_ROOT' ? 'outside' : 'workspace'); }
    catch (failure) { error = failure.message; }
    check(error === 'PROCESS_denied', 'business rejection');
    writeFileSync(observation + '/process-proof', JSON.stringify({ rejected: true }), { flag: 'wx' });
    return { text: 'DENIED', count: 0 };
  }
  const child = await spawn('cat', 'workspace', kind === 'PROCESS_STDERR' ? [marker, '-!'] : [marker]);
  check(child.verifiedVersion === 'fixture', 'business version');
  writeFileSync(observation + '/business-started', 'started', { flag: 'wx' });
  if (kind === 'PROCESS_STOP' || kind === 'PROCESS_LOSS') {
    writeFileSync(observation + '/handler', 'started', { flag: 'wx' });
    return await new Promise(() => {});
  }
  const untilExit = async () => {
    const end = Date.now() + 10000;
    for (;;) { const status = child.poll(); if (status.state === 'exited') return status;
      check(Date.now() < end, 'business exit'); await pause(); }
  };
  if (kind === 'PROCESS_CANCEL') {
    cancelled = true;
    check(child.writeStdin(new Uint8Array([1])).state === 'closed', 'cancelled business write');
    const exit = await untilExit(); check(exit.signal === constants.signals.SIGTERM && exit.code === null, 'business cancellation');
    await child.release();
    writeFileSync(observation + '/process-proof', JSON.stringify({ signal: exit.signal, reaped: true }), { flag: 'wx' });
    return { text: 'cancelled', count: exit.signal };
  }
  const hash = createHash('sha256');
  let received = 0, sent = 0;
  if (kind === 'PROCESS') {
    let closed = false;
    while (received < 262144) {
      if (sent < 262144) {
        const bytes = Uint8Array.from({ length: Math.min(16384, 262144 - sent) }, (_, i) => (sent + i) % 251);
        const result = child.writeStdin(bytes);
        if (result.state === 'data') sent += result.count;
        else check(result.state === 'wait', 'business write');
      }
      if (sent === 262144 && !closed) { child.closeStdin(); closed = true; }
      const result = child.readStdout(16384);
      if (result.state === 'data') {
        for (const byte of result.bytes) { check(byte === received % 251, 'business byte'); received++; }
        hash.update(result.bytes);
      } else check(result.state === 'wait', 'business premature EOF');
      await pause();
    }
    check(sent === 262144, 'business input size');
  } else child.closeStdin();
  const collect = async stderr => {
    const bytes = [];
    for (;;) {
      const result = stderr ? child.readStderr(16384) : child.readStdout(16384);
      if (result.state === 'eof') return Buffer.from(bytes);
      if (result.state === 'data') { bytes.push(...result.bytes); check(bytes.length <= 8192, 'business capture limit'); }
      else check(result.state === 'wait', 'business pipe');
      await pause();
    }
  };
  const stdout = await collect(false), stderr = await collect(true), exit = await untilExit();
  check(exit.signal === null && exit.terminationReason === undefined, 'business exit cause');
  if (kind === 'PROCESS_STDERR') {
    check(stdout.length === 0 && stderr.length > 0 && exit.code !== 0, 'stderr separation');
    received = stderr.length; hash.update(stderr);
  } else {
    check(exit.code === 0 && stderr.length === 0, 'business success');
    if (kind === 'PROCESS') check(stdout.length === 0, 'business extra stdout');
    else {
      received = stdout.length;
      const normalized = kind === 'PROCESS_ENVIRONMENT'
        ? Buffer.from(stdout.toString('utf8').trimEnd().split('\n').sort().join('\n') + '\n') : stdout;
      hash.update(normalized);
    }
  }
  await child.release();
  let retired = false; try { child.poll(); } catch { retired = true; } check(retired, 'retired business reference');
  const digest = hash.digest('hex');
  writeFileSync(observation + '/process-proof', JSON.stringify({ bytes: received, sha256: digest,
    code: exit.code, signal: 0, reaped: true }), { flag: 'wx' });
  return { text: digest, count: received };
}
