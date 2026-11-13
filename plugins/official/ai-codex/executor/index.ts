import { BoundaryError, boolean, encode, integer, object, text, utf8Bytes, type Call, type Disposable, type PluginContext,
  type RequestContext } from "@ghosterm/plugin-sdk";
import type { ApprovalDecision, RequestID } from "../protocol/client.ts";
import { CodexProtocolError } from "../protocol/client.ts";
import type { CodexConversationEvent } from "../protocol/conversation.ts";
import { spawnCodexAppServer, type CodexAppServerProcess } from "../protocol/process.ts";
import { CODEX_CHANGE_MAX_BYTES, CodexChangeStore } from "./changes.ts";

const empty = object({});
const threadResult = object({ threadID: text(128) });
const messageInput = object({ text: text(2048) });
const turnResult = object({ turnID: text(128) });
const interruptResult = object({ accepted: boolean() });
const approvalInput = object({ requestID: text(140), decisionJSON: text(2048) });
const eventText = text(3072);
const eventAck = object({ accepted: boolean() });
const EVENT_QUEUE_LIMIT = 16;
const changeInput = object({ snapshotID: text(128), offset: integer(0, CODEX_CHANGE_MAX_BYTES) });
const changeResult = object({ status: text(16), snapshotID: text(128), offset: integer(0, CODEX_CHANGE_MAX_BYTES),
  nextOffset: integer(0, CODEX_CHANGE_MAX_BYTES), totalUnits: integer(0, CODEX_CHANGE_MAX_BYTES),
  totalBytes: integer(0, CODEX_CHANGE_MAX_BYTES), content: text(3072), done: boolean() });

function requestKey(id: RequestID): string {
  return typeof id === "string" ? `s:${id}` : `n:${id}`;
}

function boundedPrefix(value: string, maximumBytes: number): string {
  let result = "";
  let bytes = 0;
  for (const character of value) {
    const size = utf8Bytes(character);
    if (bytes + size > maximumBytes) break;
    result += character;
    bytes += size;
  }
  return result;
}

function fitEvent(value: unknown): string | undefined {
  const serialized = JSON.stringify(value);
  if (serialized === undefined) return undefined;
  try {
    // Session.request wraps the event string in another JSON object. Check that
    // exact wire shape so quoting and multibyte text cannot exceed the Core cap.
    encode({ event: serialized });
    return serialized;
  } catch (error) {
    if (error instanceof BoundaryError && error.code === "BODY_LIMIT") return undefined;
    throw error;
  }
}

function boundedFileChangeParams(params: unknown, sequence: number, method: string): string | undefined {
  if (typeof params !== "object" || params === null || Array.isArray(params)) return undefined;
  const record = params as Record<string, unknown>;
  const nestedItem = typeof record.item === "object" && record.item !== null && !Array.isArray(record.item)
    ? record.item as Record<string, unknown> : undefined;
  const changes = Array.isArray(record.changes) ? record.changes :
    (nestedItem && Array.isArray(nestedItem.changes) ? nestedItem.changes : undefined);
  if (!changes) return undefined;
  const location = record.changes === changes ? "root" : "item";
  const diffBudgets = [1200, 800, 400, 0];
  for (let count = Math.min(changes.length, 8); count > 0; count--) {
    for (const diffBudget of diffBudgets) {
      const bounded = changes.slice(0, count).map(change => {
        if (typeof change !== "object" || change === null || Array.isArray(change)) {
          return { value: change, truncated: false };
        }
        const file = change as Record<string, unknown>;
        const originalPath = typeof file.path === "string" ? file.path : "";
        const originalDiff = typeof file.diff === "string" ? file.diff : "";
        const path = originalPath;
        const diff = boundedPrefix(originalDiff, diffBudget);
        const diffTruncated = path !== originalPath || diff !== originalDiff;
        return { value: { path, kind: file.kind,
          diff: diffTruncated && diff ? `${diff}\n…（diff 已截断）` : diff }, truncated: diffTruncated };
      });
      const visible = bounded.map(change => change.value);
      const omitted = changes.length - visible.length;
      const originalMarker = location === "root" ? record._ghosterm : nestedItem?._ghosterm;
      const marker = { ...(typeof originalMarker === "object" && originalMarker !== null ? originalMarker : {}),
        truncated: true, omittedChanges: omitted,
        diffTruncated: bounded.some(change => change.truncated) };
      const limitedParams = location === "root"
        ? { ...record, changes: visible, _ghosterm: marker }
        : { ...record, item: { ...nestedItem, changes: visible, _ghosterm: marker } };
      const payload = fitEvent({ sequence, kind: "notification", notification: { method, params: limitedParams } });
      if (payload) return payload;
    }
  }
  // Long paths or unrelated backend fields must not hide a readable snapshot.
  const originalMarker = location === "root" ? record._ghosterm : nestedItem?._ghosterm;
  const marker = { ...(typeof originalMarker === "object" && originalMarker !== null ? originalMarker : {}),
    truncated: true, omittedChanges: changes.length, diffTruncated: true };
  const identity = { threadId: record.threadId, turnId: record.turnId };
  const minimalParams = location === "root"
    ? { ...identity, itemId: record.itemId, changes: [], _ghosterm: marker }
    : { ...identity, item: { id: nestedItem?.id, type: "fileChange", changes: [], _ghosterm: marker } };
  return fitEvent({ sequence, kind: "notification", notification: { method, params: minimalParams } });
}

function eventPayload(event: CodexConversationEvent, changes: CodexChangeStore): string {
  let value: unknown;
  const notificationParams = event.kind === "notification"
    ? changes.capture(event.notification.method, event.notification.params) : undefined;
  switch (event.kind) {
    case "notification":
      value = { sequence: event.sequence, kind: event.kind,
        notification: { method: event.notification.method, params: notificationParams ?? null } };
      break;
    case "approval-request":
      value = { sequence: event.sequence, kind: event.kind,
        request: { id: event.request.id, method: event.request.method, params: event.request.params } };
      break;
    case "approval-resolved":
      value = { sequence: event.sequence, kind: event.kind, threadID: event.threadID,
        requestID: event.requestID };
      break;
    case "outcome-unknown":
      value = { sequence: event.sequence, kind: event.kind, requestID: event.requestID };
      break;
    case "protocol-error":
      value = { sequence: event.sequence, kind: event.kind,
        message: event.error.message.slice(0, 512) };
      break;
    case "closed":
      value = { sequence: event.sequence, kind: event.kind, outcome: event.outcome };
      break;
  }
  const fit = fitEvent(value);
  if (fit) return fit;
  if (event.kind === "notification" && ["item/fileChange/patchUpdated", "item/started", "item/completed"].includes(event.notification.method)) {
    const bounded = boundedFileChangeParams(notificationParams, event.sequence, event.notification.method);
    if (bounded) return bounded;
  }
  if (event.kind === "notification") {
    const fallback = fitEvent({ sequence: event.sequence, kind: "notification", notification: {
      method: "ghosterm/eventTruncated",
      params: { sourceMethod: event.notification.method, message: "事件内容超过 Host 消息上限，已省略详细内容。" },
    } });
    if (fallback) return fallback;
  }
  throw new CodexProtocolError("Codex event exceeds the Host message limit");
}

/** Official Codex Executor entry. It starts only after the Host admits a thread request. */
export function activate(context: PluginContext): Disposable {
  if (context.side !== "executor" || context.invocation?.command !== "ai codex") {
    throw new CodexProtocolError("Codex requires the ai codex Executor command");
  }

  let child: CodexAppServerProcess | undefined;
  let unsubscribe: (() => void) | undefined;
  let conversation: ReturnType<CodexAppServerProcess["createConversation"]> | undefined;
  let closing = false;
  let closePromise: Promise<void> | undefined;
  let failure: Error | undefined;
  let flight: Call<{ readonly accepted: boolean }> | undefined;
  let startRequest: RequestContext | undefined;
  let activeTurnRequest: RequestContext | undefined;
  let interruptedTurnRequest: RequestContext | undefined;
  let interruptPromise: Promise<void> | undefined;
  const events: string[] = [];
  const approvals = new Map<string, RequestID>();
  const changes = new CodexChangeStore();
  const eventMethod = { name: "codex.event", input: object({ event: eventText }), output: eventAck } as const;

  const close = (): Promise<void> => {
    if (closePromise) return closePromise;
    closing = true;
    unsubscribe?.();
    unsubscribe = undefined;
    events.length = 0;
    approvals.clear();
    changes.clear();
    flight?.cancel();
    const active = conversation;
    conversation = undefined;
    closePromise = (async () => {
      if (active) await active.close();
      else if (child) await child.stop();
      child = undefined;
    })();
    return closePromise;
  };

  const interruptCancelledTurn = (): void => {
    const request = activeTurnRequest;
    if (!request?.cancelled || !conversation?.currentTurnID || interruptedTurnRequest === request) return;
    interruptedTurnRequest = request;
    interruptPromise = conversation.interrupt().catch(error => {
      failure = error instanceof Error ? error : new Error(String(error));
    });
  };

  context.session.register({ name: "codex.thread.start", input: empty, output: threadResult }, async (_input, request) => {
    request.check();
    if (child || conversation || startRequest || closing) throw new CodexProtocolError("Codex thread already started");
    startRequest = request;
    try {
      const process = await spawnCodexAppServer(context, {
        name: "ghosterm", title: "Ghosterm Codex", version: "0.1.0",
      });
      child = process;
      // Cancellation may stop the child before the handler reaches `await ready`.
      void process.ready.catch(() => {});
      if (closing || request.cancelled) {
        await process.stop();
        child = undefined;
        request.check();
      }
      await process.ready;
      request.check();
      conversation = process.createConversation();
      unsubscribe = conversation.onEvent(event => {
        try {
          if (event.kind === "approval-request") {
            approvals.set(requestKey(event.request.id), event.request.id);
            if (approvals.size > 32) throw new CodexProtocolError("too many pending Codex approvals");
          } else if (event.kind === "approval-resolved") {
            approvals.delete(requestKey(event.requestID));
          } else if ((event.kind === "notification" && event.notification.method === "turn/completed") ||
                     event.kind === "protocol-error" || event.kind === "closed" || event.kind === "outcome-unknown") {
            approvals.clear();
          }
          if (events.length >= EVENT_QUEUE_LIMIT) throw new CodexProtocolError("Codex event queue is full");
          events.push(eventPayload(event, changes));
        } catch (error) {
          failure = error instanceof Error ? error : new Error(String(error));
        }
      });
      const id = await conversation.startThread({});
      request.check();
      return { threadID: id };
    } catch (error) {
      try { await close(); } catch { /* retain the activation request failure */ }
      throw error;
    } finally {
      if (startRequest === request) startRequest = undefined;
    }
  });

  context.session.register({ name: "codex.turn.send", input: messageInput, output: turnResult }, async (input, request) => {
    request.check();
    if (!conversation || closing) throw new CodexProtocolError("Codex thread is not ready");
    if (activeTurnRequest) throw new CodexProtocolError("a Codex turn request is already active");
    activeTurnRequest = request;
    try {
      const turnID = await conversation.sendMessage(input.text);
      if (request.cancelled) {
        interruptCancelledTurn();
        if (interruptPromise) await interruptPromise;
      }
      request.check();
      return { turnID };
    } finally {
      if (activeTurnRequest === request) activeTurnRequest = undefined;
      if (interruptedTurnRequest === request) interruptedTurnRequest = undefined;
      interruptPromise = undefined;
    }
  });

  context.session.register({ name: "codex.turn.interrupt", input: empty, output: interruptResult }, async (_input, request) => {
    request.check();
    if (!conversation || closing) throw new CodexProtocolError("Codex thread is not ready");
    await conversation.interrupt();
    request.check();
    return { accepted: true };
  });

  context.session.register({ name: "codex.approval.respond", input: approvalInput, output: interruptResult }, (input, request) => {
    request.check();
    const id = approvals.get(input.requestID);
    if (!conversation || id === undefined) throw new CodexProtocolError("Codex approval is unknown or expired");
    const decision = JSON.parse(input.decisionJSON) as ApprovalDecision;
    conversation.respondApproval(id, decision);
    approvals.delete(input.requestID);
    return { accepted: true };
  });

  context.session.register({ name: "codex.change.read", input: changeInput, output: changeResult }, (input, request) => {
    request.check();
    if (closing || failure || !conversation) throw new CodexProtocolError("Codex change snapshots are unavailable");
    return changes.read(input.snapshotID, input.offset);
  });

  const disposable: Disposable = {
    step(): void {
      if (closing) return;
      if (failure) { void close().catch(() => {}); return; }
      if (startRequest?.cancelled) { void close().catch(() => {}); return; }
      try {
        child?.step();
        if (child?.stdio.failure) throw child.stdio.failure;
        if (child?.exit?.state === "exited") throw new Error("Codex app-server exited");
        interruptCancelledTurn();
      } catch (error) {
        failure = error instanceof Error ? error : new Error(String(error));
        void close().catch(() => {});
        return;
      }
      if (flight || events.length === 0) return;
      const payload = events.shift();
      if (payload === undefined) return;
      try {
        const call = context.session.request(eventMethod, { event: payload }, 30000);
        flight = call;
        void call.outcome.then(outcome => {
          if (flight !== call) return;
          flight = undefined;
          if (outcome.kind !== "success" || !outcome.value.accepted) {
            failure = new Error("Codex event delivery did not receive an acknowledged outcome");
          }
        }, error => {
          if (flight === call) {
            flight = undefined;
            failure = error instanceof Error ? error : new Error(String(error));
          }
        });
      } catch (error) {
        failure = error instanceof Error ? error : new Error(String(error));
      }
    },
    dispose(): void { void close().catch(() => {}); },
  };
  return disposable;
}
