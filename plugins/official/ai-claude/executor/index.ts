import { boolean, integer, object, text, type Disposable, type PluginContext, type RequestContext } from "@ghosterm/plugin-sdk";
import { ClaudeDocuments, PARAMS_MAX, RESPONSE_MAX } from "./documents.ts";
import { ClaudeNodeProcess, ClaudeTransportError, METHODS, spawnClaudeNode } from "../protocol/process.ts";

const id = text(128), status = text(32), size = integer(0, RESPONSE_MAX);
const inputID = object({ inputID: id }), accepted = object({ accepted: boolean() });
const responseInfo = object({ status, responseID: id, totalUnits: size, totalBytes: size });

/** Candidate production bridge. Native chat/question components consume these
 * receipts later; no raw SDK payload is truncated to fit a Core message. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "ai claude") throw new Error("Claude Executor command required");
  const documents = new ClaudeDocuments();
  let child: ClaudeNodeProcess | undefined, attempted = false, disposed = false;
  let active: RequestContext | undefined, method = "";
  let stopPromise: Promise<void> | undefined;
  let startupFailure = "";
  const close = (): Promise<void> => {
    disposed = true; documents.clear();
    return stopPromise ??= child?.stop() ?? Promise.resolve();
  };
  context.session.register({ name: "claude.input.open", input: object({ totalUnits: integer(2, PARAMS_MAX) }),
    output: object({ status, inputID: id, nextOffset: size }) }, (value, request) => {
    request.check(); if (disposed) throw new Error("Claude bridge closed"); return documents.open(value.totalUnits);
  });
  context.session.register({ name: "claude.input.append", input: object({ inputID: id, offset: size, content: text(256) }),
    output: object({ status, nextOffset: size }) }, (value, request) => {
    request.check(); if (disposed) throw new Error("Claude bridge closed"); return documents.append(value.inputID, value.offset, value.content);
  });
  context.session.register({ name: "claude.input.seal", input: inputID, output: object({ status }) }, (value, request) => {
    request.check(); if (disposed) throw new Error("Claude bridge closed"); return documents.seal(value.inputID);
  });
  context.session.register({ name: "claude.input.discard", input: inputID, output: accepted }, (value, request) => {
    request.check(); return documents.discard(value.inputID);
  });
  context.session.register({ name: "claude.call", input: object({ method: text(32), inputID: id }), output: responseInfo }, async (value, request) => {
    request.check();
    const refusal = (status: string) => ({ status, responseID: "", totalUnits: 0, totalBytes: 0 });
    if (disposed) return refusal("closed");
    if (active || child?.busy) return refusal("busy");
    if (!METHODS.some(method => method === value.method)) return refusal("unsupported");
    if (!child && (value.method !== "start" || attempted)) return refusal("unavailable");
    const responseID = documents.reserve(value.inputID);
    if (!responseID) return refusal("limit");
    const parameters = documents.take(value.inputID);
    if (parameters === undefined) { documents.abandon(responseID); return refusal("invalid"); }
    active = request; method = value.method;
    try {
      if (!child) { attempted = true; child = await spawnClaudeNode(context); }
      if (disposed || request.cancelled) { await child.stop(); request.check(); throw new Error("Claude call cancelled before dispatch"); }
      request.check();
      const raw = await child.call(value.method, parameters);
      // Retain an actual helper receipt even if the outer RPC was cancelled.
      // Reading that observation never replays the input or changes old outcomes.
      const receipt = documents.capture(responseID, raw);
      if (value.method === "stop" && child.acceptedStop) {
        const exit = await child.waitForExit();
        if (exit.state !== "exited" || exit.code !== 0 || exit.signal !== null || !child.reaped) throw new Error("Claude stop cleanup not confirmed");
      }
      request.check(); return receipt;
    } catch (error) {
      if (!child) startupFailure = error instanceof ClaudeTransportError ? error.code : "STARTUP";
      documents.abandon(responseID);
      throw error;
    } finally { if (active === request) { active = undefined; method = ""; } }
  });
  context.session.register({ name: "claude.response.read", input: object({ responseID: id, offset: size }),
    output: object({ status, responseID: id, offset: size, nextOffset: size, totalUnits: size, totalBytes: size, content: text(256), done: boolean() }) }, (value, request) => {
    request.check(); return documents.read(value.responseID, value.offset);
  });
  context.session.register({ name: "claude.response.forget", input: object({ responseID: id }), output: accepted }, (value, request) => {
    request.check(); return documents.forget(value.responseID);
  });
  context.session.register({ name: "claude.status", input: object({}),
    output: object({ phase: status, failure: status, exitJSON: text(256), reaped: boolean(), responseID: id, inputID: id }) }, (_value, request) => {
    request.check(); return { phase: disposed ? "disposed" : child?.phase ?? (attempted ? "failed" : "new"),
      failure: child?.failure?.code ?? startupFailure, exitJSON: JSON.stringify(child?.exit ?? null), reaped: child?.reaped ?? false, ...documents.latestResponse };
  });
  return { step() {
    if (disposed) return;
    if (active?.cancelled && method !== "poll") void child?.stop().catch(() => {});
    child?.step();
  }, dispose() { void close().catch(() => {}); } };
}
