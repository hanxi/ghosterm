import assert from "node:assert/strict";
import { spawn, execFileSync } from "node:child_process";
import { mkdtempSync, mkdirSync, readFileSync, rmSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import { CodexAppServerClient, CodexOutcomeUnknownError, CodexProtocolError } from
  "../plugins/official/ai-codex/protocol/client.ts";
import { CodexLineDecoder, CodexLineDecoderError, MAX_APP_SERVER_LINE_BYTES, APP_SERVER_DECODE_CHUNK_BYTES } from
  "../plugins/official/ai-codex/protocol/line-decoder.ts";
import { CodexAppServerStdio, CODEX_PENDING_WRITE_MAX, CODEX_PIPE_CHUNK_MAX, CODEX_PENDING_LINES_MAX } from
  "../plugins/official/ai-codex/protocol/stdio.ts";
import { CodexConversation } from "../plugins/official/ai-codex/protocol/conversation.ts";
import { spawnCodexAppServer } from "../plugins/official/ai-codex/protocol/process.ts";
import { CODEX_CLI_VERSION } from "../plugins/official/ai-codex/protocol/version.ts";
import { utf8ByteLength } from "../plugins/official/ai-codex/protocol/utf8.ts";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const lock = JSON.parse(readFileSync(join(root, "plugins/official/ai-codex/protocol/app-server.lock.json"), "utf8"));
assert.equal(CODEX_CLI_VERSION, lock.codexCliVersion);
const clientInfo = { name: "ghosterm-protocol-test", title: "Ghosterm protocol test", version: "0.0.0" };

test("UTF-8 length preserves every scalar width and rejects unpaired surrogates", () => {
  const encoder = new TextEncoder();
  for (let point = 0; point <= 0x10ffff; point++) {
    if (point >= 0xd800 && point <= 0xdfff) continue;
    const value = String.fromCodePoint(point);
    assert.equal(utf8ByteLength(value), encoder.encode(value).length, `UTF-8 scalar byte width must be exact: ${point}`);
  }
  for (const invalid of ["\ud800", "\udfff", "a\ud800b", "\ud800\ud800", "😀\udfff", "\ud800😀"]) {
    assert.throws(() => utf8ByteLength(invalid), /invalid UTF-16/, "unpaired surrogates must never receive a byte count");
  }
  const mixed = "a\0\r\n%é中😀𐀀\uffff".repeat(12000);
  assert.equal(utf8ByteLength(mixed), encoder.encode(mixed).length, "mixed full-document UTF-8 bytes must remain exact");
});

test("prepared protocol messages dispatch once and cannot deliver after close", () => {
  const client = new CodexAppServerClient(() => {});
  const events = [];
  client.onNotification(event => events.push(event.method));
  const dispatch = client.prepareLine('{"method":"fixture.note","params":{}}');
  assert.equal(typeof dispatch, "function");
  assert.deepEqual(events, []);
  dispatch(); dispatch();
  assert.deepEqual(events, ["fixture.note"], "prepared protocol frames must dispatch once");
  const late = client.prepareLine('{"method":"fixture.late","params":{}}');
  client.close(); late();
  assert.deepEqual(events, ["fixture.note"], "prepared frames must not deliver into a closed client");
});

test("prepared protocol messages still reject invalid UTF-16 JSON and envelopes", () => {
  for (const line of ["\ud800", "{", '[]', '{"jsonrpc":"1.0","method":"fixture.note"}']) {
    const client = new CodexAppServerClient(() => {});
    let errors = 0, events = 0;
    client.onProtocolError(() => errors++); client.onNotification(() => events++);
    assert.equal(client.prepareLine(line), undefined, "preparation must validate the complete original frame");
    assert.equal(errors, 1); assert.equal(events, 0);
  }
});

async function pumpInitialization(server) {
  let finished = false;
  void server.ready.then(() => { finished = true; }, () => { finished = true; });
  for (let step = 0; step < 20 && !finished; step++) {
    server.step();
    await Promise.resolve();
    await Promise.resolve();
  }
  assert(finished, "initialization must settle under bounded owner-driven steps");
}

function fakeActiveClient() {
  const writes = [];
  const client = new CodexAppServerClient(line => writes.push(JSON.parse(line)));
  const starting = client.initialize(clientInfo);
  assert.equal(writes[0].method, "initialize");
  client.receiveLine(JSON.stringify({ id: writes[0].id, result: { userAgent: "ghosterm-protocol-test/0.166.0 (fixture)" } }));
  return starting.then(() => ({ client, writes }));
}

function turnFixture(id, status) {
  return { id, rootTurnId: null, items: [], itemsView: "full", status,
    error: null, startedAt: 1, completedAt: status === "inProgress" ? null : 2, durationMs: 1 };
}

function commandItemFixture(id) {
  return { type: "commandExecution", id, pluginId: null, scriptPath: null, command: "echo ok",
    cwd: "/isolated", processId: null, source: "agent", status: "inProgress", commandActions: [],
    aggregatedOutput: null, exitCode: null, durationMs: null };
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

test("turn completion expires only matching unanswered approvals without writing a decision", async () => {
  const { client, writes } = await fakeActiveClient();
  for (const [id, threadId, turnId] of [[17, "thread-a", "turn-a"],
    ["17", "thread-a", "turn-b"], [18, "thread-b", "turn-a"]]) {
    client.receiveLine(JSON.stringify({ id, method: "item/fileChange/requestApproval",
      params: { threadId, turnId, itemId: "change" } }));
  }
  client.receiveLine(JSON.stringify({ method: "turn/completed", params: {
    threadId: "thread-a", turn: turnFixture("turn-a", "interrupted"),
  } }));
  const count = writes.length;
  assert.throws(() => client.respond(17, "accept"), CodexProtocolError,
    "completed turn approval must expire before any decision is written");
  assert.equal(writes.length, count);
  client.respond("17", "decline");
  client.respond(18, "decline");
  assert.deepEqual(writes.slice(-2).map(value => value.id), ["17", 18]);
});

test("conversation refuses an old approval after completion and during a new turn", async () => {
  const { client, writes } = await fakeActiveClient();
  const conversation = new CodexConversation(client);
  const opening = conversation.startThread({});
  client.receiveLine(JSON.stringify({ id: writes.at(-1).id, result: { thread: { id: "thread-a" } } }));
  await opening;
  const starting = conversation.sendMessage("first");
  client.receiveLine(JSON.stringify({ id: writes.at(-1).id, result: { turn: { id: "turn-a" } } }));
  await starting;
  client.receiveLine(JSON.stringify({ id: "old", method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-a", itemId: "command", startedAtMs: 1 } }));
  client.receiveLine(JSON.stringify({ method: "turn/completed", params: {
    threadId: "thread-a", turn: turnFixture("turn-a", "completed"),
  } }));
  const count = writes.length;
  assert.throws(() => conversation.respondApproval("old", "accept"), CodexProtocolError,
    "conversation must refuse a completed turn approval");
  assert.equal(writes.length, count);
  const next = conversation.sendMessage("second");
  client.receiveLine(JSON.stringify({ id: writes.at(-1).id, result: { turn: { id: "turn-b" } } }));
  await next;
  assert.throws(() => conversation.respondApproval("old", "accept"), CodexProtocolError);
  client.receiveLine(JSON.stringify({ id: "new", method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-b", itemId: "command", startedAtMs: 2 } }));
  conversation.respondApproval("new", "decline");
  assert.equal(writes.at(-1).id, "new");
  await conversation.close();
});

test("validates structured command policy amendments against the pinned schema", async () => {
  const { client, writes } = await fakeActiveClient();
  client.receiveLine(JSON.stringify({ id: "command-1", method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-b", itemId: "item-c" } }));
  assert.throws(() => client.respond("command-1", { acceptWithExecpolicyAmendment: ["echo ok"] }), CodexProtocolError);
  assert.throws(() => client.respond("command-1", { acceptWithExecpolicyAmendment: { execpolicy_amendment: ["ok", 1] } }), CodexProtocolError);
  assert.throws(() => client.respond("command-1", { applyNetworkPolicyAmendment: { host: "example.test", action: "deny" } }), CodexProtocolError);
  assert.throws(() => client.respond("command-1", { applyNetworkPolicyAmendment: { network_policy_amendment: { host: "example.test", action: "ask" } } }), CodexProtocolError);
  client.respond("command-1", { applyNetworkPolicyAmendment: { network_policy_amendment: { host: "example.test", action: "deny" } } });
  assert.deepEqual(writes.at(-1).result, { decision: { applyNetworkPolicyAmendment: {
    network_policy_amendment: { host: "example.test", action: "deny" },
  } } });
});

test("turn/start emits the stable thread input shape", async () => {
  const { client, writes } = await fakeActiveClient();
  const pending = client.turnStart({
    threadId: "thread-a",
    input: [{ type: "text", text: "hello", text_elements: [] }],
  });
  assert.deepEqual(writes.at(-1), { jsonrpc: "2.0", id: writes.at(-1).id,
    method: "turn/start", params: { threadId: "thread-a",
      input: [{ type: "text", text: "hello", text_elements: [] }] } });
  client.receiveLine(JSON.stringify({ id: writes.at(-1).id, result: { turn: { id: "turn-a", status: "inProgress" } } }));
  await pending;
});

test("conversation preserves turn, item, approval, resolution, and completion order", async () => {
  const { client, writes } = await fakeActiveClient();
  const conversation = new CodexConversation(client);
  const events = [];
  conversation.onEvent(event => events.push(event));

  const opening = conversation.startThread({ cwd: "/isolated" });
  const threadRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ id: threadRequest.id, result: { thread: { id: "thread-a" } } }));
  assert.equal(await opening, "thread-a");

  const starting = conversation.sendMessage("hello", "user-message-1");
  const turnRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ method: "turn/started", params: {
    threadId: "thread-a", turn: turnFixture("turn-a", "inProgress"),
  } }));
  client.receiveLine(JSON.stringify({ id: "approval-a", method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-a", itemId: "item-command", startedAtMs: 1 } }));
  client.receiveLine(JSON.stringify({ method: "serverRequest/resolved", params: {
    threadId: "thread-a", requestId: "approval-a",
  } }));
  client.receiveLine(JSON.stringify({ method: "item/started", params: {
    threadId: "thread-a", turnId: "turn-a", item: commandItemFixture("item-command"), startedAtMs: 1,
  } }));
  client.receiveLine(JSON.stringify({ id: "approval-b", method: "item/commandExecution/requestApproval",
    params: { threadId: "thread-a", turnId: "turn-a", itemId: "item-command", startedAtMs: 2 } }));
  conversation.respondApproval("approval-b", "accept");
  assert.throws(() => conversation.respondApproval("approval-b", "accept"), CodexProtocolError);
  client.receiveLine(JSON.stringify({ id: turnRequest.id, result: { turn: { id: "turn-a" } } }));
  assert.equal(await starting, "turn-a");
  client.receiveLine(JSON.stringify({ method: "item/agentMessage/delta", params: {
    threadId: "thread-a", turnId: "turn-a", itemId: "item-message", delta: "done",
  } }));
  client.receiveLine(JSON.stringify({ method: "turn/completed", params: {
    threadId: "thread-a", turn: turnFixture("turn-a", "completed"),
  } }));

  assert.deepEqual(events.map(event => event.kind), [
    "notification", "approval-request", "approval-resolved", "notification", "approval-request",
    "notification", "notification",
  ]);
  assert.deepEqual(events.map(event => event.sequence), [1, 2, 3, 4, 5, 6, 7]);
  assert.equal(conversation.state, "ready");
  assert.equal(conversation.currentTurnID, undefined);
  await conversation.close();
});

test("conversation interrupts only its active turn and reports closed work as unknown", async () => {
  const { client, writes } = await fakeActiveClient();
  const conversation = new CodexConversation(client);
  const opening = conversation.startThread({});
  const threadRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ id: threadRequest.id, result: { thread: { id: "thread-b" } } }));
  await opening;
  await assert.rejects(conversation.interrupt(), CodexProtocolError);

  const starting = conversation.sendMessage("work");
  const turnRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ id: turnRequest.id, result: { turn: { id: "turn-b" } } }));
  assert.equal(await starting, "turn-b");
  const interrupting = conversation.interrupt();
  assert.deepEqual(writes.at(-1).params, { threadId: "thread-b", turnId: "turn-b" });
  const interruptRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ id: interruptRequest.id, result: {} }));
  await interrupting;
  client.receiveLine(JSON.stringify({ method: "turn/completed", params: {
    threadId: "thread-b", turn: turnFixture("turn-b", "interrupted"),
  } }));
  assert.equal(conversation.state, "ready");

  const events = [];
  conversation.onEvent(event => events.push(event));
  const uncertain = conversation.sendMessage("uncertain");
  const unknownRequest = writes.at(-1);
  const rejected = assert.rejects(uncertain, CodexOutcomeUnknownError);
  await conversation.close();
  await rejected;
  assert.equal(events.at(-1).kind, "closed");
  assert.equal(events.at(-1).outcome, "unknown");
  assert.equal(conversation.state, "closed");
  assert.equal(writes.filter(message => message.id === unknownRequest.id).length, 1);
});

test("conversation fails closed on turn events from a different thread", async () => {
  const { client, writes } = await fakeActiveClient();
  const conversation = new CodexConversation(client);
  const events = [];
  conversation.onEvent(event => events.push(event));
  const opening = conversation.startThread({});
  const threadRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ id: threadRequest.id, result: { thread: { id: "thread-owned" } } }));
  await opening;
  const pending = conversation.sendMessage("hello");
  const turnRequest = writes.at(-1);
  client.receiveLine(JSON.stringify({ method: "turn/started", params: {
    threadId: "thread-other", turn: turnFixture("turn-other", "inProgress"),
  } }));
  assert.equal(conversation.state, "failed");
  assert.equal(events.at(-1).kind, "protocol-error");
  const rejected = assert.rejects(pending, CodexOutcomeUnknownError);
  await conversation.close();
  await rejected;
  assert.equal(writes.filter(message => message.id === turnRequest.id).length, 1);
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

test("incremental line decoding preserves split code points at block and pipe boundaries", () => {
  for (const prefix of [8188, 8189, 8190, 8191, 8192, 8193]) {
    for (const chunk of [1, 2, 3, 7, 2048, 8191, 8192, 16384]) {
      const line = "x".repeat(prefix) + "é中😀last";
      const bytes = new TextEncoder().encode(line + "\r\nsecond😀\n");
      const lines = [];
      const decoder = new CodexLineDecoder(value => lines.push(value));
      assert.doesNotThrow(() => {
        for (let at = 0; at < bytes.length; at += chunk) decoder.push(bytes.subarray(at, at + chunk));
        decoder.finish();
      }, "incremental UTF-8 must retain an incomplete code point across decode blocks");
      assert.deepEqual(lines, [line, "second😀"], "incremental UTF-8 must preserve every split code point");
    }
  }
});

test("incremental line decoding rejects malformed completed blocks before newline", () => {
  for (const invalid of [[0xe0, 0x80, 0x80], [0xed, 0xa0, 0x80], [0xf4, 0x90, 0x80, 0x80], [0xc0, 0x80], [0x80]]) {
    const bytes = new Uint8Array(APP_SERVER_DECODE_CHUNK_BYTES * 2 + invalid.length);
    bytes.fill(0x78); bytes.set(invalid, APP_SERVER_DECODE_CHUNK_BYTES - 1);
    const decoder = new CodexLineDecoder(() => assert.fail("invalid partial line must never publish"));
    assert.throws(() => decoder.push(bytes), /invalid UTF-8/,
      "invalid UTF-8 must fail in a completed decode block before newline");
    assert.throws(() => decoder.push(new Uint8Array([0x0a])), /decoder is failed/);
  }
});

test("incremental line decoding retains aggregate byte and partial EOF limits", () => {
  const block = new Uint8Array(APP_SERVER_DECODE_CHUNK_BYTES).fill(0x78);
  const lines = [];
  const decoder = new CodexLineDecoder(value => lines.push(value));
  for (let at = 0; at < MAX_APP_SERVER_LINE_BYTES; at += block.length) decoder.push(block);
  assert.equal(lines.length, 0);
  decoder.push(new Uint8Array([0x0a]));
  assert.equal(lines[0], "x".repeat(MAX_APP_SERVER_LINE_BYTES));
  const overflow = new CodexLineDecoder(() => assert.fail("oversized line must never publish"));
  for (let at = 0; at < MAX_APP_SERVER_LINE_BYTES; at += block.length) overflow.push(block);
  assert.throws(() => overflow.push(new Uint8Array([0x78])), /exceeds 1 MiB/,
    "flushed fragments must retain the complete line byte budget");
  const partial = new CodexLineDecoder(() => assert.fail("partial line must never publish"));
  partial.push(block);
  assert.throws(() => partial.finish(), /inside a JSON line/,
    "fully decoded fragments must still reject EOF before newline");
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
  stdout = { state: "data", bytes: new TextEncoder().encode('{"id":1,"result":{"userAgent":"ghosterm-protocol-test/0.166.0 (fixture)"}}\n') };
  stderr = { state: "data", bytes: new Uint8Array(CODEX_PENDING_WRITE_MAX) };
  for (let i = 0; i < 64; i++) connection.step();
  assert.equal((await initializing).userAgent, "ghosterm-protocol-test/0.166.0 (fixture)");
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

test("stdio decodes and dispatches in separate slices before observing EOF", async () => {
  const response = new TextEncoder().encode('{"method":"fixture.note","params":{}}\n' +
    '{"id":1,"result":{"userAgent":"fixture/0.166.0"}}\n');
  const stdout = [{ state: "data", bytes: response }, { state: "eof" }];
  const connection = new CodexAppServerStdio({
    writeStdin: bytes => ({ state: "data", count: bytes.length }),
    readStdout: () => stdout.shift() ?? { state: "eof" },
    readStderr: () => ({ state: "eof" }), closeStdin() {},
  });
  let events = 0;
  connection.client.onNotification(() => events++);
  const ready = connection.client.initialize(clientInfo);
  void ready.catch(() => {});
  connection.step();
  await Promise.resolve();
  assert.equal(events, 0, "decoding must defer business dispatch to the next owner slice");
  assert.equal(connection.drained, false);
  connection.step();
  assert.equal(events, 0, "JSON validation must defer business dispatch to its own owner slice");
  connection.step();
  assert.equal(events, 1);
  connection.step();
  connection.step();
  assert.equal((await ready).userAgent, "fixture/0.166.0", "queued reply must survive until dispatch before EOF");
  connection.step();
  assert.equal(connection.drained, true);
  assert.equal(connection.failure, undefined);
  connection.close();
});

test("stdio bounds its decoded line queue before business dispatch", () => {
  const lines = new TextEncoder().encode('{"method":"fixture.note","params":{}}\n'.repeat(CODEX_PENDING_LINES_MAX + 1));
  let taken = false;
  const connection = new CodexAppServerStdio({
    writeStdin: bytes => ({ state: "data", count: bytes.length }),
    readStdout: () => taken ? { state: "wait" } : (taken = true, { state: "data", bytes: lines }),
    readStderr: () => ({ state: "wait" }), closeStdin() {},
  });
  let events = 0;
  connection.client.onNotification(() => events++);
  connection.step();
  assert.match(connection.failure?.message ?? "", /decoded line queue is full/,
    "decoded line queue must reject overflow");
  assert.equal(events, 0);
  connection.close();
});

test("stdio continues stderr drainage while dispatching queued lines", async () => {
  const output = new TextEncoder().encode('{"id":1,"result":{"userAgent":"fixture/0.166.0"}}\n' +
    '{"method":"fixture.note","params":{}}\n');
  let stdoutTaken = false, stderrRead = 0, drained = 0, events = 0;
  const connection = new CodexAppServerStdio({
    writeStdin: bytes => ({ state: "data", count: bytes.length }),
    readStdout: () => stdoutTaken ? { state: "eof" } : (stdoutTaken = true, { state: "data", bytes: output }),
    readStderr: () => {
      stderrRead++;
      if (stderrRead === 1) return { state: "wait" };
      if (stderrRead === 2) { drained += CODEX_PIPE_CHUNK_MAX; return { state: "data", bytes: new Uint8Array(CODEX_PIPE_CHUNK_MAX) }; }
      return { state: "eof" };
    }, closeStdin() {},
  });
  connection.client.onNotification(() => events++);
  const ready = connection.client.initialize(clientInfo);
  connection.step(); connection.step(); connection.step(); await ready;
  assert.equal(drained, CODEX_PIPE_CHUNK_MAX, "business dispatch must not block stderr drainage");
  connection.step(); connection.step(); connection.step();
  assert.equal(events, 1);
  assert.equal(connection.drained, true);
  assert.equal(connection.failure, undefined);
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
    verifiedVersion: lock.codexCliVersion,
    writeStdin(bytes) {
      const text = new TextDecoder().decode(bytes);
      writes.push(text);
      const request = JSON.parse(text.trim());
      reads.push(new TextEncoder().encode(JSON.stringify({
        id: request.id, result: { userAgent: "ghosterm-protocol-test/0.166.0 (fixture)" },
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
    program: "codex", argv: ["codex", "app-server"], cwd: { id: "workspace-root" },
    envPolicy: "selected-user-environment", cancellation: context.cancellation,
  });
  await pumpInitialization(server);
  assert.equal((await server.ready).userAgent, "ghosterm-protocol-test/0.166.0 (fixture)");
  assert.match(writes[0], /"method":"initialize"/);
  await server.stop();
  assert.equal(signalled, "terminate");
  assert.equal(released, true);
});

test("Codex initialize rejection terminates and reaps its owned child", async () => {
  let response;
  let signalled;
  let released = false;
  const child = {
    verifiedVersion: lock.codexCliVersion,
    writeStdin(bytes) {
      const request = JSON.parse(new TextDecoder().decode(bytes).trim());
      response = new TextEncoder().encode(JSON.stringify({
        id: request.id, error: { code: -32000, message: "fixture initialization rejected" },
      }) + "\n");
      return { state: "data", count: bytes.length };
    },
    readStdout(maxBytes) {
      if (!response) return { state: "wait" };
      const bytes = response.subarray(0, maxBytes);
      response = response.subarray(maxBytes);
      if (!response.length) response = undefined;
      return { state: "data", bytes };
    },
    readStderr() { return { state: "wait" }; },
    closeStdin() {},
    signal(value) { signalled = value; },
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
  await pumpInitialization(server);
  await assert.rejects(server.ready, /fixture initialization rejected/);
  assert.equal(signalled, "terminate");
  assert.equal(released, true);
  assert.equal(server.reaped, true);
  assert.deepEqual(server.exit, { state: "exited", code: 0, signal: null });
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

test("stops and reaps a Host child whose verified version differs from the protocol lock", async () => {
  let signalled;
  let released = false;
  let wrote = false;
  const context = {
    side: "executor",
    capabilities: { query: () => "allowed" },
    cancellation: { cancelled: false },
    invocation: { workspaceHandle: { id: "workspace-root" } },
    process: { async spawn() {
      return {
        verifiedVersion: "codex-cli 0.165.0",
        writeStdin() { wrote = true; return { state: "closed" }; },
        readStdout() { return { state: "eof" }; },
        readStderr() { return { state: "eof" }; },
        closeStdin() {},
        signal(value) { signalled = value; },
        poll() { return { state: "exited", code: 0, signal: null }; },
        async release() { released = true; },
      };
    } },
  };
  await assert.rejects(spawnCodexAppServer(context, clientInfo), /Codex version mismatch/);
  assert.equal(signalled, "terminate");
  assert.equal(released, true);
  assert.equal(wrote, false);
});

test("initialize response version mismatch stops and reaps its child", async () => {
  let response;
  let signalled;
  let released = false;
  const child = {
    verifiedVersion: lock.codexCliVersion,
    writeStdin(bytes) {
      const request = JSON.parse(new TextDecoder().decode(bytes).trim());
      response = new TextEncoder().encode(JSON.stringify({
        id: request.id, result: { userAgent: "ghosterm-protocol-test/0.165.0 (fixture)" },
      }) + "\n");
      return { state: "data", count: bytes.length };
    },
    readStdout(maxBytes) {
      if (!response) return { state: "wait" };
      const bytes = response.subarray(0, maxBytes);
      response = response.subarray(maxBytes);
      if (!response.length) response = undefined;
      return { state: "data", bytes };
    },
    readStderr() { return { state: "wait" }; },
    closeStdin() {},
    signal(value) { signalled = value; },
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
  await pumpInitialization(server);
  await assert.rejects(server.ready, /Codex app-server version mismatch: expected 0\.166\.0, got 0\.165\.0/);
  assert.equal(signalled, "terminate");
  assert.equal(released, true);
  assert.equal(server.reaped, true);
  assert.deepEqual(server.exit, { state: "exited", code: 0, signal: null });
});

test("naturally exited Codex child drains both pipes and is reaped without an explicit stop", async () => {
  let released = false;
  let signalled = false;
  const child = {
    verifiedVersion: lock.codexCliVersion,
    writeStdin(bytes) { return { state: "data", count: bytes.length }; },
    readStdout() { return { state: "eof" }; },
    readStderr() { return { state: "eof" }; },
    closeStdin() {},
    signal() { signalled = true; },
    poll() { assert.equal(released, false, "poll must not use a released Native handle"); return { state: "exited", code: 0, signal: null }; },
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

test("runs the production process helper against the pinned real server, without a turn", async () => {
  const executable = process.env.GHOSTERM_CODEX ?? "codex";
  const version = execFileSync(executable, ["--version"], { cwd: root, encoding: "utf8" }).trim();
  assert.equal(version, lock.codexCliVersion);

  const temp = mkdtempSync(join(root, "tmp", "codex-protocol-live-"));
  const home = join(temp, "home"), config = join(temp, "config"), cwd = join(temp, "project");
  for (const path of [home, config, cwd]) mkdirSync(path);
  const env = { HOME: home, XDG_CONFIG_HOME: config, TMPDIR: join(root, "tmp"), PATH: process.env.PATH ?? "" };
  const readQueue = (queue, ended, maxBytes) => {
    if (!queue.length) return { state: ended() ? "eof" : "wait" };
    const head = queue[0];
    const bytes = head.subarray(0, maxBytes);
    if (bytes.length === head.length) queue.shift();
    else queue[0] = head.subarray(maxBytes);
    return { state: "data", bytes };
  };
  let child;
  let childExit;
  let exitPromise;
  const processService = { async spawn(options) {
    assert.equal(options.program, "codex");
    assert.deepEqual(options.argv, ["codex", "app-server"]);
    assert.equal(options.cwd.id, "isolated-codex-project");
    assert.equal(options.envPolicy, "selected-user-environment");
    child = spawn(executable, options.argv.slice(1), { cwd, env, stdio: ["pipe", "pipe", "pipe"] });
    const stdoutChunks = [], stderrChunks = [];
    let stdoutEnded = false, stderrEnded = false;
    child.stdout.on("data", chunk => stdoutChunks.push(Uint8Array.from(chunk)));
    child.stderr.on("data", chunk => stderrChunks.push(Uint8Array.from(chunk)));
    child.stdout.on("end", () => { stdoutEnded = true; });
    child.stderr.on("end", () => { stderrEnded = true; });
    exitPromise = new Promise(resolveExit => child.once("exit", (code, signal) => {
      childExit = { state: "exited", code, signal: signal === null ? null : signal };
      resolveExit();
    }));
    child.stdin.on("error", () => {});
    return {
      verifiedVersion: version,
      readStdout(maxBytes) { return readQueue(stdoutChunks, () => stdoutEnded, maxBytes); },
      readStderr(maxBytes) { return readQueue(stderrChunks, () => stderrEnded, maxBytes); },
      writeStdin(bytes) {
        if (!child.stdin.writable || child.stdin.destroyed) return { state: "closed" };
        child.stdin.write(Buffer.from(bytes));
        return { state: "data", count: bytes.length };
      },
      closeStdin() { child.stdin.end(); },
      signal(value) { child.kill(value === "kill" ? "SIGKILL" : "SIGTERM"); },
      poll() { return childExit ?? { state: "running" }; },
      async release() { await exitPromise; },
    };
  } };
  const context = {
    side: "executor",
    capabilities: { query: () => "allowed" },
    cancellation: { cancelled: false },
    invocation: { workspaceHandle: { id: "isolated-codex-project" } },
    process: processService,
  };
  const server = await spawnCodexAppServer(context, clientInfo);
  const conversation = server.createConversation();
  const pump = setInterval(() => server.step(), 2);

  const withTimeout = (promise, ms = 20_000) => {
    let timer;
    return Promise.race([promise, new Promise((_, reject) => {
      timer = setTimeout(() => reject(new Error("live app-server timed out")), ms);
    })]).finally(() => clearTimeout(timer));
  };
  try {
    const initialization = await withTimeout(server.ready);
    assert.equal(typeof initialization.userAgent, "string");
    const threadID = await withTimeout(conversation.startThread({ cwd, approvalPolicy: "on-request", sandbox: "read-only" }));
    assert.equal(typeof threadID, "string");
    assert.equal(server.stdio.failure, undefined, server.stdio.failure?.message);
    assert.deepEqual(server.exit, { state: "running" });
    console.log(`CODEX APP-SERVER HELPER PASS: ${version}; verified mapping, initialize, initialized, conversation thread/start, bounded pipes; no turn/model request`);
  } finally {
    clearInterval(pump);
    const closing = conversation.close();
    assert.equal(conversation.close(), closing);
    await withTimeout(closing, 3_000).catch(() => {});
    if (child && !childExit) child.kill("SIGKILL");
    rmSync(temp, { recursive: true, force: true });
  }
  assert.deepEqual(server.exit?.state, "exited");
  assert.equal(server.reaped, true);
});


test("permissions retain immutable scope, use their own envelope, and cannot expand or persist grants", async () => {
  const { client, writes } = await fakeActiveClient();
  let request;
  client.onServerRequest(value => { request = value; });
  const params = { threadId: "thread-a", turnId: "turn-b", itemId: "permission-1", environmentId: null,
    startedAtMs: 1, cwd: "/owned", reason: "only this turn", permissions: {
      network: null, fileSystem: { read: ["/owned/e\u0301"], write: ["/owned/é"], entries: [
        { path: { type: "glob_pattern", pattern: "/owned/*.txt" }, access: "write" },
        { path: { type: "special", value: { kind: "project_roots", subpath: null } }, access: "read" }] } } };
  client.receiveLine(JSON.stringify({ id: "permissions-1", method: "item/permissions/requestApproval", params }));
  assert.equal(request.method, "item/permissions/requestApproval");
  assert.throws(() => { request.params.permissions.fileSystem.write.push("/"); }, TypeError);
  for (const decision of ["accept", { permissions: {}, scope: "session" },
    { permissions: { network: { enabled: true } }, scope: "turn" },
    { permissions: { fileSystem: { read: null, write: ["/"] } }, scope: "turn" },
    { permissions: {}, scope: "turn", strictAutoReview: false }]) {
    const count = writes.length;
    assert.throws(() => client.respond("permissions-1", decision), /permission decision/);
    assert.equal(writes.length, count, "invalid permission scope must never reach the child");
  }
  const grant = { permissions: { fileSystem: params.permissions.fileSystem }, scope: "turn" };
  client.respond("permissions-1", grant);
  assert.deepEqual(writes.at(-1), { jsonrpc: "2.0", id: "permissions-1", result: grant });
  assert.throws(() => client.respond("permissions-1", grant), /unknown/);
  client.receiveLine(JSON.stringify({ id: 99, method: "item/permissions/requestApproval", params }));
  client.respond(99, { permissions: {}, scope: "turn" });
  assert.deepEqual(writes.at(-1).result, { permissions: {}, scope: "turn" }, "denial grants no overlay");
  const { environmentId, reason, ...minimal } = params;
  client.receiveLine(JSON.stringify({ id: "optional-nulls", method: "item/permissions/requestApproval", params: {
    ...minimal, permissions: { fileSystem: { write: ["/owned"], entries: null } } } }));
  assert.deepEqual(request.params.permissions, { network: null, fileSystem: { read: null, write: ["/owned"] } },
    "official JSON Schema permits omitted nullable defaults");
  client.respond("optional-nulls", { permissions: { fileSystem: { read: null, write: ["/owned"] } }, scope: "turn" });
  assert.deepEqual(writes.at(-1).result.permissions, { fileSystem: { read: null, write: ["/owned"] } });
});

test("permissions reject unknown scope fields before any approval reaches a listener", async () => {
  const { client } = await fakeActiveClient();
  let count = 0, failure;
  client.onServerRequest(() => count++); client.onProtocolError(error => { failure = error; });
  client.receiveLine(JSON.stringify({ id: 1, method: "item/permissions/requestApproval", params: {
    threadId: "t", turnId: "u", itemId: "i", environmentId: null, startedAtMs: 1, cwd: "/owned", reason: null,
    permissions: { network: { enabled: true, hiddenGrant: true }, fileSystem: null } } }));
  assert.equal(count, 0); assert.match(failure.message, /invalid or unsupported/);
});
