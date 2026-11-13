import assert from "node:assert/strict";
import { CODEX_CLI_VERSION } from "../plugins/official/ai-codex/protocol/version.ts";
import { activate } from "../plugins/official/ai-codex/executor/index.ts";

class FakeCodexChild {
  verifiedVersion = CODEX_CLI_VERSION;
  input = "";
  output = [];
  writes = [];
  exit = { state: "running" };
  reaped = false;
  signals = 0;
  writeStdin(bytes) {
    const chunk = new TextDecoder().decode(bytes);
    this.input += chunk;
    for (;;) {
      const newline = this.input.indexOf("\n");
      if (newline < 0) break;
      const line = this.input.slice(0, newline);
      this.input = this.input.slice(newline + 1);
      if (line) this.receive(JSON.parse(line));
    }
    return { state: "data", count: bytes.length };
  }
  receive(message) {
    this.writes.push(message);
    if (message.method === "initialize") {
      this.line({ id: message.id, result: { userAgent: "codex/0.166.0 (fake executor)" } });
    } else if (message.method === "thread/start") {
      this.line({ id: message.id, result: { thread: { id: "thread-test" } } });
    } else if (message.method === "turn/start") {
      this.line({ method: "turn/started", params: { threadId: "thread-test",
        turn: turnFixture("inProgress") } });
      this.line({ id: "approval-41", method: "item/commandExecution/requestApproval",
        params: { threadId: "thread-test", turnId: "turn-test", itemId: "item-1", startedAtMs: 1 } });
      this.line({ method: "item/agentMessage/delta", params: { threadId: "thread-test",
        turnId: "turn-test", itemId: "item-2", delta: "hello" } });
      this.line({ method: "item/fileChange/patchUpdated", params: { threadId: "thread-test",
        turnId: "turn-test", itemId: "item-3", changes: [{ path: "src/example.ts",
          kind: { type: "update", move_path: null }, diff: "@@ -1 +1 @@\n" + "+x".repeat(5000) }] } });
      this.line({ id: message.id, result: { turn: { id: "turn-test", status: "inProgress" } } });
    } else if (message.id === "approval-41") {
      assert.deepEqual(message.result, { decision: "accept" });
      this.line({ method: "serverRequest/resolved", params: { threadId: "thread-test", requestId: "approval-41" } });
      this.line({ method: "turn/completed", params: { threadId: "thread-test",
        turn: turnFixture("completed") } });
    } else if (message.method === "turn/interrupt") {
      this.line({ id: message.id, result: {} });
      this.line({ method: "turn/completed", params: { threadId: "thread-test",
        turn: turnFixture("interrupted") } });
    }
  }
  line(value) { this.output.push(new TextEncoder().encode(JSON.stringify(value) + "\n")); }
  readStdout(maxBytes) {
    const chunk = this.output[0];
    if (!chunk) return { state: "wait" };
    const bytes = chunk.subarray(0, maxBytes);
    if (bytes.length === chunk.length) this.output.shift();
    else this.output[0] = chunk.subarray(maxBytes);
    return { state: "data", bytes };
  }
  readStderr() { return { state: "wait" }; }
  closeStdin() {}
  signal() { this.signals++; this.exit = { state: "exited", code: 0, signal: null }; }
  poll() { return this.exit; }
  async release() { this.exit = { state: "exited", code: 0, signal: null }; this.reaped = true; }
}

function turnFixture(status) {
  return { id: "turn-test", rootTurnId: null, items: [], itemsView: "full", status,
    error: null, startedAt: 1, completedAt: status === "inProgress" ? null : 2, durationMs: 1 };
}

function fixtureContext({ side = "executor", command = "ai codex", allowed = true } = {}) {
  const handlers = new Map();
  const events = [];
  const child = new FakeCodexChild();
  let spawnCount = 0;
  const context = {
    side,
    invocation: { command, workspaceHandle: { id: "workspace" } },
    instanceId: "fixture-instance",
    epoch: "1",
    cancellation: { get cancelled() { return false; } },
    capabilities: { query(name) { return allowed && ["process.spawn", "process.pipes"].includes(name) ? "allowed" : "denied"; } },
    process: { async spawn(options) {
      spawnCount++;
      if (!allowed) throw new Error("spawn attempted without Executor process grants");
      assert.deepEqual(options, { program: "codex", argv: ["app-server"], cwd: { id: "workspace" },
        envPolicy: "selected-user-environment", cancellation: context.cancellation });
      return child;
    } },
    session: {
      register(method, handler) { assert(!handlers.has(method.name)); handlers.set(method.name, handler); },
      request(method, input, timeoutMs) {
        assert.equal(typeof input, "object", "Native Host methods require object input");
        assert.equal(typeof input.event, "string");
        method.input.parse(input);
        events.push({ method: method.name, input: input.event, timeoutMs });
        return { outcome: Promise.resolve({ kind: "success", value: { accepted: true } }), cancel() {} };
      },
    },
  };
  return { context, handlers, events, child, get spawnCount() { return spawnCount; } };
}

async function driveUntil(disposable, predicate, label) {
  for (let i = 0; i < 200 && !predicate(); i++) {
    disposable.step();
    await Promise.resolve();
    await Promise.resolve();
  }
  assert(predicate(), `timed out driving ${typeof label === "function" ? label() : label}`);
}

const fixture = fixtureContext();
const disposable = activate(fixture.context);
assert.equal(fixture.spawnCount, 0, "activation must not spawn before an admitted thread request");
assert.deepEqual([...fixture.handlers.keys()], [
  "codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond",
]);

const start = fixture.handlers.get("codex.thread.start")({}, { check() {} });
let startResult;
start.then(value => { startResult = value; });
await driveUntil(disposable, () => startResult !== undefined, "initialize and thread/start");
assert.deepEqual(startResult, { threadID: "thread-test" });
assert.equal(fixture.child.writes.filter(value => value.method === "thread/start").length, 1);

const turn = fixture.handlers.get("codex.turn.send")({ text: "hello" }, { check() {} });
let turnResult;
turn.then(value => { turnResult = value; });
await driveUntil(disposable, () => turnResult !== undefined && fixture.events.some(event =>
  event.method === "codex.event" && event.input.includes("approval-request")), "turn and event delivery");
assert.deepEqual(turnResult, { turnID: "turn-test" });

const approval = fixture.handlers.get("codex.approval.respond")({
  requestID: "s:approval-41", decisionJSON: "\"accept\"",
}, { check() {} });
assert.deepEqual(await approval, { accepted: true });
await driveUntil(disposable, () => fixture.events.some(event =>
  event.method === "codex.event" && event.input.includes("turn/completed")), "turn completion delivery");
assert.equal(fixture.child.writes.filter(value => value.id === "approval-41").length, 1);
assert(fixture.events.some(event => event.input.includes("item/agentMessage/delta")));
const fileChangeEvent = JSON.parse(fixture.events.find(event => event.input.includes("patchUpdated")).input);
assert.equal(fileChangeEvent.notification.params._ghosterm.truncated, true);
assert.equal(fileChangeEvent.notification.params._ghosterm.diffTruncated, true);
assert(fileChangeEvent.notification.params.changes[0].diff.includes("diff 已截断"));
assert(new TextEncoder().encode(JSON.stringify({ event: JSON.stringify(fileChangeEvent) })).length <= 3072,
  "file change diff events must fit the complete Native RPC body");
assert(fixture.events.every(event => event.method === "codex.event" && event.timeoutMs === 30000));

let turnCancelled = false;
let cancelledTurnFailure;
const cancelledTurn = fixture.handlers.get("codex.turn.send")({ text: "cancel me" }, {
  get cancelled() { return turnCancelled; },
  check() { if (turnCancelled) throw new Error("cancelled"); },
});
void cancelledTurn.catch(error => { cancelledTurnFailure = error; });
turnCancelled = true;
await driveUntil(disposable, () => cancelledTurnFailure !== undefined &&
  fixture.child.writes.some(value => value.method === "turn/interrupt"), "cancelled turn interrupt");
assert.match(cancelledTurnFailure.message, /cancelled/i);
assert.equal(fixture.child.writes.filter(value => value.method === "turn/interrupt").length, 1);

disposable.dispose();
disposable.dispose();
for (let i = 0; i < 8; i++) await Promise.resolve();
assert.equal(fixture.child.reaped, true, "disposing the Executor must stop and reap Codex");
assert.equal(fixture.child.signals, 1, "repeated disposal must not signal the child twice");

const denied = fixtureContext({ allowed: false });
const deniedDisposable = activate(denied.context);
await assert.rejects(denied.handlers.get("codex.thread.start")({}, { check() {} }),
  /authorized Executor process grant/);
assert.equal(denied.spawnCount, 0, "missing process grants must reject before spawn");
deniedDisposable.dispose();

const cancelled = fixtureContext();
const cancelledDisposable = activate(cancelled.context);
let cancellationRequested = false;
let startFailure;
const cancelledStart = cancelled.handlers.get("codex.thread.start")({}, {
  get cancelled() { return cancellationRequested; },
  check() { if (cancellationRequested) throw new Error("cancelled"); },
});
void cancelledStart.catch(error => { startFailure = error; });
for (let i = 0; i < 20 && cancelled.spawnCount === 0; i++) await Promise.resolve();
assert.equal(cancelled.spawnCount, 1);
cancellationRequested = true;
cancelledDisposable.step();
for (let i = 0; i < 12 && !startFailure; i++) await Promise.resolve();
assert.match(startFailure?.message ?? "", /closed|cancelled|unknown/i);
assert.equal(cancelled.child.reaped, true, "cancelling initialization must stop and reap the child");
assert.equal(cancelled.child.writes.some(value => value.method === "thread/start"), false);
cancelledDisposable.dispose();

console.log("CODEX EXECUTOR ENTRY PASS: lazy spawn, initialize, thread, turn, approval, events, cancel, reap, grant refusal");
