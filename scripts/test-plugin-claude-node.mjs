import assert from "node:assert/strict";
import test from "node:test";
import { createRequire } from "node:module";
import { pathToFileURL } from "node:url";
import { join, resolve } from "node:path";
import { EventEmitter, once } from "node:events";
import { PassThrough, Writable } from "node:stream";
import { spawn, execFileSync } from "node:child_process";
import { readFileSync, mkdtempSync, mkdirSync, rmSync } from "node:fs";

const directory = resolve(process.env.GHOSTERM_CLAUDE_NODE_PACKAGE);
const require = createRequire(join(directory, "package.json"));
const sdk = await import(pathToFileURL(require.resolve("@anthropic-ai/claude-agent-sdk")));
const { ClaudeApprovalBroker, claudeOptions, CLAUDE_CLI_VERSION, CLAUDE_SDK_VERSION } =
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
  stdin = new Writable({ write: (chunk, _, done) => { try { this.receive(chunk.toString()); done(); } catch (e) { done(e); } } });
  pending = "";
  emitFrame(value) { this.stdout.write(JSON.stringify(value) + "\n"); }
  receive(chunk) {
    this.pending += chunk;
    for (;;) {
      const newline = this.pending.indexOf("\n"); if (newline < 0) return;
      const message = JSON.parse(this.pending.slice(0, newline)); this.pending = this.pending.slice(newline + 1);
      this.sent.push(message);
      if (message.type === "control_request" && message.request.subtype === "initialize") {
        this.emitFrame({ type: "control_response", response: { subtype: "success", request_id: message.request_id,
          response: { commands: [], agents: [], models: [], output_style: "default", available_output_styles: [], account: {} } } });
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

if (process.env.GHOSTERM_CLAUDE_REAL_CLI === "1") test("locked real Claude CLI initializes and exits without a prompt in an isolated no-network process", async () => {
  const base = mkdtempSync(join(process.env.TMPDIR, "claude-real-"));
  const broker = new ClaudeApprovalBroker("real-cli"); let child; let warm; const writes = [];
  const binary = require.resolve(`@anthropic-ai/claude-agent-sdk-${process.platform}-${process.arch}/claude`);
  for (const name of ["home", "config", "workspace"]) mkdirSync(join(base, name));
  const environment = { HOME: join(base, "home"), CLAUDE_CONFIG_DIR: join(base, "config"), TMPDIR: process.env.TMPDIR,
    PATH: process.env.PATH, CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC: "1" };
  try {
    const version = execFileSync(binary, ["--version"], { env: environment, encoding: "utf8" }).trim();
    assert.equal(version, `${CLAUDE_CLI_VERSION} (Claude Code)`);
    const options = claudeOptions({ ...profile(broker), cwd: join(base, "workspace"), executable: binary,
      environment, tools: [], spawn: request => {
        assert.equal(request.command, binary);
        child = spawn("/usr/bin/sandbox-exec", ["-p", '(version 1)(allow default)(deny network*)(deny mach-lookup (global-name "com.apple.securityd"))',
          request.command, ...request.args], { cwd: request.cwd, env: request.env, stdio: ["pipe", "pipe", "pipe"] });
        child.stderr.resume();
        const original = child.stdin.write.bind(child.stdin);
        child.stdin.write = (chunk, ...args) => { writes.push(chunk.toString()); return original(chunk, ...args); };
        return child;
      } });
    warm = await sdk.startup({ options, initializeTimeoutMs: 15000 });
    const frames = writes.join("").split("\n").filter(Boolean).map(value => JSON.parse(value));
    assert(!frames.some(value => value.type === "user"), "real CLI preflight must not send any prompt");
    assert.equal(broker.pending.length, 0);
  } finally {
    broker.close(); warm?.close();
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
