import assert from "node:assert/strict";
import { CODEX_CLI_VERSION } from "../plugins/official/ai-codex/protocol/version.ts";
import { pathToFileURL } from "node:url";
const { activate } = await import(process.env.GHOSTERM_CODEX_PACKAGE_ENTRY
  ? pathToFileURL(process.env.GHOSTERM_CODEX_PACKAGE_ENTRY).href
  : "../plugins/official/ai-codex/executor/index.ts");
const { CodexChangeStore, CODEX_CHANGE_MAX_BYTES, CODEX_CHANGES_MAX_BYTES, CODEX_CHANGES_MAX_ITEMS } =
  await import(process.env.GHOSTERM_CODEX_PACKAGE_ENTRY
    ? new URL("changes.js", pathToFileURL(process.env.GHOSTERM_CODEX_PACKAGE_ENTRY)).href
    : "../plugins/official/ai-codex/executor/changes.ts");

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
      assert.deepEqual(options, { program: "codex", argv: ["codex", "app-server"], cwd: { id: "workspace" },
        envPolicy: "selected-user-environment", cancellation: context.cancellation });
      return child;
    } },
    session: {
      register(method, handler) {
        assert(!handlers.has(method.name));
        handlers.set(method.name, (input, request) => handler(method.input.parse(input), request));
      },
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
  "codex.thread.start", "codex.turn.send", "codex.turn.interrupt", "codex.approval.respond", "codex.change.read",
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
  requestID: "s:approval-41", decisionJSON: "\"accept\"", snapshotID: "",
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
const snapshotID = fileChangeEvent.notification.params._ghosterm.snapshotID;
assert.equal(typeof snapshotID, "string", "a partial preview must retain its complete snapshot reference");
function readComplete(read, snapshotID) {
  let content = "", offset = 0, totalBytes;
  for (let count = 0; count < 4096; count++) {
    const chunk = read(snapshotID, offset);
    assert.equal(chunk.status, "ok");
    assert.equal(chunk.snapshotID, snapshotID);
    assert.equal(chunk.offset, offset);
    assert.equal(chunk.nextOffset, offset + chunk.content.length);
    assert(new TextEncoder().encode(JSON.stringify({ kind: "success", value: chunk })).length <= 3072,
      "complete snapshot chunks must fit the RPC outcome body");
    totalBytes ??= chunk.totalBytes;
    assert.equal(chunk.totalBytes, totalBytes);
    content += chunk.content;
    offset = chunk.nextOffset;
    if (chunk.done) {
      assert.equal(offset, chunk.totalUnits);
      assert.equal(new TextEncoder().encode(content).length, totalBytes);
      return JSON.parse(content);
    }
    assert(chunk.content.length > 0);
  }
  assert.fail("snapshot read did not finish within its bounded budget");
}
const completeChange = readComplete((snapshotID, offset) => fixture.handlers.get("codex.change.read")(
  { snapshotID, offset }, { check() {} }), snapshotID);
assert.equal(completeChange.itemId, "item-3");
assert.equal(completeChange.changes[0].diff, "@@ -1 +1 @@\n" + "+x".repeat(5000));
const widePath = "p".repeat(4096);
fixture.child.line({ method: "item/fileChange/patchUpdated", params: {
  threadId: "thread-test", turnId: "turn-test", itemId: "wide-item",
  changes: Array.from({ length: 10 }, (_, index) => ({
    path: index === 0 ? widePath : `src/file-${index}.ts`, kind: { type: "add" }, diff: `+complete ${index}`,
  })),
  backendDetail: "x".repeat(4000),
} });
await driveUntil(disposable, () => fixture.events.some(event =>
  JSON.parse(event.input).notification?.params?.itemId === "wide-item"), "minimal preview retaining full snapshot");
const wide = JSON.parse(fixture.events.find(event => JSON.parse(event.input).notification?.params?.itemId === "wide-item").input);
assert.deepEqual(wide.notification.params.changes, []);
assert.equal(wide.notification.params._ghosterm.omittedChanges, 10);
const wideDocument = readComplete((snapshotID, offset) => fixture.handlers.get("codex.change.read")(
  { snapshotID, offset }, { check() {} }), wide.notification.params._ghosterm.snapshotID);
assert.equal(wideDocument.changes[0].path, widePath, "snapshot identity must select the requested document");
assert.equal(wideDocument.changes.length, 10, "complete snapshots must retain files omitted from the preview");
assert.equal(wideDocument.changes[9].diff, "+complete 9");
assert(new TextEncoder().encode(JSON.stringify({ event: JSON.stringify(fileChangeEvent) })).length <= 3072,
  "file change diff events must fit the complete Native RPC body");
assert(fixture.events.every(event => event.method === "codex.event" && event.timeoutMs === 30000));

const expired = fixtureContext();
const expiredDisposable = activate(expired.context);
let expiredStart;
expired.handlers.get("codex.thread.start")({}, { check() {} }).then(value => { expiredStart = value; });
await driveUntil(expiredDisposable, () => expiredStart !== undefined, "expiry fixture thread");
let expiredTurn;
expired.handlers.get("codex.turn.send")({ text: "wait for approval" }, { check() {} })
  .then(value => { expiredTurn = value; });
await driveUntil(expiredDisposable, () => expiredTurn !== undefined && expired.events.some(event =>
  JSON.parse(event.input).kind === "approval-request"), "pending approval before completion");
expired.child.line({ method: "turn/completed", params: {
  threadId: "thread-test", turn: turnFixture("interrupted"),
} });
await driveUntil(expiredDisposable, () => expired.events.some(event =>
  JSON.parse(event.input).notification?.method === "turn/completed"), "completion without approval resolution");
const expiredWrites = expired.child.writes.length;
assert.throws(() => expired.handlers.get("codex.approval.respond")({
  requestID: "s:approval-41", decisionJSON: "\"accept\"", snapshotID: "",
}, { check() {} }), /unknown or expired|not pending/,
"Executor must refuse approval after turn completion without a backend resolution");
assert.equal(expired.child.writes.length, expiredWrites, "an expired approval must not reach the child pipe");
expiredDisposable.dispose();
for (let i = 0; i < 8; i++) await Promise.resolve();
assert.equal(expired.child.reaped, true);

const snapshots = new CodexChangeStore();
function capture(itemId, diff, method = "item/fileChange/patchUpdated") {
  const item = { id: itemId, type: "fileChange", changes: [
    { path: "完整/😀.ts", kind: { type: "update", move_path: "new/path.ts" }, diff },
  ] };
  const stamped = snapshots.capture(method, { threadId: "thread", turnId: "turn",
    ...(method === "item/fileChange/patchUpdated" ? { itemId, changes: item.changes } : { item }) });
  return method === "item/fileChange/patchUpdated" ? stamped._ghosterm : stamped.item._ghosterm;
}
const unicodeDiff = '😀' + '中文'.repeat(1000) + '😀中文\\\u0000"\n'.repeat(2000);
const unicode = capture("unicode", unicodeDiff, "item/started");
assert.equal(capture("unicode", unicodeDiff, "item/completed").snapshotID, unicode.snapshotID,
  "an identical backend patch must not invalidate an in-progress review");
assert.equal(readComplete((id, at) => snapshots.read(id, at), unicode.snapshotID).changes[0].diff, unicodeDiff);
const eof = snapshots.read(unicode.snapshotID, unicode.snapshotUnits);
assert.equal(eof.status, "ok");
assert.equal(eof.done, true);
assert.equal(eof.content, "");
assert.equal(snapshots.read(unicode.snapshotID, unicode.snapshotUnits + 1).status, "invalid");
const firstChunk = snapshots.read(unicode.snapshotID, 0);
const surrogate = firstChunk.content.indexOf("😀");
assert(surrogate >= 0);
assert.equal(snapshots.read(unicode.snapshotID, surrogate + 1).status, "invalid");
assert.equal(snapshots.read(unicode.snapshotID, -1).status, "invalid");
const replacement = capture("unicode", "replacement", "item/completed");
assert.notEqual(replacement.snapshotID, unicode.snapshotID);
assert.equal(snapshots.read(unicode.snapshotID, 0).status, "expired", "changed snapshot IDs must not read replacement content");
assert.equal(snapshots.read(unicode.snapshotID, firstChunk.nextOffset).status, "expired");
const overLimit = capture("unicode", "x".repeat(CODEX_CHANGE_MAX_BYTES));
assert.equal(overLimit.snapshotUnavailable, "limit", "oversized snapshots must be unavailable");
assert.equal(snapshots.read(replacement.snapshotID, 0).status, "expired");
assert.equal(snapshots.retainedBytes, 0);
const evicted = capture("old", "old");
for (let i = 0; i < CODEX_CHANGES_MAX_ITEMS; i++) capture(`small-${i}`, "small");
assert.equal(snapshots.retainedItems, CODEX_CHANGES_MAX_ITEMS);
assert.equal(snapshots.read(evicted.snapshotID, 0).status, "expired");
for (let i = 0; i < 4; i++) {
  capture(`large-${i}`, "x".repeat(350000));
  assert(snapshots.retainedBytes <= CODEX_CHANGES_MAX_BYTES);
}
snapshots.clear();
assert.equal(snapshots.retainedBytes, 0);
assert.equal(snapshots.retainedItems, 0);
assert.equal(snapshots.read(evicted.snapshotID, 0).status, "expired");
assert.notEqual(capture("old", "new").snapshotID, evicted.snapshotID);

// Drive the real Executor entry over an isolated fake app-server pipe. No model or user files.
const bound = fixtureContext();
const boundDisposable = activate(bound.context);
let boundStarted, boundTurn;
bound.handlers.get("codex.thread.start")({}, { check() {} }).then(value => { boundStarted = value; });
await driveUntil(boundDisposable, () => boundStarted !== undefined, "bound approval thread");
bound.handlers.get("codex.turn.send")({ text: "review patch" }, { check() {} }).then(value => { boundTurn = value; });
await driveUntil(boundDisposable, () => boundTurn !== undefined && bound.events.some(event =>
  JSON.parse(event.input).notification?.method === "item/fileChange/patchUpdated"), "bound patch capture");
function boundPatch(itemID = "item-3") {
  const rows = bound.events.map(event => JSON.parse(event.input)).filter(event =>
    event.notification?.method === "item/fileChange/patchUpdated" && event.notification.params.itemId === itemID);
  return rows.at(-1)?.notification.params._ghosterm.snapshotID;
}
async function fileApproval(id, itemID = "item-3") {
  bound.child.line({ id, method: "item/fileChange/requestApproval", params: {
    threadId: "thread-test", turnId: "turn-test", itemId: itemID, startedAtMs: 1,
  } });
  await driveUntil(boundDisposable, () => bound.events.some(event => JSON.parse(event.input).request?.id === id),
    "file approval request");
}
function decideFile(id, snapshotID, decision = "accept") {
  return bound.handlers.get("codex.approval.respond")({ requestID: `s:${id}`,
    decisionJSON: JSON.stringify(decision), snapshotID }, { check() {} });
}
await fileApproval("file-1");
const oldPatch = boundPatch();
assert.throws(() => bound.handlers.get("codex.approval.respond")({
  requestID: "s:file-1", decisionJSON: '"accept"',
}, { check() {} }), /SCHEMA/, "approval codec requires an explicit snapshot field");
let boundWrites = bound.child.writes.length;
assert.throws(() => decideFile("file-1", ""), /patch changed or expired/,
  "file approval must require a complete snapshot revision");
assert.equal(bound.child.writes.length, boundWrites, "missing patch revision must not write to Codex");
bound.child.line({ method: "item/fileChange/patchUpdated", params: { threadId: "thread-test", turnId: "turn-test",
  itemId: "other-item", changes: [{ path: "other", kind: { type: "add" }, diff: "+other" }] } });
await driveUntil(boundDisposable, () => boundPatch("other-item") !== undefined, "other item patch");
assert.throws(() => decideFile("file-1", boundPatch("other-item")), /patch changed or expired/,
  "file approval revision must belong to its exact thread turn and item");
bound.child.line({ method: "item/fileChange/patchUpdated", params: { threadId: "thread-test", turnId: "turn-test",
  itemId: "item-3", changes: [{ path: "src/example.ts", kind: { type: "update", move_path: null }, diff: "+new revision" }] } });
await driveUntil(boundDisposable, () => boundPatch() !== oldPatch, "replacement patch");
boundWrites = bound.child.writes.length;
assert.throws(() => decideFile("file-1", oldPatch), /patch changed or expired/,
  "changed patch must reject a stale approval before pipe write");
assert.equal(bound.child.writes.length, boundWrites, "a stale approval must not write to Codex");
assert.deepEqual(decideFile("file-1", boundPatch()), { accepted: true });
await driveUntil(boundDisposable, () => bound.child.writes.some(value => value.id === "file-1"), "accepted patch pipe write");
assert.deepEqual(bound.child.writes.find(value => value.id === "file-1"),
  { jsonrpc: "2.0", id: "file-1", result: { decision: "accept" } });
assert.throws(() => decideFile("file-1", boundPatch()), /unknown or expired/);
await fileApproval("file-no-patch", "unavailable-item");
assert.deepEqual(decideFile("file-no-patch", "", "decline"), { accepted: true },
  "decline must remain possible without a patch snapshot");
await fileApproval("file-oversize");
const beforeOversize = boundPatch();
bound.child.line({ method: "item/fileChange/patchUpdated", params: { threadId: "thread-test", turnId: "turn-test",
  itemId: "item-3", changes: [{ path: "large", kind: { type: "add" }, diff: "x".repeat(CODEX_CHANGE_MAX_BYTES) }] } });
await driveUntil(boundDisposable, () => bound.handlers.get("codex.change.read")(
  { snapshotID: beforeOversize, offset: 0 }, { check() {} }).status === "expired", "oversize invalidation");
assert.throws(() => decideFile("file-oversize", beforeOversize, "acceptForSession"), /patch changed or expired/,
  "unavailable patch must reject all accepting decision classes");
boundDisposable.dispose();
for (let i = 0; i < 8; i++) await Promise.resolve();
assert.equal(bound.child.reaped, true);

const revisionStore = new CodexChangeStore();
const revision = revisionStore.capture("item/fileChange/patchUpdated", { threadId: "t", turnId: "u", itemId: "é",
  changes: [{ path: "a", kind: { type: "add" }, diff: "+a" }] })._ghosterm.snapshotID;
assert.equal(revisionStore.isCurrent(revision, "t", "u", "é"), true);
for (const identity of [["other", "u", "é"], ["t", "other", "é"], ["t", "u", "e\u0301"]]) {
  assert.equal(revisionStore.isCurrent(revision, ...identity), false, "approval identity must match exact bytes");
}
revisionStore.clear();
assert.equal(revisionStore.isCurrent(revision, "t", "u", "é"), false);

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
assert.throws(() => fixture.handlers.get("codex.change.read")({ snapshotID, offset: 0 }, { check() {} }),
  /snapshots are unavailable/, "closed Executors must not retain readable snapshot access");

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
