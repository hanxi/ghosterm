import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import { CodexAppServerClient, CodexOutcomeUnknownError, CodexProtocolError } from
  "../plugins/official/ai-codex/protocol/client.ts";
import { CodexLineDecoder, CodexLineDecoderError, MAX_APP_SERVER_LINE_BYTES } from
  "../plugins/official/ai-codex/protocol/line-decoder.ts";
import { CodexAppServerStdio, CODEX_PENDING_WRITE_MAX, CODEX_PIPE_CHUNK_MAX } from
  "../plugins/official/ai-codex/protocol/stdio.ts";
import { spawnCodexAppServer } from "../plugins/official/ai-codex/protocol/process.ts";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const lock = JSON.parse(readFileSync(join(root, "plugins/official/ai-codex/protocol/app-server.lock.json"), "utf8"));
const clientInfo = { name: "ghosterm-protocol-test", title: "Ghosterm protocol test", version: "0.0.0" };

function fakeActiveClient() {
  const writes = [];
  const client = new CodexAppServerClient(line => writes.push(JSON.parse(line)));
  const starting = client.initialize(clientInfo);
  assert.equal(writes[0].method, "initialize");
  client.receiveLine(JSON.stringify({ id: writes[0].id, result: { userAgent: "fixture" } }));
  return starting.then(() => ({ client, writes }));
}

test("preserves approval ids, exact decision classes, and single response", async () => {
  const { client, writes } = await fakeActiveClient();
  let approval;
  client.onServerRequest(value => { approval = value; });
  client.receiveLine(JSON.stringify({ id: "approval-9", method: "item/fileChange/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-b", itemId: "item-c" } }));
  assert.equal(approval.id, "approval-9");
  assert.throws(() => client.respond("approval-9", { applyNetworkPolicyAmendment: {} }), CodexProtocolError);
  client.respond("approval-9", "decline");
  assert.deepEqual(writes.at(-1), { jsonrpc: "2.0", id: "approval-9", result: { decision: "decline" } });
  assert.throws(() => client.respond("approval-9", "accept"), CodexProtocolError);
});

test("server resolution expires an unanswered approval", async () => {
  const { client } = await fakeActiveClient();
  client.receiveLine(JSON.stringify({ id: 17, method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-b", itemId: "item-c" } }));
  client.receiveLine(JSON.stringify({ method: "serverRequest/resolved", params: { threadId: "thread-a", requestId: 17 } }));
  assert.throws(() => client.respond(17, "accept"), CodexProtocolError);
});

test("validates structured command policy amendments against the pinned schema", async () => {
  const { client, writes } = await fakeActiveClient();
  client.receiveLine(JSON.stringify({ id: "command-1", method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-b", itemId: "item-c" } }));
  assert.throws(() => client.respond("command-1", { acceptWithExecpolicyAmendment: ["ok", 1] }), CodexProtocolError);
  assert.throws(() => client.respond("command-1", { applyNetworkPolicyAmendment: { host: "example.test", action: "ask" } }), CodexProtocolError);
  client.respond("command-1", { applyNetworkPolicyAmendment: { host: "example.test", action: "deny" } });
  assert.deepEqual(writes.at(-1).result, { decision: { applyNetworkPolicyAmendment: { host: "example.test", action: "deny" } } });
});

test("closing a pipe reports outstanding operation as unknown without retry", async () => {
  const { client, writes } = await fakeActiveClient();
  const pending = client.threadStart({ cwd: "/isolated", approvalPolicy: "on-request", sandbox: "read-only" });
  assert.equal(writes.at(-1).method, "thread/start");
  client.close();
  await assert.rejects(pending, error => error instanceof CodexOutcomeUnknownError && error.requestID === writes.at(-1).id);
  assert.equal(writes.filter(message => message.method === "thread/start").length, 1);
});

test("decodes split UTF-8 and multiple JSON lines with CRLF support", () => {
  const lines = [];
  const decoder = new CodexLineDecoder(line => lines.push(line));
  const bytes = new TextEncoder().encode('{"text":"雪😀"}\r\n{"next":true}\n');
  decoder.push(bytes.subarray(0, 13));
  decoder.push(bytes.subarray(13, 17));
  decoder.push(bytes.subarray(17));
  decoder.finish();
  assert.deepEqual(lines, ['{"text":"雪😀"}', '{"next":true}']);
});

test("rejects oversized and truncated stdout records", () => {
  const oversized = new CodexLineDecoder(() => {});
  assert.throws(() => oversized.push(new Uint8Array(MAX_APP_SERVER_LINE_BYTES + 1)), CodexLineDecoderError);
  const truncated = new CodexLineDecoder(() => {});
  truncated.push(new TextEncoder().encode('{"partial":'));
  assert.throws(() => truncated.finish(), CodexLineDecoderError);
  const invalidUTF8 = new CodexLineDecoder(() => {});
  assert.throws(() => invalidUTF8.push(new Uint8Array([0xff, 0x0a])), CodexLineDecoderError);
});

test("stdio owner bounds writes, handles partial IO, and drains stderr without retaining it", async () => {
  const output = [], errors = [];
  let writesBlocked = true;
  let stdout = { state: "wait" }, stderr = { state: "wait" };
  const pipes = {
    writeStdin(bytes) {
      if (writesBlocked) return { state: "wait" };
      const count = Math.min(3, bytes.length);
      output.push(...bytes.subarray(0, count));
      return { state: "data", count };
    },
    readStdout(maxBytes) {
      if (stdout.state === "data" && stdout.bytes.length > maxBytes) {
        const bytes = stdout.bytes.subarray(0, maxBytes);
        stdout = { state: "data", bytes: stdout.bytes.subarray(maxBytes) };
        return { state: "data", bytes };
      }
      const result = stdout;
      stdout = { state: "wait" };
      return result;
    },
    readStderr(maxBytes) {
      if (stderr.state === "data" && stderr.bytes.length > maxBytes) {
        const bytes = stderr.bytes.subarray(0, maxBytes);
        stderr = { state: "data", bytes: stderr.bytes.subarray(maxBytes) };
        errors.push(bytes.length);
        return { state: "data", bytes };
      }
      const result = stderr;
      if (result.state === "data") errors.push(result.bytes.length);
      stderr = { state: "wait" };
      return result;
    },
    closeStdin() {},
  };
  const connection = new CodexAppServerStdio(pipes);
  const initializing = connection.client.initialize(clientInfo);
  assert.equal(connection.client.active, false);
  connection.step();
  writesBlocked = false;
  for (let i = 0; i < 100; i++) connection.step();
  assert.ok(output.length > 0);
  assert.match(new TextDecoder().decode(Uint8Array.from(output)), /"method":"initialize"/);
  stdout = { state: "data", bytes: new TextEncoder().encode('{"id":1,"result":{"userAgent":"fixture"}}\n') };
  stderr = { state: "data", bytes: new Uint8Array(CODEX_PENDING_WRITE_MAX) };
  for (let i = 0; i < 64; i++) connection.step();
  assert.equal((await initializing).userAgent, "fixture");
  assert.equal(errors.reduce((sum, size) => sum + size, 0), CODEX_PENDING_WRITE_MAX);
  connection.close();
});

test("stdio EOF inside a JSON record fails closed and leaves the request outcome unknown", async () => {
  let stdout = { state: "data", bytes: new TextEncoder().encode('{"id":1,"result":') };
  const connection = new CodexAppServerStdio({
    writeStdin: bytes => ({ state: "data", count: bytes.length }),
    readStdout: () => { const result = stdout; stdout = { state: "wait" }; return result; },
    readStderr: () => ({ state: "wait" }),
    closeStdin() {},
  });
  const pending = connection.client.initialize(clientInfo);
  connection.step();
  stdout = { state: "eof" };
  connection.step();
  await assert.rejects(pending, error => error instanceof CodexOutcomeUnknownError);
  assert.equal(connection.failure?.name, "CodexLineDecoderError");
  connection.close();
});

test("stdout EOF does not stop later stderr drainage", () => {
  const stderr = [{ state: "wait" }];
  let retained = 0;
  const connection = new CodexAppServerStdio({
    writeStdin: bytes => ({ state: "data", count: bytes.length }),
    readStdout: () => ({ state: "eof" }),
    readStderr: () => {
      const result = stderr.shift() ?? { state: "eof" };
      if (result.state === "data") retained += result.bytes.length;
      return result;
    },
    closeStdin() {},
  });
  connection.step();
  assert.equal(connection.drained, false);
  stderr.push({ state: "data", bytes: new Uint8Array(CODEX_PIPE_CHUNK_MAX) });
  connection.step();
  assert.equal(retained, CODEX_PIPE_CHUNK_MAX);
  assert.equal(connection.drained, false);
  connection.step();
  assert.equal(connection.drained, true);
  connection.close();
});

test("PluginContext process grant launches Codex through the bounded stdio adapter", async () => {
  const writes = [];
  const reads = [];
  let signalled;
  let released = false;
  const child = {
    verifiedVersion: "0.166.0",
    writeStdin(bytes) {
      const text = new TextDecoder().decode(bytes);
      writes.push(text);
      const request = JSON.parse(text.trim());
      reads.push(new TextEncoder().encode(JSON.stringify({
        id: request.id, result: { userAgent: "fixture" },
      }) + "\n"));
      return { state: "data", count: bytes.length };
    },
    readStdout(maxBytes) {
      const bytes = reads.shift();
      return bytes ? { state: "data", bytes: bytes.subarray(0, maxBytes) } : { state: "wait" };
    },
    readStderr() { return { state: "wait" }; },
    closeStdin() {},
    signal(signal) { signalled = signal; },
    poll() { return { state: "exited", code: 0, signal: null }; },
    async release() { released = true; },
  };
  let spawnOptions;
  const context = {
    side: "executor",
    capabilities: { query: () => "allowed" },
    cancellation: { cancelled: false },
    invocation: { workspaceHandle: { id: "workspace-root" } },
    process: { async spawn(options) { spawnOptions = options; return child; } },
  };
  const server = await spawnCodexAppServer(context, clientInfo);
  assert.deepEqual({
    program: spawnOptions.program,
    argv: spawnOptions.argv,
    cwd: spawnOptions.cwd,
    envPolicy: spawnOptions.envPolicy,
    cancellation: spawnOptions.cancellation,
  }, {
    program: "codex", argv: ["app-server"], cwd: { id: "workspace-root" },
    envPolicy: "selected-user-environment", cancellation: context.cancellation,
  });
  server.step();
  assert.equal((await server.ready).userAgent, "fixture");
  assert.match(writes[0], /"method":"initialize"/);
  await server.stop();
  assert.equal(signalled, "terminate");
  assert.equal(released, true);
});

test("PluginContext process integration rejects non-Executor or missing live grants", async () => {
  let spawned = false;
  const context = {
    side: "client",
    capabilities: { query: () => "allowed" },
    cancellation: { cancelled: false },
    invocation: { workspaceHandle: { id: "workspace-root" } },
    process: { async spawn() { spawned = true; throw new Error("must not spawn"); } },
  };
  await assert.rejects(spawnCodexAppServer(context, clientInfo), /authorized Executor process grant/);
  context.side = "executor";
  context.capabilities.query = name => name === "process.spawn" ? "allowed" : "revoked";
  await assert.rejects(spawnCodexAppServer(context, clientInfo), /authorized Executor process grant/);
  assert.equal(spawned, false);
});

test("naturally exited Codex child drains both pipes and is reaped without an explicit stop", async () => {
  let released = false;
  let signalled = false;
  const child = {
    verifiedVersion: "0.166.0",
    writeStdin(bytes) { return { state: "data", count: bytes.length }; },
    readStdout() { return { state: "eof" }; },
    readStderr() { return { state: "eof" }; },
    closeStdin() {},
    signal() { signalled = true; },
    poll() { return { state: "exited", code: 0, signal: null }; },
    async release() { released = true; },
  };
  const context = {
    side: "executor",
    capabilities: { query: () => "allowed" },
    cancellation: { cancelled: false },
    invocation: { workspaceHandle: { id: "workspace-root" } },
    process: { async spawn() { return child; } },
  };
  const server = await spawnCodexAppServer(context, clientInfo);
  const ready = assert.rejects(server.ready, error => error instanceof CodexOutcomeUnknownError);
  server.step();
  await ready;
  await server.stop();
  assert.deepEqual(server.exit, { state: "exited", code: 0, signal: null });
  assert.equal(server.reaped, true);
  assert.equal(released, true);
  assert.equal(signalled, false);
});

test("runs initialize and thread/start against the pinned real server, without a turn", async () => {
  const executable = process.env.GHOSTERM_CODEX ?? "codex";
  const version = execFileSync(executable, ["--version"], { cwd: root, encoding: "utf8" }).trim();
  assert.equal(version, lock.codexCliVersion);

  const temp = mkdtempSync(join(root, "tmp", "codex-protocol-live-"));
  const home = join(temp, "home"), config = join(temp, "config"), cwd = join(temp, "project");
  for (const path of [home, config, cwd]) mkdirSync(path);
  const env = { ...process.env, HOME: home, XDG_CONFIG_HOME: config, TMPDIR: join(root, "tmp") };
  for (const key of ["CODEX_HOME", "OPENAI_API_KEY", "CODEX_API_KEY"]) delete env[key];
  const child = spawn(executable, ["app-server"], { cwd, env, stdio: ["pipe", "pipe", "pipe"] });
  const stdoutChunks = [], stderrChunks = [];
  let exited = false;
  child.on("exit", () => { exited = true; });
  child.stdout.on("data", chunk => stdoutChunks.push(Uint8Array.from(chunk)));
  child.stderr.on("data", chunk => stderrChunks.push(Uint8Array.from(chunk)));
  const readQueue = (queue, maxBytes) => {
    if (!queue.length) return { state: exited ? "eof" : "wait" };
    const head = queue[0];
    const bytes = head.subarray(0, maxBytes);
    if (bytes.length === head.length) queue.shift();
    else queue[0] = head.subarray(maxBytes);
    return { state: "data", bytes };
  };
  const connection = new CodexAppServerStdio({
    readStdout: maxBytes => readQueue(stdoutChunks, maxBytes),
    readStderr: maxBytes => readQueue(stderrChunks, maxBytes),
    writeStdin: bytes => {
      if (child.stdin.destroyed || !child.stdin.writable) return { state: "closed" };
      child.stdin.write(Buffer.from(bytes));
      return { state: "data", count: bytes.length };
    },
    closeStdin: () => child.stdin.end(),
  });
  const pump = setInterval(() => connection.step(), 2);

  const withTimeout = (promise, ms = 20_000) => {
    let timer;
    return Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error("live app-server timed out")), ms);
    })]).finally(() => clearTimeout(timer));
  };
  try {
    const initialization = await withTimeout(connection.client.initialize(clientInfo));
    assert.equal(typeof initialization.userAgent, "string");
    const started = await withTimeout(connection.client.threadStart({ cwd, approvalPolicy: "on-request", sandbox: "read-only" }));
    assert.equal(typeof started.thread?.id, "string");
    assert.equal(connection.failure, undefined, connection.failure?.message);
    assert.equal(exited, false);
    console.log(`CODEX APP-SERVER STDIO PASS: ${version}; initialize, initialized, thread/start through bounded pipe owner; no turn/model request`);
  } finally {
    clearInterval(pump);
    connection.close();
    child.kill("SIGTERM");
    await withTimeout(new Promise(resolveExit => child.once("exit", resolveExit)), 3_000).catch(() => {});
    if (!exited) child.kill("SIGKILL");
    rmSync(temp, { recursive: true, force: true });
  }
});
