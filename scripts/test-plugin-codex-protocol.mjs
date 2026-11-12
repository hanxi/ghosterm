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
  const stderr = [];
  child.stderr.on("data", chunk => { if (stderr.join("").length < 16_384) stderr.push(String(chunk)); });
  let protocolFailure;
  let exited = false;
  child.on("exit", () => { exited = true; });
  const client = new CodexAppServerClient(line => child.stdin.write(`${line}\n`));
  client.onProtocolError(error => { protocolFailure = error; });
  const decoder = new CodexLineDecoder(line => client.receiveLine(line));
  child.stdout.on("data", chunk => {
    try { decoder.push(chunk); }
    catch (error) { protocolFailure = error; client.close(); }
  });

  const withTimeout = (promise, ms = 20_000) => {
    let timer;
    return Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error("live app-server timed out")), ms);
    })]).finally(() => clearTimeout(timer));
  };
  try {
    const initialization = await withTimeout(client.initialize(clientInfo));
    assert.equal(typeof initialization.userAgent, "string");
    const started = await withTimeout(client.threadStart({ cwd, approvalPolicy: "on-request", sandbox: "read-only" }));
    assert.equal(typeof started.thread?.id, "string");
    assert.equal(protocolFailure, undefined, protocolFailure?.message);
    decoder.finish();
    assert.equal(exited, false);
    console.log(`CODEX APP-SERVER ADAPTER PASS: ${version}; initialize, initialized, thread/start; no turn/model request`);
  } finally {
    client.close();
    child.kill("SIGTERM");
    await withTimeout(new Promise(resolveExit => child.once("exit", resolveExit)), 3_000).catch(() => {});
    if (!exited) child.kill("SIGKILL");
    rmSync(temp, { recursive: true, force: true });
  }
});
