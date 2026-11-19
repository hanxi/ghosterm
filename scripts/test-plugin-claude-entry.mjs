import assert from "node:assert/strict";
import test from "node:test";
import { pathToFileURL } from "node:url";
import { encode as wireEncode } from "@ghosterm/plugin-sdk";
const entry = pathToFileURL(process.env.GHOSTERM_CLAUDE_PACKAGE_ENTRY);
const { activate } = await import(entry.href);
const { ClaudeDocuments, RESPONSE_BUDGET } = await import(new URL("documents.js", entry));
const { ClaudeNodeProcess, spawnClaudeNode } = await import(new URL("../protocol/process.js", entry));
const { ClaudeLineDecoder } = await import(new URL("../protocol/line-decoder.js", entry));
const { byteLength } = await import(new URL("../protocol/json.js", entry));
const versions = await import(new URL("../protocol/version.js", entry));

/** Named private-protocol fixture. No SDK/CLI/model or OS process is simulated as real. */
class PipeFixture {
  verifiedVersion = "v26.4.0";
  output = []; writes = []; input = ""; eof = false; released = 0; terminated = 0;
  exit = { state: "running" }; modify = value => JSON.stringify(value); maxWrite = 1024; maxRead = 8192;
  pollEvents = []; decoder = new TextDecoder("utf-8", { fatal: true });
  writeStdin(bytes) {
    const count = Math.min(bytes.length, this.maxWrite);
    this.input += this.decoder.decode(bytes.subarray(0, count), { stream: true });
    for (;;) {
      const newline = this.input.indexOf("\n"); if (newline < 0) break;
      const request = JSON.parse(this.input.slice(0, newline)); this.input = this.input.slice(newline + 1);
      this.writes.push(request);
      let result;
      if (request.method === "start") result = { requested: true, phase: "starting", sdkVersion: versions.CLAUDE_SDK_VERSION, cliVersion: versions.CLAUDE_CLI_VERSION };
      else if (request.method === "poll") result = this.pollEvents.shift() ?? { source: "none", phase: "ready" };
      else if (request.method === "send") result = { messageID: "fixture-message" };
      else if (request.method === "approve" || request.method === "answer") result = { accepted: true };
      else if (request.method === "stop") { result = { phase: "closed", pendingMessageID: null }; this.eof = true; this.exit = { state: "exited", code: 0, signal: null }; }
      else result = { requested: true, phase: "interrupting" };
      this.output.push(new TextEncoder().encode(this.modify({ version: 1, id: request.id, sequence: request.sequence, ok: true, result }) + "\n"));
    }
    return { state: "data", count };
  }
  readStdout(maximum) {
    const chunk = this.output[0];
    if (!chunk) return { state: this.eof ? "eof" : "wait" };
    const bytes = chunk.subarray(0, Math.min(maximum, this.maxRead));
    if (bytes.length === chunk.length) this.output.shift(); else this.output[0] = chunk.subarray(bytes.length);
    return { state: "data", bytes };
  }
  readStderr() { return { state: this.eof ? "eof" : "wait" }; }
  poll() { return this.exit; }
  closeStdin() {}
  signal() { this.terminated++; this.eof = true; this.exit = { state: "exited", code: null, signal: 15 }; }
  async release() { this.released++; this.eof = true; if (this.exit.state === "running") this.exit = { state: "exited", code: null, signal: null }; }
}
function fixture({ allowed = true, child = new PipeFixture(), spawn } = {}) {
  const handlers = new Map(); let count = 0;
  const context = { side: "executor", invocation: { command: "ai claude", workspaceHandle: { id: "workspace" } },
    instanceId: "fixture", epoch: "1", cancellation: { cancelled: false },
    capabilities: { query: () => allowed ? "allowed" : "denied" },
    process: { async spawn(options) {
      count++; assert.deepEqual(options.argv, ["claude-node"]); assert.equal(options.program, "claude-node");
      assert.deepEqual(options.cwd, { id: "workspace" }); assert.equal(options.envPolicy, "selected-user-environment");
      return spawn ? spawn() : child;
    } }, session: { register(method, handler) {
      assert(!handlers.has(method.name)); handlers.set(method.name, async (input, request = { check() {}, cancelled: false }) => {
        request.check(); const result = await handler(method.input.parse(input), request); request.check();
        method.output.parse(result); wireEncode({ kind: "success", value: result }); return result;
      });
    } } };
  const disposable = activate(context);
  return { context, handlers, disposable, child, get spawns() { return count; } };
}
async function pump(promise, step) {
  let done = false, value, failure;
  promise.then(result => { done = true; value = result; }, error => { done = true; failure = error; });
  for (let i = 0; i < 30_000 && !done; i++) { step(); await new Promise(resolve => setImmediate(resolve)); }
  assert(done, "Claude fixture operation must complete within its pump bound");
  if (failure) throw failure; return value;
}
async function upload(value, params) {
  const text = JSON.stringify(params);
  const opened = await value.handlers.get("claude.input.open")({ totalUnits: text.length }); assert.equal(opened.status, "ok");
  for (let offset = 0; offset < text.length;) {
    let end = Math.min(offset + 256, text.length); if (end < text.length && /[\ud800-\udbff]/.test(text[end - 1])) end--;
    const result = await value.handlers.get("claude.input.append")({ inputID: opened.inputID, offset, content: text.slice(offset, end) });
    assert.equal(result.status, "ok"); offset = result.nextOffset;
  }
  assert.equal((await value.handlers.get("claude.input.seal")({ inputID: opened.inputID })).status, "ok");
  return opened.inputID;
}
async function invoke(value, method, params = {}) {
  const inputID = await upload(value, params);
  const result = await pump(value.handlers.get("claude.call")({ inputID, method }), () => value.disposable.step());
  return { ...result, inputID };
}
async function read(value, receipt) {
  assert.equal(receipt.status, "ok"); let text = "", offset = 0;
  for (;;) {
    const part = await value.handlers.get("claude.response.read")({ responseID: receipt.responseID, offset });
    assert.equal(part.status, "ok"); assert.equal(part.totalUnits, receipt.totalUnits); assert.equal(part.totalBytes, receipt.totalBytes);
    assert.equal(part.offset, offset); text += part.content; offset = part.nextOffset;
    if (part.done) break;
    assert(part.nextOffset > part.offset);
  }
  assert.equal(text.length, receipt.totalUnits); assert.equal(new TextEncoder().encode(text).length, receipt.totalBytes);
  assert.equal((await value.handlers.get("claude.response.forget")({ responseID: receipt.responseID })).accepted, true);
  return JSON.parse(text);
}

test("Claude documents require a complete immutable input and preserve full Unicode reply pages", () => {
  const docs = new ClaudeDocuments(), text = JSON.stringify({ text: "中文😀\n".repeat(500) });
  const opened = docs.open(text.length);
  assert.equal(docs.open(2).status, "busy");
  assert.equal(docs.append(opened.inputID, 1, "{}").status, "invalid");
  assert.equal(docs.seal(opened.inputID).status, "invalid");
  for (let offset = 0; offset < text.length;) {
    let end = Math.min(offset + 256, text.length); if (end < text.length && /[\ud800-\udbff]/.test(text[end - 1])) end--;
    offset = docs.append(opened.inputID, offset, text.slice(offset, end)).nextOffset;
  }
  assert.equal(docs.seal(opened.inputID).status, "ok");
  assert.equal(docs.append(opened.inputID, text.length, "x").status, "invalid");
  assert.equal(docs.take(opened.inputID), text);
  assert.equal(docs.take(opened.inputID), undefined, "Claude uploaded parameters must be consumed once");
  const response = docs.reserve(opened.inputID); const meta = docs.capture(response, text);
  let restored = "";
  for (let offset = 0; offset < text.length;) {
    const part = docs.read(response, offset); assert.equal(part.status, "ok"); wireEncode({ kind: "success", value: part });
    restored += part.content; offset = part.nextOffset;
  }
  assert.equal(restored, text, "Claude response pages must preserve the complete Unicode record");
  const low = text.indexOf("😀") + 1; assert.equal(docs.read(response, low).status, "invalid");
  assert.equal(meta.totalBytes, byteLength(text)); assert(docs.forget(response).accepted); assert.equal(docs.reservedBytes, 0);
});
test("Claude reply reservations enforce capacity before a child side effect", () => {
  const docs = new ClaudeDocuments();
  const ids = [docs.reserve("one"), docs.reserve("two"), docs.reserve("three")];
  assert(ids.every(Boolean)); assert.equal(docs.reserve("four"), undefined, "Claude response reservation must precede an overflowing effect");
  assert.equal(docs.forget(ids[0]).accepted, false);
  docs.capture(ids[0], "{}"); assert(docs.reserve("four")); assert.equal(docs.reserve("five"), undefined);
  assert(docs.reservedBytes <= RESPONSE_BUDGET); docs.clear(); assert.equal(docs.reservedBytes, 0);
});
test("Claude transport handles partial Unicode writes and binds every response to both identifiers", async () => {
  const child = new PipeFixture(); child.maxWrite = 3; child.maxRead = 7;
  const client = new ClaudeNodeProcess(child);
  const parameters = JSON.stringify({ text: "quoted \\\"\n中文😀".repeat(300) });
  try {
    const response = JSON.parse(await pump(client.call("send", parameters), () => client.step()));
    assert.equal(response.result.messageID, "fixture-message"); assert.equal(child.writes[0].params.text, JSON.parse(parameters).text);
    assert.equal(child.writes.length, 1, "Claude transport must not replay a request while making partial progress");
    child.modify = value => JSON.stringify({ ...value, id: "foreign" });
    await assert.rejects(pump(client.call("poll", "{}"), () => client.step()), /RESPONSE_IDENTITY/,
      "Claude response identity mismatch must never be accepted");
    assert.equal(client.failure.code, "RESPONSE_IDENTITY");
  } finally { await client.stop(); }
});
test("Claude transport refuses malformed responses and preserves unknown on partial EOF", async () => {
  for (const mode of ["sequence", "version", "duplicate", "backend-version", "partial"]) {
    const child = new PipeFixture(), client = new ClaudeNodeProcess(child);
    child.modify = value => {
      if (mode === "sequence") return JSON.stringify({ ...value, sequence: 9 });
      if (mode === "version") return JSON.stringify({ ...value, version: 2 });
      if (mode === "duplicate") return JSON.stringify(value).replace('"ok":true', '"ok":false,"ok":true');
      if (mode === "backend-version") return JSON.stringify({ ...value, result: { ...value.result, sdkVersion: "unknown" } });
      return JSON.stringify(value).slice(0, -1);
    };
    if (mode === "partial") {
      child.modify = () => { child.eof = true; return '{"partial":'; };
      const original = child.writeStdin.bind(child);
      child.writeStdin = bytes => { const result = original(bytes); if (child.eof && child.output[0]) child.output[0] = child.output[0].subarray(0, child.output[0].length - 1); return result; };
    }
    try { await assert.rejects(pump(client.call("start", "{}"), () => client.step())); assert(client.failure); }
    finally { await client.stop(); }
  }
});
test("Claude decoder preserves chunk boundaries cumulative limits and strict UTF8", () => {
  const text = JSON.stringify({ text: "😀中\n".repeat(4000) }), bytes = new TextEncoder().encode(text + "\n");
  for (const width of [1, 2, 3, 7, 8191, 8192, 16384]) {
    const lines = [], decoder = new ClaudeLineDecoder(value => lines.push(value));
    for (let at = 0; at < bytes.length; at += width) decoder.push(bytes.subarray(at, at + width));
    decoder.finish(); assert.deepEqual(lines, [text]);
  }
  assert.throws(() => new ClaudeLineDecoder(() => {}).push(new Uint8Array([0xf0, 0x80, 0x80, 0x80, 10])));
  const partial = new ClaudeLineDecoder(() => {}); partial.push(new Uint8Array([123])); assert.throws(() => partial.finish());
  assert.throws(() => new ClaudeLineDecoder(() => {}).push(new Uint8Array(600_001)), undefined,
    "Claude response byte limit must survive decoder flushes");
});
test("Claude production bridge is lazy and transfers full inputs and response documents", async () => {
  const value = fixture(); assert.equal(value.spawns, 0);
  try {
    const start = await read(value, await invoke(value, "start")); assert.equal(start.result.requested, true);
    const parameters = { text: "long user input 中文😀\n".repeat(500) };
    const receipt = await invoke(value, "send", parameters);
    assert.equal(value.child.writes.at(-1).params.text, parameters.text, "Claude uploaded request must reach the child without truncation");
    assert.equal((await read(value, receipt)).result.messageID, "fixture-message");
    const stale = await value.handlers.get("claude.call")({ method: "send", inputID: receipt.inputID });
    assert.equal(stale.status, "invalid", "Claude consumed input must never authorize another action");
    const payload = { source: "approval", event: { kind: "requested", request: {
      reference: { scope: "fixture", requestID: "审批😀", token: "opaque-fixture" }, input: { command: "show me 中文😀\n".repeat(1500) } } } };
    value.child.pollEvents.push(payload);
    assert.deepEqual((await read(value, await invoke(value, "poll"))).result, payload);
    const answers = { reference: payload.event.request.reference,
      answers: Object.fromEntries(Array.from({ length: 4 }, (_, i) => [`question-${i}-${"问😀".repeat(80)}`, "回答😀".repeat(80)])) };
    await read(value, await invoke(value, "answer", answers)); assert.deepEqual(value.child.writes.at(-1).params, answers);
    await read(value, await invoke(value, "stop"));
    const state = await value.handlers.get("claude.status")({}); assert(state.reaped); assert.equal(state.phase, "closed");
    assert.equal(value.spawns, 1);
  } finally { value.disposable.dispose(); }
});
test("Claude bridge checks cancellation before queued effects and never relaunches implicitly", async () => {
  let releaseSpawn, cancelled = false;
  const value = fixture({ spawn: () => new Promise(resolve => { releaseSpawn = resolve; }) });
  const inputID = await upload(value, {}), request = { get cancelled() { return cancelled; }, check() { if (cancelled) throw Error("cancelled fixture request"); } };
  const call = value.handlers.get("claude.call")({ method: "start", inputID }, request);
  await new Promise(resolve => setImmediate(resolve)); cancelled = true; releaseSpawn(value.child);
  try {
    await assert.rejects(pump(call, () => value.disposable.step()));
    assert.equal(value.child.writes.length, 0, "Claude cancelled spawn handoff must not send a helper action");
    assert.equal(value.child.released, 1); assert.equal(value.spawns, 1);
  } finally { value.disposable.dispose(); }
});
test("Claude version and grant checks reject unsupported Node before helper actions", async () => {
  for (const version of ["v18.14.0", "v19.9.9", "20.0.0", "v999999999999999999999.0.0"]) {
    const value = fixture(); value.child.verifiedVersion = version;
    try {
      await assert.rejects(spawnClaudeNode(value.context), /NODE_VERSION/);
      assert.equal(value.child.writes.length, 0); assert.equal(value.child.released, 1);
    } finally { value.disposable.dispose(); }
  }
  const denied = fixture({ allowed: false });
  await assert.rejects(spawnClaudeNode(denied.context), /UNSUPPORTED/); assert.equal(denied.spawns, 0); denied.disposable.dispose();
});

test("Claude releases an unsupported Node child even if termination signaling fails", async () => {
  const child = new PipeFixture();
  child.verifiedVersion = "v18.14.0";
  child.signal = () => { throw Error("fixture signal failure"); };
  const value = fixture({ child });
  try {
    await assert.rejects(spawnClaudeNode(value.context).finally(() => {
      assert.equal(child.released, 1, "unsupported Node child must be released");
    }), /NODE_VERSION/);
    assert.equal(child.writes.length, 0);
    assert.equal(value.spawns, 1);
  } finally { value.disposable.dispose(); }
});
