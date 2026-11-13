import assert from "node:assert/strict";
import test from "node:test";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { join, resolve } from "node:path";
import { EventEmitter, once } from "node:events";
import { PassThrough, Writable } from "node:stream";
import { spawn, execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { readFileSync, writeFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";

const directory = resolve(process.env.GHOSTERM_CLAUDE_NODE_PACKAGE);
const require = createRequire(join(directory, "package.json"));
const sdk = await import(pathToFileURL(require.resolve("@anthropic-ai/claude-agent-sdk")));
const { ClaudeApprovalBroker, ClaudeNodeSession, ClaudeStdioBridge, serveClaudeStdio, claudeOptions, CLAUDE_CLI_VERSION, CLAUDE_SDK_VERSION } =
  await import(pathToFileURL(join(directory, "dist/index.js")));
assert.equal(JSON.parse(readFileSync(join(directory, "node_modules/@anthropic-ai/claude-agent-sdk/package.json"))).version, CLAUDE_SDK_VERSION);
const context = (id = "request-1", signal = new AbortController().signal) =>
  ({ requestId: id, toolUseID: "tool-1", signal });
const drain = broker => { while (broker.takeEvent()) {} };
const event = broker => { const value = broker.takeEvent(); assert(value); return value; };

test("Claude approval binds scope and frozen input, resolves once, and replays only its identical live request", async () => {
  const broker = new ClaudeApprovalBroker("scope-1");
  const input = { command: "printf '中文😀'", nested: { value: 1 } };
  const waiting = broker.canUseTool("Bash", input, context());
  const request = event(broker).request;
  const duplicate = broker.canUseTool("Bash", { nested: { value: 1 }, command: "printf '中文😀'" }, context());
  assert.strictEqual(duplicate, waiting, "pending Claude replay must share its original decision");
  assert.equal(broker.takeEvent(), undefined);
  input.nested.value = 9;
  assert.equal(request.input.nested.value, 1, "Claude approval must retain its captured input");
  assert(Object.isFrozen(request.input.nested));
  assert.equal(broker.decide({ ...request.reference, scope: "scope-2" }, true), false,
    "foreign Claude scope must not authorize a callback");
  assert.equal(broker.decide({ ...request.reference, token: "approval-0" }, true), false);
  assert.equal(broker.decide(request.reference, true), true);
  const result = await waiting;
  assert.equal(result.behavior, "allow");
  assert.deepEqual(JSON.parse(JSON.stringify(result.updatedInput)), { command: "printf '中文😀'", nested: { value: 1 } });
  assert.equal(broker.decide(request.reference, true), false, "Claude approval must resolve once");
  const replay = await broker.canUseTool("Bash", { nested: { value: 1 }, command: "printf '中文😀'" }, context());
  assert.strictEqual(replay, result);
  assert.equal(event(broker).kind, "resolved");
  assert.equal(broker.takeEvent(), undefined, "identical replay must not create a second approval prompt");
  broker.close();
});

test("Claude decline preserves hints but never grants suggested persistent permissions", async () => {
  const broker = new ClaudeApprovalBroker("decline");
  const pending = broker.canUseTool("Bash", { command: "fixture" }, { ...context(), defaultToNo: true,
    suppressAlwaysAllowRule: true, suggestions: [{ type: "setMode", mode: "bypassPermissions", destination: "userSettings" }] });
  const request = event(broker).request;
  assert.equal(request.defaultToNo, true); assert.equal(request.suppressAlwaysAllowRule, true);
  assert.equal(broker.decide(request.reference, false), true);
  const result = await pending;
  assert.equal(result.behavior, "deny");
  assert.equal(result.updatedPermissions, undefined);
  assert.equal(result.updatedInput, undefined);
  broker.close();
});

test("Claude backend cancellation expires a prompt without synthesizing a decline", async () => {
  const broker = new ClaudeApprovalBroker("cancel");
  const abort = new AbortController();
  const pending = broker.canUseTool("Bash", { command: "fixture" }, context("request-1", abort.signal));
  const request = event(broker).request;
  abort.abort();
  await assert.rejects(pending, { name: "AbortError" });
  assert.equal(event(broker).kind, "expired");
  assert.equal(broker.decide(request.reference, true), false, "cancelled Claude request must never accept a late decision");
  await assert.rejects(broker.canUseTool("Bash", { command: "fixture" }, context()), { name: "AbortError" });
  assert.equal(broker.pending.length, 0);
  broker.close();
});

test("Claude approval tokens do not survive a new broker even when caller scope and request id repeat", async () => {
  const original = new ClaudeApprovalBroker("same-caller-scope");
  const old = original.canUseTool("Bash", { command: "fixture" }, context());
  const reference = event(original).request.reference;
  original.close(); await assert.rejects(old, { name: "AbortError" });
  const replacement = new ClaudeApprovalBroker("same-caller-scope");
  const pending = replacement.canUseTool("Bash", { command: "fixture" }, context());
  const current = event(replacement).request.reference;
  assert.equal(replacement.decide(reference, true), false, "Claude references must not cross broker generations");
  assert(replacement.decide(current, false)); await pending;
  replacement.close();
});

test("Claude backend request ids cannot replay an approval across explicit user turns", async () => {
  const broker = new ClaudeApprovalBroker("turn-identity");
  broker.beginTurn("user-one");
  const first = broker.canUseTool("Bash", { command: "fixture" }, context());
  const request = event(broker).request;
  assert.equal(request.turnID, "user-one"); assert(broker.decide(request.reference, true)); await first;
  drain(broker); broker.beginTurn("user-two");
  const reused = broker.canUseTool("Bash", { command: "fixture" }, context());
  assert(broker.closed, "Claude approval identity must include the submitted user turn");
  await assert.rejects(reused, /reused/);
});

test("Claude request identity collisions close all pending work while distinct Unicode ids remain separate", async () => {
  const broker = new ClaudeApprovalBroker("identity");
  const a = broker.canUseTool("Bash", { command: "a" }, context("é"));
  const b = broker.canUseTool("Bash", { command: "b" }, context("e\u0301"));
  assert.equal(broker.pending.length, 2);
  const collision = broker.canUseTool("Bash", { command: "a-changed" }, context("é"));
  assert(broker.closed, "Claude reused request IDs must not bind changed input");
  await assert.rejects(collision, /reused/);
  await assert.rejects(a, /reused/); await assert.rejects(b, /reused/);
  assert(broker.closed);
  assert.equal(broker.retainedBytes, 0);
  assert.equal(event(broker).kind, "closed");
});

test("Claude MCP provenance stays immutable and changed sources cannot reuse an approval", async () => {
  const broker = new ClaudeApprovalBroker("mcp-source");
  const source = { name: "backend-reported", source: "future-unknown-source" };
  const pending = broker.canUseTool("mcp__service__write", { path: "fixture" }, { ...context(), mcpServer: source,
    title: "Backend prompt", displayName: "Write", description: "Backend scope" });
  const request = event(broker).request;
  source.source = "sdk";
  assert.equal(request.mcpServer.source, "future-unknown-source", "Claude MCP provenance must not upgrade an unknown source");
  assert(Object.isFrozen(request.mcpServer));
  assert.equal(request.title, "Backend prompt");
  const replacement = broker.canUseTool("mcp__service__write", { path: "fixture" }, { ...context(), mcpServer: source,
    title: "Backend prompt", displayName: "Write", description: "Backend scope" });
  assert(broker.closed, "Claude request identities must retain MCP provenance");
  await assert.rejects(replacement, /reused/); await assert.rejects(pending, /reused/);
});

test("Claude retained budgets charge unread event payloads and never reuse evicted approval tokens", async () => {
  const broker = new ClaudeApprovalBroker("budgets", { pending: 1, retained: 1, bytes: 4096 });
  const first = broker.canUseTool("Bash", { command: "a" }, context("one"));
  const ref = broker.pending[0].reference;
  await assert.rejects(broker.canUseTool("Bash", { command: "b" }, context("two")), /budget/);
  assert(broker.decide(ref, true)); await first;
  const over = broker.canUseTool("Bash", { command: "b" }, context("two"));
  assert.equal(broker.pending.length, 0, "unread Claude request events must retain their byte reservation");
  await assert.rejects(over, /budget/);
  drain(broker);
  const second = broker.canUseTool("Bash", { command: "b" }, context("two"));
  const current = event(broker).request.reference;
  assert.notEqual(current.token, ref.token);
  assert.equal(broker.decide(ref, true), false);
  assert(broker.decide(current, false)); await second;
  broker.close();
  assert.equal(broker.retainedCount, 0); assert.equal(broker.retainedBytes, 0);
});

test("Claude input rejects getters proxies malformed Unicode and oversized data before publishing", async () => {
  const broker = new ClaudeApprovalBroker("json");
  let invoked = 0;
  const getter = {}; Object.defineProperty(getter, "command", { enumerable: true, get() { invoked++; return "bad"; } });
  const proxy = new Proxy({}, { getPrototypeOf() { invoked++; return Object.prototype; } });
  const cycle = {}; cycle.self = cycle;
  for (const input of [getter, proxy, cycle, { value: "\ud800" }, { value: NaN }, { value: 9007199254740992 },
    { value: "x".repeat(65_536) }]) {
    await assert.rejects(broker.canUseTool("Bash", input, context()));
  }
  assert.equal(invoked, 0, "Claude snapshot must not invoke application getters or proxy traps");
  assert.equal(broker.takeEvent(), undefined); assert.equal(broker.retainedBytes, 0);
  broker.close();
});

const questionInput = { questions: [{ question: "Which format?", header: "Format", multiSelect: false,
  options: [{ label: "Short", description: "Brief" }, { label: "Long", description: "Detailed" }] }] };
test("Claude questions require explicit complete answers and cannot be approved as ordinary tools", async () => {
  const broker = new ClaudeApprovalBroker("questions");
  const waiting = broker.canUseTool("AskUserQuestion", questionInput, context());
  const request = event(broker).request;
  assert.equal(request.kind, "question");
  assert.equal(broker.decide(request.reference, true), false, "Claude questions cannot accept an empty generic approval");
  assert.throws(() => broker.answer(request.reference, {}), /Every Claude question/);
  assert.throws(() => broker.answer(request.reference, { "Which format?": "", unknown: "bad" }));
  assert.equal(broker.answer(request.reference, { "Which format?": "A custom answer 中文" }), true);
  const result = await waiting;
  assert.equal(result.behavior, "allow");
  assert.equal(result.updatedInput.answers["Which format?"], "A custom answer 中文");
  assert.deepEqual(JSON.parse(JSON.stringify(result.updatedInput.questions)), questionInput.questions);
  assert.equal(result.updatedPermissions, undefined);
  broker.close();
});

function profile(broker, spawnProcess = () => { throw new Error("fixture must not spawn"); }) {
  return { cwd: process.env.TMPDIR, executable: "/explicit/claude", cliVersion: CLAUDE_CLI_VERSION,
    environment: { HOME: process.env.HOME, CLAUDE_CONFIG_DIR: process.env.CLAUDE_CONFIG_DIR, TMPDIR: process.env.TMPDIR,
      PATH: process.env.PATH }, spawn: spawnProcess, approvals: broker, abortController: new AbortController(), tools: ["Bash", "AskUserQuestion"] };
}

test("Claude profile requires a live explicit owner and preserves selected configuration boundaries", () => {
  const broker = new ClaudeApprovalBroker("profile");
  const config = profile(broker);
  assert.throws(() => claudeOptions({ ...config, cliVersion: "2.1.119" }), /locked SDK profile/);
  assert.throws(() => claudeOptions({ ...config, spawn: undefined }), /explicit process owner/);
  const options = claudeOptions(config);
  assert.equal(typeof options.spawnClaudeCodeProcess, "function");
  assert.equal(options.canUseTool, broker.canUseTool);
  assert.equal(options.permissionMode, "default", "Claude profile must not bypass backend permissions");
  assert.equal(options.allowDangerouslySkipPermissions, false);
  assert.deepEqual(options.settingSources, []); assert.deepEqual(options.allowedTools, []);
  assert.deepEqual(options.mcpServers, {}); assert.deepEqual(options.plugins, []);
  assert.equal(options.strictMcpConfig, true); assert.equal(options.persistSession, false);
  config.environment.HOME = "/changed";
  assert.notEqual(options.env.HOME, "/changed");
  assert.equal(options.env.ANTHROPIC_API_KEY, undefined);
  let starts = 0;
  const owned = claudeOptions({ ...config, spawn: () => { starts++; return { owned: true }; } });
  const request = { command: config.executable, cwd: config.cwd, args: [], env: owned.env, signal: config.abortController.signal };
  assert.throws(() => owned.spawnClaudeCodeProcess({ ...request, command: "/wrong/claude" }), /execution target/);
  assert.equal(starts, 0);
  owned.spawnClaudeCodeProcess(request);
  assert.equal(starts, 1);
  assert.throws(() => owned.spawnClaudeCodeProcess(request), /cannot replay/, "Claude profile must not replay a process launch");
  const rebuilt = claudeOptions({ ...config, spawn: () => { starts++; return { owned: true }; } });
  assert.throws(() => rebuilt.spawnClaudeCodeProcess(request), /cannot replay/, "Claude scopes must not respawn through rebuilt options");
  broker.close();
  assert.throws(() => options.spawnClaudeCodeProcess(request), /no longer live/, "a closed Claude scope must not start its prepared profile");
  assert.throws(() => claudeOptions(config), /no longer live/);
});

class ProtocolFixture extends EventEmitter {
  stdout = new PassThrough(); killed = false; exitCode = null; signalCode = null; sent = [];
  initialize = true;
  stdin = new Writable({ write: (chunk, _, done) => { try { this.receive(chunk.toString()); done(); } catch (e) { done(e); } } });
  pending = "";
  emitFrame(value) { this.stdout.write(JSON.stringify(value) + "\n"); }
  receive(chunk) {
    this.pending += chunk;
    for (;;) {
      const newline = this.pending.indexOf("\n"); if (newline < 0) return;
      const message = JSON.parse(this.pending.slice(0, newline)); this.pending = this.pending.slice(newline + 1);
      this.sent.push(message);
      if (this.initialize && message.type === "control_request" && message.request.subtype === "initialize") {
        this.emitFrame({ type: "control_response", response: { subtype: "success", request_id: message.request_id,
          response: { commands: [], agents: [], models: [], output_style: "default", available_output_styles: [], account: {} } } });
      }
      if (message.type === "control_request" && message.request.subtype === "interrupt") {
        this.emitFrame({ type: "control_response", response: { subtype: "success", request_id: message.request_id,
          response: { still_queued: [], cancelled: [] } } });
      }
    }
  }
  kill(signal = "SIGTERM") {
    if (this.killed) return false;
    this.killed = true; this.signalCode = signal;
    this.stdout.end(); queueMicrotask(() => this.emit("exit", null, signal)); return true;
  }
  permission(id, tool = "Bash", input = { command: "fixture only" }) {
    this.emitFrame({ type: "control_request", request_id: id,
      request: { subtype: "can_use_tool", tool_name: tool, input, tool_use_id: "sdk-tool" } });
  }
}

async function until(predicate, label) {
  const end = Date.now() + 5000;
  while (!predicate() && Date.now() < end) await new Promise(resolve => setTimeout(resolve, 5));
  assert(predicate(), label);
}

async function sessionFixture(limits = {}) {
  const broker = new ClaudeApprovalBroker("session");
  const child = new ProtocolFixture(); let starts = 0;
  const session = new ClaudeNodeSession(profile(broker, () => { starts++; return child; }), limits);
  await session.start();
  return { session, child, broker, starts };
}
function resultFrame(id, extra = {}) {
  return { type: "result", subtype: "success", is_error: false, user_message_uuid: id,
    uuid: "fixture-result", session_id: "fixture-session", result: "fixture result only", ...extra };
}
async function stopFixture({ session, child }) {
  session.close();
  await until(() => child.killed, "session must eventually stop its SDK fixture process");
}

test("Claude session streams correlated input and ordered immutable SDK events across turns", async () => {
  const value = await sessionFixture(); const { session, child, broker } = value;
  try {
    assert.equal(session.phase, "ready"); assert.equal(value.starts, 1);
    assert(!child.sent.some(value => value.type === "user"));
    const id = session.send("fixture input 中文😀");
    assert.throws(() => session.send("overlapping input"), /not ready/);
    await until(() => child.sent.some(value => value.type === "user"), "SDK must receive streaming input");
    const user = child.sent.find(value => value.type === "user");
    assert.equal(user.uuid, id); assert.equal(user.message.content, "fixture input 中文😀");
    child.permission("turn-one-pending");
    await until(() => broker.pending.length === 1, "session must route SDK approval");
    assert.equal(broker.pending[0].turnID, id, "Claude session must bind approval identity to its submitted input");
    const ref = broker.pending[0].reference;
    child.emitFrame({ type: "assistant", uuid: "fixture-assistant", session_id: "fixture-session", message: { content: "fixture reply" } });
    child.emitFrame(resultFrame(id));
    await until(() => session.phase === "ready", "correlated result must end this input");
    assert.equal(broker.decide(ref, true), false, "Claude turn boundary must expire pending prompts");
    const a = session.takeEvent(), b = session.takeEvent();
    assert.equal(a.message.type, "assistant"); assert.equal(b.message.type, "result");
    assert(a.sequence < b.sequence); assert(Object.isFrozen(a.message.message));
    const next = session.send("second explicit input"); assert.notEqual(next, id);
    await until(() => child.sent.filter(value => value.type === "user").length === 2, "SDK must accept next explicit input");
    child.emitFrame(resultFrame(next, { subtype: "error_during_execution", is_error: true, errors: ["fixture failure"] }));
    await until(() => session.phase === "ready", "backend error result must remain distinct from transport loss");
    assert.equal(session.takeEvent().message.is_error, true); assert.equal(session.failure, undefined);
    await assert.rejects(session.start(), /only start once/);
  } finally { await stopFixture(value); }
});

test("Claude session interrupt expires prompts and preserves receipt without fabricating turn completion", async () => {
  const value = await sessionFixture(); const { session, child, broker } = value;
  try {
    const id = session.send("fixture interrupt");
    await until(() => child.sent.some(value => value.type === "user"), "SDK input not sent");
    child.permission("interrupt-pending");
    await until(() => broker.pending.length === 1, "SDK approval not received");
    const ref = broker.pending[0].reference;
    const one = session.interrupt(), duplicate = session.interrupt();
    assert.strictEqual(one, duplicate, "duplicate interrupt must share one SDK control request");
    await one;
    assert.equal(session.phase, "interrupting", "Claude interrupt receipt must not complete a turn");
    assert.equal(session.pendingMessageID, id);
    assert.equal(broker.decide(ref, true), false, "Claude interrupt must expire pending approval immediately");
    assert.throws(() => session.send("too early"), /not ready/);
    assert.equal(child.sent.filter(value => value.request?.subtype === "interrupt").length, 1);
    const receipt = session.takeEvent(); assert.equal(receipt.kind, "interrupt-receipt"); assert.equal(receipt.messageID, id);
    child.emitFrame(resultFrame(id, { stop_reason: "interrupted" }));
    await until(() => session.phase === "ready", "interrupt still needs a correlated result");
  } finally { await stopFixture(value); }
});

test("Claude session EOF and explicit close preserve an unresolved input as unknown", async () => {
  for (const explicit of [false, true]) {
    const value = await sessionFixture(); const { session, child, broker } = value;
    try {
      const id = session.send("fixture unfinished input");
      await until(() => child.sent.some(value => value.type === "user"), "SDK input not sent");
      if (explicit) session.close(); else child.stdout.end();
      await until(() => ["closed", "lost", "failed"].includes(session.phase), "session must observe EOF/close");
      assert.equal(session.phase, explicit ? "closed" : "lost", "Claude EOF must not be treated as successful completion");
      const observation = session.takeEvent();
      assert.equal(observation.pendingMessageID, id, "Claude close must preserve unresolved input identity");
      assert(broker.closed); assert.throws(() => session.send("late input"), /not ready/);
      session.close(); assert.equal(session.takeEvent(), undefined);
    } finally { await stopFixture(value); }
  }
  const value = await sessionFixture(); const { session, child } = value;
  try {
    const id = session.send("fixture result immediately before EOF");
    await until(() => child.sent.some(value => value.type === "user"), "SDK input not sent");
    child.emitFrame(resultFrame(id)); child.stdout.end();
    await until(() => session.phase === "lost", "SDK EOF must finish after draining buffered output");
    assert.equal(session.takeEvent().message.user_message_uuid, id, "Claude EOF must preserve a buffered result");
    assert.equal(session.takeEvent().pendingMessageID, null);
  } finally { await stopFixture(value); }
});

test("Claude session preserves uncorrelated backend authentication diagnostics without claiming a result", async () => {
  const value = await sessionFixture(); const { session, child } = value;
  try {
    const id = session.send("fixture authentication failure");
    child.emitFrame(resultFrame("foreign-input", { subtype: "error_during_execution", is_error: true,
      startup_failure_reason: "fixture-unknown-reason", errors: ["fixture authentication required"] }));
    await until(() => session.phase !== "running", "uncorrelated result must terminate observation");
    assert.equal(session.phase, "lost", "Claude foreign result must not complete the active input");
    const diagnostic = session.takeEvent(); assert.equal(diagnostic.message.errors[0], "fixture authentication required");
    assert.equal(diagnostic.message.startup_failure_reason, "fixture-unknown-reason");
    assert.equal(session.takeEvent().pendingMessageID, id);
  } finally { await stopFixture(value); }
});

test("Claude session enforces unread events aggregate output and lifetime budgets", async () => {
  for (const kind of ["events", "bytes", "output", "lifetime"]) {
    const limits = kind === "events" ? { events: 2 } : kind === "bytes" ? { queuedBytes: 1024 } :
      kind === "output" ? { outputBytes: 1024 } : {};
    const value = await sessionFixture(limits); const { session, child } = value;
    try {
      if (kind === "lifetime") {
        // A fresh session with a short deadline also covers startup cancellation.
        await stopFixture(value);
        const broker = new ClaudeApprovalBroker("deadline"), late = new ProtocolFixture(); late.initialize = false;
        const timed = new ClaudeNodeSession(profile(broker, () => late), { lifetimeMs: 20 });
        const starting = timed.start();
        await assert.rejects(starting);
        assert.equal(timed.phase, "failed", "Claude deadline must stop a stalled SDK startup");
        await until(() => late.killed, "deadline must close its SDK child");
        continue;
      }
      const id = session.send("fixture output limit");
      const frame = { type: "system", subtype: "fixture", text: kind === "bytes" ? "x".repeat(700) : "x".repeat(550) };
      child.emitFrame(frame);
      if (kind !== "bytes") {
        await until(() => session.queuedBytes > 0, "session did not retain SDK output");
        if (kind === "output") session.takeEvent();
        child.emitFrame(frame);
      }
      await until(() => session.phase === "failed", `Claude ${kind} budget must stop the session`);
      assert(session.queuedBytes <= (limits.queuedBytes ?? 2_097_152));
      const events = []; for (let item; (item = session.takeEvent());) events.push(item);
      assert.equal(events.at(-1).kind, "closed"); assert.equal(events.at(-1).pendingMessageID, id);
      assert.equal(session.queuedBytes, 0);
    } finally { await stopFixture(value); }
  }
});

test("Claude session close during startup rejects late initialization and blocks replacement spawn", async () => {
  const broker = new ClaudeApprovalBroker("late-start"), child = new ProtocolFixture(); child.initialize = false;
  let starts = 0;
  const session = new ClaudeNodeSession(profile(broker, () => { starts++; return child; }));
  const starting = session.start();
  await until(() => child.sent.some(value => value.request?.subtype === "initialize"), "initialize request not received");
  session.close(); await assert.rejects(starting);
  assert.equal(session.phase, "closed"); assert.equal(starts, 1);
  await assert.rejects(session.start(), /only start once/);
  await until(() => child.killed, "closing startup must stop its SDK child");
});

test("Claude session rejects invalid prompts and oversized backend messages before publication", async () => {
  const value = await sessionFixture(); const { session, child } = value;
  try {
    for (const prompt of ["", "\ud800", "x".repeat(65_537)]) assert.throws(() => session.send(prompt));
    assert(!child.sent.some(value => value.type === "user"));
    const id = session.send("fixture oversized output");
    child.emitFrame({ type: "system", subtype: "fixture", text: "x".repeat(524_288) });
    await until(() => session.phase === "failed", "Claude oversized SDK output must fail before publishing");
    assert.equal(session.takeEvent().pendingMessageID, id); assert.equal(session.takeEvent(), undefined);
  } finally { await stopFixture(value); }
});

test("pinned real Claude SDK routes its control protocol through the production approval broker", async () => {
  const broker = new ClaudeApprovalBroker("sdk-control");
  const child = new ProtocolFixture(); let spawns = 0;
  const warm = await sdk.startup({ options: claudeOptions(profile(broker, () => { spawns++; return child; })), initializeTimeoutMs: 5000 });
  try {
    assert.equal(spawns, 1);
    child.permission("original:审批");
    await until(() => broker.pending.length === 1, "real SDK did not invoke canUseTool");
    const request = event(broker).request;
    assert.equal(request.reference.requestID, "original:审批");
    assert.equal(request.toolUseID, "sdk-tool");
    assert(broker.decide(request.reference, true));
    await until(() => child.sent.some(value => value.type === "control_response" && value.response.request_id === "original:审批"), "SDK did not send its correlated response");
    const reply = child.sent.find(value => value.type === "control_response" && value.response.request_id === "original:审批");
    assert.equal(reply.response.response.behavior, "allow");
    assert.equal(reply.response.response.updatedInput.command, "fixture only");
    assert.equal(reply.response.response.updatedPermissions, undefined);
    assert.equal(child.sent.filter(value => value.type === "control_response" && value.response.request_id === "original:审批").length, 1);
    drain(broker);
    child.permission("cancel-next");
    await until(() => broker.pending.length === 1, "SDK did not deliver second prompt");
    const old = event(broker).request.reference;
    child.emitFrame({ type: "control_cancel_request", request_id: "cancel-next" });
    await until(() => broker.pending.length === 0, "SDK cancel did not expire its broker prompt");
    assert.equal(broker.decide(old, true), false);
    await new Promise(resolve => setImmediate(resolve));
    assert(!child.sent.some(value => value.type === "control_response" && value.response.request_id === "cancel-next" &&
      ["allow", "deny"].includes(value.response.response?.behavior)), "SDK cancellation must not synthesize a permission choice");
    drain(broker);
    child.permission("question-next", "AskUserQuestion", questionInput);
    await until(() => broker.pending.length === 1, "SDK did not route its user question");
    const question = event(broker).request;
    assert.equal(question.kind, "question");
    assert(broker.answer(question.reference, { "Which format?": "Short" }));
    await until(() => child.sent.some(value => value.type === "control_response" && value.response.request_id === "question-next"), "SDK did not return question answers");
    assert.equal(child.sent.find(value => value.type === "control_response" && value.response.request_id === "question-next")
      .response.response.updatedInput.answers["Which format?"], "Short");
    assert(!child.sent.some(value => value.type === "user"), "SDK control test must not send a model prompt");
  } finally { broker.close(); warm.close(); }
  await until(() => child.killed, "SDK close must eventually stop its owned fixture process");
});

function canonical(value) {
  if (Array.isArray(value)) return value.map(canonical);
  if (value && typeof value === "object") return Object.fromEntries(Object.keys(value).sort().map(key => [key, canonical(value[key])]));
  return value;
}
function stdioRequest(method, sequence, params = {}) {
  return JSON.stringify(canonical({ version: 1, id: `command-${sequence}`, sequence, method, params }));
}
function stdioFixture() {
  const broker = new ClaudeApprovalBroker("stdio"), child = new ProtocolFixture(); let spawns = 0;
  const session = new ClaudeNodeSession(profile(broker, () => { spawns++; return child; }));
  const bridge = new ClaudeStdioBridge(session, broker);
  return { broker, child, session, bridge, get spawns() { return spawns; } };
}
async function stdioReady(value) {
  await value.bridge.handle(stdioRequest("start", 1));
  await until(() => value.session.phase === "ready", "stdio session did not initialize");
}

test("Claude stdio dispatches explicit actions and identical retries never send twice", async () => {
  const value = stdioFixture(); const { bridge, session, child, broker } = value;
  try {
    const start = stdioRequest("start", 1);
    const first = bridge.handle(start), duplicate = bridge.handle(start);
    assert.strictEqual(first, duplicate, "Claude stdio pending retry must share its request");
    assert.equal(JSON.parse(await first).result.requested, true);
    await until(() => session.phase === "ready", "stdio start must reach SDK ready");
    assert.equal(value.spawns, 1);
    const send = stdioRequest("send", 2, { text: "fixture stdio 中文😀" });
    const response = await bridge.handle(send);
    assert.equal(await bridge.handle(send), response, "Claude stdio retry must replay its identical response");
    const id = JSON.parse(response).result.messageID;
    await until(() => child.sent.some(item => item.type === "user"), "stdio send not delivered to SDK");
    assert.equal(child.sent.filter(item => item.type === "user").length, 1, "Claude stdio replay must not duplicate an SDK input");
    child.permission("stdio-tool");
    await until(() => broker.pending.length === 1, "SDK stdio approval missing");
    const poll = JSON.parse(await bridge.handle(stdioRequest("poll", 3)));
    assert.equal(poll.result.source, "approval");
    const reference = poll.result.event.request.reference;
    assert.equal(JSON.parse(await bridge.handle(stdioRequest("approve", 4, { reference, allow: true }))).result.accepted, true);
    child.permission("stdio-question", "AskUserQuestion", questionInput);
    await until(() => broker.pending.length === 1, "SDK stdio question missing");
    const questionRef = broker.pending[0].reference;
    const answer = JSON.parse(await bridge.handle(stdioRequest("answer", 5, { reference: questionRef, answers: { "Which format?": "Short" } })));
    assert.equal(answer.result.accepted, true);
    const interrupt = JSON.parse(await bridge.handle(stdioRequest("interrupt", 6)));
    assert.equal(interrupt.result.requested, true); assert.equal(interrupt.result.phase, "interrupting");
    const stop = JSON.parse(await bridge.handle(stdioRequest("stop", 7)));
    assert.equal(stop.result.pendingMessageID, id); assert(bridge.explicitStop);
    assert.throws(() => session.send("late"));
  } finally { await stopFixture(value); }
});

test("Claude stdio poll batches bound count and bytes without losing fair ordered events", async () => {
  const value = stdioFixture(); const { session, child, broker, bridge } = value;
  try {
    await session.start(); session.send("explicit batch fixture");
    await until(() => child.sent.some(item => item.type === "user"), "batch input missing");
    for (let i = 0; i < 30; i++) child.emitFrame({ type: "system", subtype: "batch-fixture", index: i, payload: "x".repeat(1000) });
    const large = "中😀".repeat(50_000);
    child.emitFrame({ type: "system", subtype: "batch-large", index: 30, payload: large });
    for (let i = 0; i < 3; i++) child.permission(`batch-approval-${i}`);
    await until(() => broker.pending.length === 3, "batch approvals missing");
    const first = JSON.parse(await bridge.handle(stdioRequest("poll", 1, { maxEvents: 4 }))).result;
    assert.deepEqual(first.events.map(item => item.source), ["approval", "session", "approval", "session"],
      "Claude batch must honor requested count and alternate event sources");
    const all = [...first.events]; let sequence = 1;
    for (let i = 0; i < 40 && !all.some(item => item.event.message?.subtype === "batch-large"); i++) {
      const response = await bridge.handle(stdioRequest("poll", ++sequence, { maxEvents: 32 }));
      assert(Buffer.byteLength(response) <= 600_000);
      const result = JSON.parse(response).result;
      assert.equal(result.source, "batch"); assert(result.events.length <= 32);
      const big = result.events.findIndex(item => item.event.message?.subtype === "batch-large");
      if (big >= 0) {
        assert.equal(big, result.events.length - 1);
        assert(result.events.slice(0, big).reduce((sum, item) => sum + Buffer.byteLength(JSON.stringify(item)), 0) < 4096,
          "Claude batch must bound the prefix before one complete large event");
      }
      all.push(...result.events);
    }
    const messages = all.filter(item => item.source === "session").map(item => item.event);
    assert.deepEqual(messages.map(item => item.message.index), Array.from({ length: 31 }, (_, i) => i));
    assert.deepEqual(messages.map(item => item.sequence), Array.from({ length: 31 }, (_, i) => i + 1));
    assert.equal(messages.at(-1).message.payload, large, "Claude batching must retain a whole large SDK event");
    assert.equal(all.filter(item => item.source === "approval").length, 3);
  } finally { await stopFixture(value); }
});

test("Claude stdio rejects invalid explicit poll batch limits before consuming events", async () => {
  for (const maxEvents of [0, 33, 1.5, true, "2", null]) {
    const value = stdioFixture();
    try {
      await value.session.start();
      await assert.rejects(value.bridge.handle(stdioRequest("poll", 1, { maxEvents })), /batch limit/,
        "Claude poll batch limits must reject out-of-range and noninteger values");
    } finally { await stopFixture(value); }
  }
});

test("Claude stdio changed or evicted sequences cannot authorize another action", async () => {
  for (const evicted of [false, true]) {
    const value = stdioFixture(); const { bridge, session } = value;
    const first = stdioRequest("poll", 1);
    await bridge.handle(first);
    if (evicted) for (let n = 2; n <= 40; n++) await bridge.handle(stdioRequest("poll", n));
    const stale = evicted ? first : stdioRequest("start", 1);
    await assert.rejects(bridge.handle(stale), /replay|sequence/, "Claude stdio stale or changed sequence must fail closed");
    assert.equal(session.phase, "closed"); assert.equal(value.spawns, 0);
    assert.equal(bridge.explicitStop, false);
  }
});

test("Claude stdio refuses configuration injection ambiguous JSON and nonexplicit decisions", async () => {
  const ref = { scope: "stdio", requestID: "one", token: "test" };
  const frames = [stdioRequest("start", 1, { executable: "/untrusted/claude" }),
    stdioRequest("approve", 1, { reference: ref, allow: "yes" }), stdioRequest("answer", 1, { reference: ref, answers: { q: false } }),
    stdioRequest("unknown", 1), stdioRequest("poll", 2), " " + stdioRequest("poll", 1),
    '{"id":"command-1","method":"poll","method":"start","params":{},"sequence":1,"version":1}',
    stdioRequest("poll", 1).replace('"version":1', '"version":2'), "x".repeat(65_537)];
  for (const frame of frames) {
    const value = stdioFixture();
    try {
      await assert.rejects(value.bridge.handle(frame), undefined, "Claude stdio invalid frame must not dispatch");
      assert.equal(value.spawns, 0); assert.equal(value.session.phase, "closed");
    } finally { value.bridge.close(); if (value.spawns) await stopFixture(value); }
  }
});

test("Claude stdio framing preserves split UTF8 and rejects malformed or incomplete byte frames", async () => {
  const request = Buffer.from(stdioRequest("poll", 1).replace("command-1", "审批😀"));
  for (let split = 0; split <= request.length; split++) {
    const value = stdioFixture(), input = new PassThrough(), output = new PassThrough(); let transcript = "";
    output.on("data", chunk => { transcript += chunk.toString(); });
    const serving = serveClaudeStdio(input, output, value.bridge);
    input.write(request.subarray(0, split)); input.write(request.subarray(split)); input.write("\n");
    await until(() => transcript.includes("\n"), "split UTF8 frame was not handled");
    input.write(stdioRequest("stop", 2) + "\n");
    assert.equal(await serving, "stopped");
    assert.equal(JSON.parse(transcript.split("\n")[0]).id, "审批😀");
    input.destroy(); output.destroy();
  }
  const invalidUtf8 = Buffer.from(stdioRequest("poll", 1)); invalidUtf8[invalidUtf8.indexOf("command-1")] = 255;
  for (const bytes of [Buffer.from([255, 10]), Buffer.from('{"unfinished":'), Buffer.alloc(65_537, 120), invalidUtf8]) {
    const value = stdioFixture(), input = new PassThrough(), output = new PassThrough();
    output.resume(); const serving = serveClaudeStdio(input, output, value.bridge);
    try {
      if (bytes === invalidUtf8) input.write(Buffer.concat([bytes, Buffer.from("\n" + stdioRequest("stop", 2) + "\n")]));
      else input.end(bytes);
      await assert.rejects(serving, undefined, "Claude stdio invalid bytes must fail");
      assert(value.bridge.stopped);
    } finally { input.destroy(); output.destroy(); }
  }
});

test("Claude stdio write backpressure prevents dispatching the next action", async () => {
  const value = stdioFixture(), input = new PassThrough(); let release; let writes = 0;
  const output = new Writable({ write: (_, __, done) => { if (++writes === 1) release = done; else done(); } });
  const serving = serveClaudeStdio(input, output, value.bridge);
  void serving.catch(() => {});
  try {
    input.write(stdioRequest("poll", 1) + "\n");
    await until(() => release !== undefined, "response write not started");
    input.write(stdioRequest("start", 2) + "\n");
    await new Promise(resolve => setTimeout(resolve, 20));
    assert.equal(value.spawns, 0, "Claude stdio must respect response backpressure before dispatch");
    release(); await until(() => value.session.phase === "ready", "start did not proceed after drain");
    input.write(stdioRequest("stop", 3) + "\n"); assert.equal(await serving, "stopped");
  } finally { input.destroy(); output.destroy(); await stopFixture(value); }
});

test("Claude stdio transport loss and broken output close their SDK scope without a stop receipt", async () => {
  for (const broken of [false, true]) {
    const value = stdioFixture(), input = new PassThrough();
    await stdioReady(value);
    const output = broken ? new Writable({ write: (_, __, done) => done(new Error("fixture pipe failure")) }) : new PassThrough();
    if (!broken) output.resume();
    const serving = serveClaudeStdio(input, output, value.bridge);
    try {
      if (broken) { input.write(stdioRequest("poll", 2) + "\n"); await assert.rejects(serving); }
      else { input.end(); assert.equal(await serving, "eof", "Claude stdio EOF must not become an explicit stop"); }
      assert.equal(value.bridge.explicitStop, false); assert(value.broker.closed);
      await until(() => value.child.killed, "lost protocol must eventually stop SDK child");
    } finally { input.destroy(); output.destroy(); await stopFixture(value); }
  }
});

if (process.env.GHOSTERM_CLAUDE_REAL_CLI === "1") test("real Node executor pipes initialize stop and EOF against the locked no-network CLI", async () => {
  const base = mkdtempSync(join(process.env.TMPDIR, "claude-executor-"));
  const binary = require.resolve(`@anthropic-ai/claude-agent-sdk-${process.platform}-${process.arch}/claude`);
  const digest = createHash("sha256").update(readFileSync(binary)).digest("hex");
  try {
    for (const mode of ["stop", "eof", "wrong-digest"]) {
      const work = join(base, mode); mkdirSync(work);
      for (const name of ["home", "config", "workspace"]) mkdirSync(join(work, name));
      const environment = { HOME: join(work, "home"), CLAUDE_CONFIG_DIR: join(work, "config"), TMPDIR: process.env.TMPDIR,
        PATH: process.env.PATH, CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC: "1" };
      const profilePath = join(work, "profile.json");
      writeFileSync(profilePath, JSON.stringify({ version: 1, cwd: join(work, "workspace"), executable: binary,
        cliVersion: CLAUDE_CLI_VERSION, sha256: mode === "wrong-digest" ? "0".repeat(64) : digest,
        environmentNames: Object.keys(environment), tools: [] }));
      const helper = spawn("/usr/bin/sandbox-exec", ["-p", '(version 1)(allow default)(deny network*)(deny mach-lookup (global-name "com.apple.securityd"))',
        process.execPath, join(directory, "dist/cli.js"), "--profile", profilePath], { env: environment, stdio: ["pipe", "pipe", "pipe"] });
      const exit = once(helper, "exit"); let stdout = "", stderr = "", sequence = 0;
      helper.stdout.on("data", bytes => { stdout += bytes.toString(); }); helper.stderr.on("data", bytes => { stderr += bytes.toString(); });
      async function call(method) {
        const n = ++sequence;
        helper.stdin.write(stdioRequest(method, n) + "\n");
        await until(() => stdout.split("\n").filter(Boolean).some(line => JSON.parse(line).sequence === n), "real Node executor reply missing");
        const value = stdout.split("\n").filter(Boolean).map(line => JSON.parse(line)).find(value => value.sequence === n);
        assert.equal(value.ok, true); return value.result;
      }
      try {
        if (mode === "wrong-digest") helper.stdin.end();
        if (mode !== "wrong-digest") {
          assert.equal((await call("start")).requested, true);
          let ready = false;
          for (let n = 0; n < 200 && !ready; n++) {
            const value = await call("poll"); ready = value.phase === "ready";
            if (!ready) await new Promise(resolve => setTimeout(resolve, 10));
          }
          assert(ready, "real Node executor must initialize its owned CLI");
          if (mode === "stop") assert.equal((await call("stop")).phase, "closed");
          else helper.stdin.end();
        }
        const [code, signal] = await Promise.race([exit, new Promise((_, reject) => {
          const timeout = setTimeout(() => reject(new Error("real executor did not stop")), 10_000); timeout.unref();
          exit.finally(() => clearTimeout(timeout));
        })]);
        assert.equal(code, mode === "stop" ? 0 : mode === "eof" ? 7 : 2, "real Node executor must distinguish stop EOF and invalid profile");
        assert.equal(signal, null);
        assert.equal(stderr, mode === "wrong-digest" ? "claude-node: PROFILE_OR_RUNTIME_ERROR\n" : "");
        if (mode === "wrong-digest") assert.equal(stdout, "");
      } finally {
        if (helper.exitCode === null && helper.signalCode === null) { helper.kill("SIGKILL"); await exit; }
      }
    }
  } finally { rmSync(base, { recursive: true, force: true }); }
});

if (process.env.GHOSTERM_CLAUDE_REAL_CLI === "1") test("locked real Claude CLI initializes and exits without a prompt in an isolated no-network process", async () => {
  const base = mkdtempSync(join(process.env.TMPDIR, "claude-real-"));
  const broker = new ClaudeApprovalBroker("real-cli"); let child; let session; const writes = [];
  const binary = require.resolve(`@anthropic-ai/claude-agent-sdk-${process.platform}-${process.arch}/claude`);
  for (const name of ["home", "config", "workspace"]) mkdirSync(join(base, name));
  const environment = { HOME: join(base, "home"), CLAUDE_CONFIG_DIR: join(base, "config"), TMPDIR: process.env.TMPDIR,
    PATH: process.env.PATH, CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC: "1" };
  try {
    const version = execFileSync(binary, ["--version"], { env: environment, encoding: "utf8" }).trim();
    assert.equal(version, `${CLAUDE_CLI_VERSION} (Claude Code)`);
    const configuration = { ...profile(broker), cwd: join(base, "workspace"), executable: binary,
      environment, tools: [], spawn: request => {
        assert.equal(request.command, binary);
        child = spawn("/usr/bin/sandbox-exec", ["-p", '(version 1)(allow default)(deny network*)(deny mach-lookup (global-name "com.apple.securityd"))',
          request.command, ...request.args], { cwd: request.cwd, env: request.env, stdio: ["pipe", "pipe", "pipe"] });
        child.stderr.resume();
        const original = child.stdin.write.bind(child.stdin);
        child.stdin.write = (chunk, ...args) => { writes.push(chunk.toString()); return original(chunk, ...args); };
        return child;
      } };
    session = new ClaudeNodeSession(configuration);
    await session.start();
    assert.equal(session.phase, "ready");
    const frames = writes.join("").split("\n").filter(Boolean).map(value => JSON.parse(value));
    assert(!frames.some(value => value.type === "user"), "real CLI preflight must not send any prompt");
    assert.equal(broker.pending.length, 0);
  } finally {
    session?.close(); broker.close();
    if (child && child.exitCode === null && child.signalCode === null) {
      child.kill("SIGTERM");
      let timer;
      try {
        await Promise.race([once(child, "exit"), new Promise((_, reject) => {
          timer = setTimeout(() => { child.kill("SIGKILL"); reject(new Error("owned Claude child did not exit")); }, 5000);
        })]);
      } finally { clearTimeout(timer); }
    }
    rmSync(base, { recursive: true, force: true });
  }
});
